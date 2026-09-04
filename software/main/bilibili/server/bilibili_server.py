import os
import socket
import struct
import subprocess
import threading
import time
import hashlib
import random
import re
import urllib.parse
from difflib import SequenceMatcher
from collections import deque
from pathlib import Path

import requests
from flask import Flask, Response, jsonify, request

app = Flask(__name__)
SERVER_HOST = "0.0.0.0"
SERVER_PORT = 8000
VIDEO_TCP_PORT = 9101
AUDIO_TCP_PORT = 9102
DISCOVERY_PORT = 39876
DISCOVERY_MAGIC = "VOCAT_BILI_DISCOVER 1"
FFMPEG_EXE = os.environ.get("FFMPEG_EXE", "ffmpeg")
BILI_HOME = "https://www.bilibili.com/"
BILI_API = "https://api.bilibili.com"
VIDEO_W = 320
VIDEO_H = 176
VIDEO_FPS = 10
AUDIO_RATE = 24000
AUDIO_BYTES = 4800
MAX_FRAME = 16 * 1024

TCP_MAGIC = 0x56434154  # VCAT
TCP_VERSION = 2
MSG_HELLO = 1
MSG_DATA = 2
MSG_ACK = 3
MSG_STOP = 4
STREAM_VIDEO = 1
STREAM_AUDIO = 2
HEADER = struct.Struct("!IHHIQII")
VIDEO_WINDOW = 7
AUDIO_WINDOW = 14
VIDEO_PREFETCH = 24
AUDIO_PREFETCH = 28

RANK_CACHE_TTL = 60
VIDEO_INFO_CACHE_TTL = 300
PLAY_URL_CACHE_TTL = 90
BILI_COOLDOWN_SECONDS = 30
CACHE_FILE = Path(__file__).with_name("bili_cache.json")

session = requests.Session()
session.headers.update({
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151 Safari/537.36",
    "Referer": BILI_HOME,
    "Origin": "https://www.bilibili.com",
    "Accept": "*/*",
    "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
    "Sec-Fetch-Site": "same-site",
    "Sec-Fetch-Mode": "cors",
    "Sec-Fetch-Dest": "empty",
})

state_lock = threading.Lock()
media_url_lock = threading.Lock()
rank_cache = {"timestamp": 0.0, "list": []}
video_info_cache = {}
media_url_cache = {}
bili_block_until = 0.0


# --- WBI / visitor identity -------------------------------------------------
wbi_lock = threading.Lock()
wbi_img_key = ""
wbi_sub_key = ""
wbi_refresh_at = 0.0
visitor_ready = False

WBI_MIXIN_KEY_ENC_TAB = [
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52,
]

SEARCH_CACHE_TTL = 300
user_video_cache = {}
verified_owner_cache = {}


def _clean_html_text(value):
    value = value or ""
    value = re.sub(r"<[^>]+>", "", value)
    return value.replace("&amp;", "&").strip()


def _norm_name(value):
    value = _clean_html_text(value).lower()
    return re.sub(r"[\s\W_]+", "", value, flags=re.UNICODE)


def ensure_visitor_cookie(force=False):
    global visitor_ready
    if visitor_ready and not force:
        return
    try:
        resp = session.get(
            BILI_API + "/x/frontend/finger/spi",
            timeout=8,
        )
        resp.raise_for_status()
        data = resp.json().get("data") or {}
        b3 = data.get("b_3") or ""
        b4 = data.get("b_4") or ""
        if b3:
            session.cookies.set("buvid3", b3, domain=".bilibili.com")
        if b4:
            session.cookies.set("buvid4", b4, domain=".bilibili.com")
        visitor_ready = True
        log(f"[VISITOR] ready buvid3={'yes' if b3 else 'no'} buvid4={'yes' if b4 else 'no'}")
    except Exception as exc:
        visitor_ready = False
        log(f"[VISITOR] init failed: {type(exc).__name__}: {exc}")


def get_wbi_keys(force=False):
    global wbi_img_key, wbi_sub_key, wbi_refresh_at
    with wbi_lock:
        if not force and wbi_img_key and wbi_sub_key and time.time() - wbi_refresh_at < 6 * 3600:
            return wbi_img_key, wbi_sub_key
        ensure_visitor_cookie(force=False)
        resp = session.get(BILI_API + "/x/web-interface/nav", timeout=8)
        resp.raise_for_status()
        data = (resp.json().get("data") or {}).get("wbi_img") or {}
        img_url = data.get("img_url") or ""
        sub_url = data.get("sub_url") or ""
        if not img_url or not sub_url:
            raise RuntimeError("WBI keys missing")
        wbi_img_key = img_url.rsplit("/",1)[-1].split(".",1)[0]
        wbi_sub_key = sub_url.rsplit("/",1)[-1].split(".",1)[0]
        wbi_refresh_at = time.time()
        log(f"[WBI] keys refreshed img={wbi_img_key[:8]}... sub={wbi_sub_key[:8]}...")
        return wbi_img_key, wbi_sub_key


def sign_wbi(params):
    img_key, sub_key = get_wbi_keys()
    raw_key = img_key + sub_key
    mixin_key = "".join(raw_key[i] for i in WBI_MIXIN_KEY_ENC_TAB)[:32]
    signed = dict(params)
    signed["wts"] = int(time.time())
    clean = {}
    for key in sorted(signed):
        clean[key] = re.sub(r"[!'()*]", "", str(signed[key]))
    query = urllib.parse.urlencode(clean, quote_via=urllib.parse.quote, safe="")
    clean["w_rid"] = hashlib.md5((query + mixin_key).encode("utf-8")).hexdigest()
    return clean


def wbi_request(path, params, retries=3):
    last_exc = None
    url = BILI_API.rstrip("/") + "/" + path.lstrip("/")
    for attempt in range(retries):
        try:
            ensure_visitor_cookie(force=(attempt > 0))
            signed = sign_wbi(params)
            response = session.get(
                url,
                params=signed,
                headers={
                    "User-Agent": session.headers["User-Agent"],
                    "Referer": f"https://space.bilibili.com/{params.get('mid', '')}",
                    "Origin": "https://space.bilibili.com",
                    "Accept": "application/json, text/plain, */*",
                },
                timeout=12,
            )
            if response.status_code == 412:
                log(f"[WBI] HTTP 412 attempt={attempt+1}/{retries}; refresh visitor/WBI")
                get_wbi_keys(force=True)
                time.sleep(0.8 * (attempt + 1))
                continue
            response.raise_for_status()
            payload = response.json()
            if payload.get("code") == -352:
                log(f"[WBI] code -352 attempt={attempt+1}/{retries}")
                get_wbi_keys(force=True)
                time.sleep(1.2 * (attempt + 1))
                continue
            if payload.get("code") != 0:
                raise RuntimeError(f"WBI code={payload.get('code')} message={payload.get('message')}")
            return payload
        except (requests.ConnectionError, requests.Timeout, requests.HTTPError, ValueError) as exc:
            last_exc = exc
            log(f"[WBI] request failed attempt={attempt+1}/{retries}: {type(exc).__name__}: {exc}")
            if attempt + 1 < retries:
                if isinstance(exc, requests.HTTPError) and getattr(exc.response, "status_code", None) == 412:
                    ensure_visitor_cookie(force=True)
                    get_wbi_keys(force=True)
                time.sleep(0.8 * (attempt + 1))
    if last_exc:
        raise last_exc
    raise RuntimeError("WBI request failed")


def log(msg):
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}", flush=True)


def load_disk_cache():
    try:
        import json
        if CACHE_FILE.exists():
            data = json.loads(CACHE_FILE.read_text(encoding="utf-8"))
            with state_lock:
                rank_cache["list"] = data.get("list", [])[:4]
                rank_cache["timestamp"] = data.get("timestamp", 0.0)
            log(f"[CACHE] load items={len(rank_cache['list'])}")
    except Exception as exc:
        log(f"[CACHE] load failed: {exc}")


def save_disk_cache(items):
    try:
        import json
        CACHE_FILE.write_text(json.dumps({"timestamp": time.time(), "list": items}, ensure_ascii=False), encoding="utf-8")
    except Exception as exc:
        log(f"[CACHE] save failed: {exc}")


def bili_in_cooldown():
    with state_lock:
        return time.time() < bili_block_until


def trigger_bili_cooldown():
    global bili_block_until
    with state_lock:
        bili_block_until = time.time() + BILI_COOLDOWN_SECONDS


def warmup_bilibili():
    try:
        r = session.get(BILI_HOME, timeout=10)
        log(f"[WARMUP] bilibili status={r.status_code}")
    except Exception as exc:
        log(f"[WARMUP] home failed: {type(exc).__name__}: {exc}")
    try:
        ensure_visitor_cookie(force=True)
    except Exception as exc:
        log(f"[WARMUP] visitor failed: {type(exc).__name__}: {exc}")
    try:
        get_wbi_keys(force=True)
    except Exception as exc:
        log(f"[WARMUP] WBI failed: {type(exc).__name__}: {exc}")


def bili_get(path, *, params=None, timeout=10, retries=3):
    if bili_in_cooldown():
        raise RuntimeError("B站接口冷却中")
    url = BILI_API.rstrip("/") + "/" + path.lstrip("/")
    last_exc = None
    for attempt in range(1, retries + 1):
        try:
            ensure_visitor_cookie(force=attempt > 1)
            response = session.get(url, params=params, timeout=timeout, headers={"Referer": BILI_HOME, "Origin": "https://www.bilibili.com"})
            if response.status_code in (412, 429, 503):
                log(f"[HTTP] status={response.status_code} attempt={attempt}/{retries} path={path}")
                if attempt < retries:
                    time.sleep(0.6 * attempt)
                    continue
            response.raise_for_status()
            data = response.json()
            if data.get("code") == -352:
                if attempt < retries:
                    get_wbi_keys(force=True)
                    time.sleep(0.8 * attempt)
                    continue
                trigger_bili_cooldown()
                raise RuntimeError("B站接口风控 -352")
            if data.get("code") != 0:
                raise RuntimeError(f"B站接口失败 code={data.get('code')} message={data.get('message')}")
            return data
        except (requests.ConnectionError, requests.Timeout, requests.HTTPError, ValueError) as exc:
            last_exc = exc
            log(f"[HTTP] {path} attempt={attempt}/{retries} {type(exc).__name__}: {exc}")
            if attempt < retries:
                time.sleep(0.6 * attempt)
    raise last_exc or RuntimeError("B站 HTTP request failed")


def fetch_bili_popular(force=False):
    now = time.time()
    with state_lock:
        cached = list(rank_cache["list"])
        ts = rank_cache["timestamp"]
    if not force and cached and now - ts < RANK_CACHE_TTL:
        log(f"[LIST] cache hit age={now-ts:.1f}s count={len(cached)}")
        return cached[:4]
    if bili_in_cooldown():
        return cached[:4]
    try:
        data = bili_get("/x/web-interface/popular", params={"ps": 4, "pn": 1}, timeout=10)
        result = []
        for item in data.get("data", {}).get("list", [])[:4]:
            stat = item.get("stat", {})
            result.append({
                "title": item.get("title", ""),
                "bvid": item.get("bvid", ""),
                "play": int(stat.get("view", stat.get("vv", 0)) or 0),
                "pic": item.get("pic", ""),
            })
        if not result:
            raise RuntimeError("popular list empty")
        with state_lock:
            rank_cache["timestamp"] = now
            rank_cache["list"] = result
        save_disk_cache(result)
        log(f"[LIST] refreshed count={len(result)}")
        return result
    except Exception as exc:
        log(f"[BILI] popular failed: {exc}")
        return cached[:4]


# --- UP主搜索 ---------------------------------------------------------------
# 需求：用户说"搜索 [UP主名]" -> 返回该UP主发布的视频列表。
# 流程：B站搜索API定位UP主(mid) -> 拉取该UP主最近发布的视频。
def select_best_user(query, users):
    q = _norm_name(query)
    best = None
    best_score = -1.0
    for user in users:
        uname = _clean_html_text(user.get("uname") or user.get("name") or "")
        if not uname:
            continue
        n = _norm_name(uname)
        if n == q:
            score = 1.0
        elif q in n or n in q:
            score = 0.95
        else:
            score = SequenceMatcher(None, q, n).ratio()
        if score > best_score:
            best_score = score
            best = user, uname, score
    if best:
        user, uname, score = best
        log(f"[SEARCH][USER] query={query!r} uname={uname!r} mid={user.get('mid') or user.get('userid')} score={score:.3f}")
    return best


def get_verified_owner_mid(bvid):
    now = time.time()
    cached = verified_owner_cache.get(bvid)
    if cached and now - cached["timestamp"] < VIDEO_INFO_CACHE_TTL:
        return cached["mid"]

    data = bili_get(
        "/x/web-interface/view",
        params={"bvid": bvid},
        timeout=8,
        retries=2,
    )
    owner = data.get("data", {}).get("owner") or {}
    owner_mid = owner.get("mid")
    verified_owner_cache[bvid] = {
        "timestamp": now,
        "mid": str(owner_mid) if owner_mid is not None else "",
    }
    return verified_owner_cache[bvid]["mid"]


def search_videos_fallback_for_user(uname, mid, limit=20):
    """Fallback for 412 on /space/wbi/arc/search.

    Do not trust a search result's displayed uploader name. Verify every
    selected BVID against /x/web-interface/view and require owner.mid == mid.
    """
    found = []
    seen = set()

    for page in range(1, 6):
        data = bili_get(
            "/x/web-interface/search/type",
            params={
                "search_type": "video",
                "keyword": uname,
                "page": page,
                "order": "pubdate",
            },
            timeout=10,
        )

        items = data.get("data", {}).get("result") or []
        log(
            f"[SEARCH][FALLBACK] page={page} candidates={len(items)}"
        )

        if not items:
            break

        for item in items:
            bvid = item.get("bvid") or ""
            if not bvid or bvid in seen:
                continue

            seen.add(bvid)

            try:
                owner_mid = get_verified_owner_mid(bvid)
            except Exception as exc:
                log(
                    f"[SEARCH][VERIFY] bvid={bvid} failed "
                    f"type={type(exc).__name__}: {exc}"
                )
                continue

            if owner_mid != str(mid):
                continue

            result = {
                "title": _clean_html_text(item.get("title")),
                "bvid": bvid,
                "play": int(
                    item.get("play")
                    or item.get("view")
                    or 0
                ),
                "pic": item.get("pic") or "",
                "up": uname,
            }

            found.append(result)

            if len(found) >= limit:
                return found

    return found


def search_up_videos(name, limit=20):
    keyword = (name or "").strip()
    if not keyword:
        return []
    cache_key = _norm_name(keyword)
    with state_lock:
        cached = user_video_cache.get(cache_key)
    if cached and time.time() - cached["timestamp"] < SEARCH_CACHE_TTL:
        log(f"[SEARCH][CACHE] query={keyword!r} count={len(cached['list'])}")
        return cached["list"][:limit]
    try:
        log(f"[SEARCH][START] query={keyword!r} limit={limit}")
        user_data = bili_get("/x/web-interface/search/type", params={
            "search_type": "bili_user",
            "keyword": keyword,
            "page": 1,
        }, timeout=10)
        users = user_data.get("data", {}).get("result") or []
        log(f"[SEARCH][USER] candidates={len(users)}")
        selected = select_best_user(keyword, users)
        if not selected:
            log(f"[SEARCH][FAIL] no user match query={keyword!r}")
            return []
        user, uname, score = selected
        mid = user.get("mid") or user.get("userid")
        if not mid:
            log(f"[SEARCH][FAIL] matched user has no mid")
            return []
        params = {
            "mid": int(mid),
            "ps": min(int(limit), 50),
            "pn": 1,
            "tid": 0,
            "keyword": "",
            "order": "pubdate",
            "platform": "web",
            "web_location": 1550101,
            "order_avoided": "true",
            "dm_img_list": "[]",
            "dm_img_str": "",
            "dm_cover_img_str": "",
        }
        try:
            log(f"[SEARCH][ARC] uname={uname!r} mid={mid}")
            archive = wbi_request("/x/space/wbi/arc/search", params, retries=3)
            vlist = archive.get("data", {}).get("list", {}).get("vlist") or []
            result = []
            for item in vlist:
                bvid = item.get("bvid") or ""
                if not bvid:
                    continue
                stat = item.get("stat") or {}
                result.append({
                    "title": _clean_html_text(item.get("title")),
                    "bvid": bvid,
                    "play": int(item.get("play") or stat.get("view") or 0),
                    "pic": item.get("pic") or "",
                    "up": uname,
                })
                if len(result) >= limit:
                    break
            if result:
                log(f"[SEARCH][DONE] WBI uname={uname!r} videos={len(result)}")
                with state_lock:
                    user_video_cache[cache_key] = {"timestamp": time.time(), "list": result}
                return result
            log(f"[SEARCH][ARC] WBI returned empty; fallback video search")
        except Exception as exc:
            log(f"[SEARCH][ARC] failed type={type(exc).__name__}: {exc}; fallback video search")

        result = search_videos_fallback_for_user(uname, mid, limit=limit)
        log(f"[SEARCH][DONE] FALLBACK uname={uname!r} videos={len(result)}")
        if result:
            with state_lock:
                user_video_cache[cache_key] = {
                    "timestamp": time.time(),
                    "list": result,
                }
        return result
    except Exception as exc:
        log(f"[SEARCH][FAIL] query={keyword!r} type={type(exc).__name__}: {exc}")
        return []


def get_video_info(bvid):
    now = time.time()
    with state_lock:
        item = video_info_cache.get(bvid)
    if item and now - item["timestamp"] < VIDEO_INFO_CACHE_TTL:
        return item["data"]
    data = bili_get("/x/web-interface/view", params={"bvid": bvid}, timeout=10).get("data")
    if not data:
        raise RuntimeError("video info empty")
    with state_lock:
        video_info_cache[bvid] = {"timestamp": now, "data": data}
    return data


def get_bili_media_urls(bvid):
    now = time.time()
    with media_url_lock:
        with state_lock:
            item = media_url_cache.get(bvid)
        if item and now - item["timestamp"] < PLAY_URL_CACHE_TTL:
            log(f"[PLAY] cache hit bvid={bvid} age={now-item['timestamp']:.1f}s")
            return item["video"], item["audio"]

        pages = get_video_info(bvid).get("pages", [])
        if not pages or not pages[0].get("cid"):
            raise RuntimeError("cid empty")
        cid = pages[0]["cid"]
        data = bili_get("/x/player/playurl", params={
            "bvid": bvid, "cid": cid, "fnval": 16, "fnver": 0, "fourk": 0, "qn": 16,
        }, timeout=15).get("data", {})
        dash = data.get("dash") or {}
        videos = list(dash.get("video") or [])
        audios = list(dash.get("audio") or [])
        if not videos or not audios:
            raise RuntimeError("DASH media empty")
        # Prefer the smallest H.264-capable track; ffmpeg handles conversion.
        videos.sort(key=lambda x: (
            int(x.get("width") or 9999), int(x.get("height") or 9999), int(x.get("bandwidth") or 999999999)
        ))
        audios.sort(key=lambda x: int(x.get("bandwidth") or 999999999))
        video = videos[0]
        audio = audios[0]
        vu = video.get("baseUrl") or video.get("base_url")
        au = audio.get("baseUrl") or audio.get("base_url")
        if not vu or not au:
            raise RuntimeError("media URL empty")
        log(f"[PLAY] {bvid} source={video.get('width')}x{video.get('height')} video_bw={video.get('bandwidth')} audio_bw={audio.get('bandwidth')}")
        with state_lock:
            media_url_cache[bvid] = {"timestamp": now, "video": vu, "audio": au}
        return vu, au


def ffmpeg_headers():
    return "User-Agent: " + session.headers["User-Agent"] + "\r\nReferer: " + BILI_HOME + "\r\n"


def kill_process(proc):
    if not proc:
        return
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:
        pass


def drain_stderr(proc, name):
    def worker():
        try:
            for raw in iter(proc.stderr.readline, b""):
                text = raw.decode("utf-8", errors="replace").strip()
                if text:
                    log(f"[FFMPEG][{name}] {text}")
        except Exception as exc:
            log(f"[FFMPEG][{name}] stderr reader stopped: {exc}")
    threading.Thread(target=worker, name=f"ffmpeg-{name}-stderr", daemon=True).start()


def start_video_ffmpeg(bvid):
    vu, _ = get_bili_media_urls(bvid)
    cmd = [
        FFMPEG_EXE, "-hide_banner", "-loglevel", "warning",
        "-headers", ffmpeg_headers(),
        "-re", "-i", vu,
        "-an",
        "-vf", f"fps={VIDEO_FPS},scale={VIDEO_W}:{VIDEO_H}:force_original_aspect_ratio=decrease,pad={VIDEO_W}:{VIDEO_H}:(ow-iw)/2:(oh-ih)/2,setsar=1",
        "-q:v", "7", "-f", "mjpeg", "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    drain_stderr(proc, f"video:{bvid}")
    log(f"[VIDEO] ffmpeg start pid={proc.pid} bvid={bvid} out={VIDEO_W}x{VIDEO_H}@{VIDEO_FPS}")
    return proc


def start_audio_ffmpeg(bvid):
    _, au = get_bili_media_urls(bvid)
    cmd = [
        FFMPEG_EXE, "-hide_banner", "-loglevel", "warning",
        "-headers", ffmpeg_headers(),
        "-reconnect", "1",
        "-reconnect_streamed", "1",
        "-reconnect_delay_max", "5",
        # Do not use FFmpeg -re for audio. The source is already a network
        # stream and -re throttles demuxing to timestamp pace; during source
        # jitter this turns short upstream stalls into progressively larger
        # output starvation. The ESP32/HTTP layer provides the playback queue.
        "-i", au,
        "-vn", "-af", "aresample=async=1:first_pts=0",
        "-ac", "1", "-ar", str(AUDIO_RATE),
        "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    drain_stderr(proc, f"audio:{bvid}")
    log(f"[AUDIO] ffmpeg start pid={proc.pid} bvid={bvid} pcm=24k/mono chunk={AUDIO_BYTES}")
    return proc


def extract_jpegs(proc, bvid, stop_evt, q):
    buf = bytearray()
    frames = 0
    bytes_total = 0
    try:
        while not stop_evt.is_set():
            t0 = time.perf_counter()
            chunk = proc.stdout.read(32 * 1024)
            read_ms = (time.perf_counter() - t0) * 1000.0
            if read_ms > 250:
                log(f"[VIDEO-PC] ffmpeg.read block={read_ms:.1f}ms frame={frames}")
            if not chunk:
                if proc.poll() is not None:
                    log(f"[VIDEO-PC] ffmpeg EOF rc={proc.poll()} frames={frames}")
                    break
                time.sleep(0.005)
                continue
            buf.extend(chunk)
            while not stop_evt.is_set():
                soi = buf.find(b"\xff\xd8")
                if soi < 0:
                    if len(buf) > 64 * 1024:
                        del buf[:-4096]
                    break
                eoi = buf.find(b"\xff\xd9", soi + 2)
                if eoi < 0:
                    if soi > 0:
                        del buf[:soi]
                    break
                frame = bytes(buf[soi:eoi + 2])
                del buf[:eoi + 2]
                if len(frame) > MAX_FRAME:
                    log(f"[VIDEO-PC] drop oversized jpeg={len(frame)}")
                    continue
                frames += 1
                bytes_total += len(frame)
                q.put(frame)
                if frames <= 3 or frames % 20 == 0:
                    log(f"[VIDEO-PC] produced frame={frames} jpeg={len(frame)} prefetch={q.qsize()}")
    finally:
        log(f"[VIDEO-PC] producer end bvid={bvid} frames={frames} bytes={bytes_total}")


def audio_producer(proc, bvid, stop_evt, q):
    pending = bytearray()
    chunks = 0
    try:
        while not stop_evt.is_set():
            t0 = time.perf_counter()
            raw = proc.stdout.read(32 * 1024)
            read_ms = (time.perf_counter() - t0) * 1000.0
            if read_ms > 250:
                log(f"[AUDIO-PC] ffmpeg.read block={read_ms:.1f}ms chunk={chunks}")
            if not raw:
                if proc.poll() is not None:
                    log(f"[AUDIO-PC] ffmpeg EOF rc={proc.poll()} chunks={chunks}")
                    break
                time.sleep(0.005)
                continue
            pending.extend(raw)
            while len(pending) >= AUDIO_BYTES and not stop_evt.is_set():
                q.put(bytes(pending[:AUDIO_BYTES]))
                del pending[:AUDIO_BYTES]
                chunks += 1
                if chunks <= 3 or chunks % 20 == 0:
                    log(f"[AUDIO-PC] produced chunk={chunks} prefetch={q.qsize()}")
    finally:
        log(f"[AUDIO-PC] producer end bvid={bvid} chunks={chunks}")


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        data = sock.recv(n - len(buf))
        if not data:
            raise ConnectionError("peer closed")
        buf.extend(data)
    return bytes(buf)


def send_all_timed(sock, data, label):
    t0 = time.perf_counter()
    sock.sendall(data)
    dt = (time.perf_counter() - t0) * 1000.0
    if dt > 250:
        log(f"[TCP][{label}] send block={dt:.1f}ms bytes={len(data)}")
    return dt


def recv_header(sock):
    raw = recv_exact(sock, HEADER.size)
    return HEADER.unpack(raw)


def send_header(sock, msg_type, stream, seq, pts_us, payload_len=0, aux=0):
    send_all_timed(sock, HEADER.pack(TCP_MAGIC, TCP_VERSION, msg_type, seq, pts_us, payload_len, aux), "HEADER")


def recv_hello(sock, expected_stream):
    magic, version, msg_type, seq, pts, length, aux = recv_header(sock)
    if magic != TCP_MAGIC or version != TCP_VERSION or msg_type != MSG_HELLO or aux != expected_stream or length > 128:
        raise ConnectionError("invalid hello")
    bvid = recv_exact(sock, length).decode("utf-8", errors="strict").strip()
    if not bvid.startswith("BV"):
        raise ConnectionError("invalid bvid")
    return bvid


class CreditChannel:
    def __init__(self, conn, window, name):
        self.conn = conn
        self.name = name
        self.credits = window
        self.cv = threading.Condition()
        self.stop = False
        self.pending = {}
        self.ack_count = 0
        self.rtt_last = 0.0
        self.rtt_max = 0.0
        self.reader = threading.Thread(target=self._reader, name=f"ack-{name}", daemon=True)
        self.reader.start()

    def _reader(self):
        try:
            while True:
                magic, version, typ, seq, pts, length, aux = recv_header(self.conn)
                if magic != TCP_MAGIC or version != TCP_VERSION:
                    raise ConnectionError("invalid control header")
                if typ == MSG_ACK:
                    now = time.perf_counter()
                    with self.cv:
                        self.credits += max(1, int(aux))
                        sent_at = self.pending.pop(seq, None)
                        rtt = (now - sent_at) * 1000.0 if sent_at is not None else -1.0
                        self.rtt_last = rtt
                        self.rtt_max = max(self.rtt_max, rtt)
                        self.ack_count += 1
                        self.cv.notify_all()
                    if self.ack_count <= 3 or self.ack_count % 20 == 0 or rtt > 500:
                        log(f"[{self.name}-ACK] seq={seq} credit+={max(1, int(aux))} credits={self.credits} rtt={rtt:.1f}ms")
                elif typ == MSG_STOP:
                    raise ConnectionError("client stop")
                else:
                    log(f"[{self.name}-CTRL] unexpected type={typ}")
        except Exception as exc:
            with self.cv:
                self.stop = True
                self.cv.notify_all()
            log(f"[{self.name}-CTRL] exit: {exc}")

    def take_credit(self):
        with self.cv:
            while self.credits <= 0 and not self.stop:
                self.cv.wait(timeout=1.0)
            if self.stop:
                raise ConnectionError("ack channel stopped")
            self.credits -= 1

    def mark_sent(self, seq):
        with self.cv:
            self.pending[seq] = time.perf_counter()

    def close(self):
        with self.cv:
            self.stop = True
            self.cv.notify_all()


def run_video_tcp(conn, addr):
    proc = None
    producer = None
    stop_evt = threading.Event()
    frames = 0
    total_bytes = 0
    q = None
    channel = None
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        conn.settimeout(4.0)
        bvid = recv_hello(conn, STREAM_VIDEO)
        log(f"[VIDEO-TCP] start {addr} bvid={bvid} window={VIDEO_WINDOW} prefetch={VIDEO_PREFETCH}")
        proc = start_video_ffmpeg(bvid)
        q = deque(maxlen=VIDEO_PREFETCH)
        cond = threading.Condition()

        def producer_fn():
            local = bytearray()
            try:
                while not stop_evt.is_set():
                    t0 = time.perf_counter()
                    chunk = proc.stdout.read(32 * 1024)
                    read_ms = (time.perf_counter() - t0) * 1000.0
                    if read_ms > 250:
                        log(f"[VIDEO-PC] stdout wait={read_ms:.1f}ms frames={frames}")
                    if not chunk:
                        if proc.poll() is not None:
                            break
                        time.sleep(0.005); continue
                    local.extend(chunk)
                    while not stop_evt.is_set():
                        soi = local.find(b"\xff\xd8")
                        if soi < 0:
                            if len(local) > 64 * 1024: del local[:-4096]
                            break
                        eoi = local.find(b"\xff\xd9", soi + 2)
                        if eoi < 0:
                            if soi > 0: del local[:soi]
                            break
                        frame = bytes(local[soi:eoi+2]); del local[:eoi+2]
                        if len(frame) > MAX_FRAME: continue
                        with cond:
                            while len(q) >= VIDEO_PREFETCH and not stop_evt.is_set():
                                cond.wait(timeout=0.1)
                            if stop_evt.is_set(): break
                            q.append(frame)
                            cond.notify_all()
            finally:
                stop_evt.set()
                with cond: cond.notify_all()

        producer = threading.Thread(target=producer_fn, name=f"video-producer-{addr[1]}", daemon=True)
        producer.start()
        channel = CreditChannel(conn, VIDEO_WINDOW, "VIDEO")

        while not stop_evt.is_set():
            channel.take_credit()
            with cond:
                while not q and not stop_evt.is_set():
                    cond.wait(timeout=0.2)
                if stop_evt.is_set() and not q: break
                frame = q.popleft()
                cond.notify_all()
            frames += 1
            total_bytes += len(frame)
            pts_us = (frames - 1) * 100_000
            channel.mark_sent(frames)
            send_header(conn, MSG_DATA, STREAM_VIDEO, frames, pts_us, len(frame), STREAM_VIDEO)
            send_all_timed(conn, frame, "VIDEO_PAYLOAD")
            if frames <= 3 or frames % 20 == 0:
                log(f"[VIDEO-TX] seq={frames} jpeg={len(frame)} q={len(q)} credits={channel.credits} rtt={channel.rtt_last:.1f}ms")

    except (ConnectionError, BrokenPipeError, OSError, socket.timeout) as exc:
        log(f"[VIDEO-TCP] disconnected {addr}: {exc}")
    except Exception as exc:
        log(f"[VIDEO-TCP] error {addr}: {exc}")
    finally:
        stop_evt.set()
        if channel: channel.close()
        kill_process(proc)
        try: conn.shutdown(socket.SHUT_RDWR)
        except Exception: pass
        try: conn.close()
        except Exception: pass
        log(f"[VIDEO-TCP] exit bvid={locals().get('bvid','?')} frames={frames} bytes={total_bytes} ack={channel.ack_count if channel else 0} rtt_max={channel.rtt_max if channel else 0:.1f}ms")


def run_audio_tcp(conn, addr):
    proc = None
    stop_evt = threading.Event()
    chunks = 0
    total_bytes = 0
    q = None
    channel = None
    producer = None
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        conn.settimeout(4.0)
        bvid = recv_hello(conn, STREAM_AUDIO)
        log(f"[AUDIO-TCP] start {addr} bvid={bvid} window={AUDIO_WINDOW} prefetch={AUDIO_PREFETCH}")
        proc = start_audio_ffmpeg(bvid)
        q = deque(maxlen=AUDIO_PREFETCH)
        cond = threading.Condition()

        def producer_fn():
            pending = bytearray()
            try:
                while not stop_evt.is_set():
                    t0 = time.perf_counter()
                    raw = proc.stdout.read(32 * 1024)
                    read_ms = (time.perf_counter() - t0) * 1000.0
                    if read_ms > 250:
                        log(f"[AUDIO-PC] stdout wait={read_ms:.1f}ms chunks={chunks}")
                    if not raw:
                        if proc.poll() is not None: break
                        time.sleep(0.005); continue
                    pending.extend(raw)
                    while len(pending) >= AUDIO_BYTES and not stop_evt.is_set():
                        item = bytes(pending[:AUDIO_BYTES]); del pending[:AUDIO_BYTES]
                        with cond:
                            while len(q) >= AUDIO_PREFETCH and not stop_evt.is_set():
                                cond.wait(timeout=0.1)
                            if stop_evt.is_set(): break
                            q.append(item)
                            cond.notify_all()
            finally:
                stop_evt.set()
                with cond: cond.notify_all()

        producer = threading.Thread(target=producer_fn, name=f"audio-producer-{addr[1]}", daemon=True)
        producer.start()
        channel = CreditChannel(conn, AUDIO_WINDOW, "AUDIO")

        while not stop_evt.is_set():
            channel.take_credit()
            with cond:
                while not q and not stop_evt.is_set():
                    cond.wait(timeout=0.2)
                if stop_evt.is_set() and not q: break
                chunk = q.popleft()
                cond.notify_all()
            chunks += 1
            total_bytes += len(chunk)
            pts_us = (chunks - 1) * 100_000
            channel.mark_sent(chunks)
            send_header(conn, MSG_DATA, STREAM_AUDIO, chunks, pts_us, len(chunk), STREAM_AUDIO)
            send_all_timed(conn, chunk, "AUDIO_PAYLOAD")
            if chunks <= 3 or chunks % 20 == 0:
                log(f"[AUDIO-TX] seq={chunks} q={len(q)} credits={channel.credits} rtt={channel.rtt_last:.1f}ms")
    except (ConnectionError, BrokenPipeError, OSError, socket.timeout) as exc:
        log(f"[AUDIO-TCP] disconnected {addr}: {exc}")
    except Exception as exc:
        log(f"[AUDIO-TCP] error {addr}: {exc}")
    finally:
        stop_evt.set()
        if channel: channel.close()
        kill_process(proc)
        try: conn.shutdown(socket.SHUT_RDWR)
        except Exception: pass
        try: conn.close()
        except Exception: pass
        log(f"[AUDIO-TCP] exit bvid={locals().get('bvid','?')} chunks={chunks} bytes={total_bytes} ack={channel.ack_count if channel else 0} rtt_max={channel.rtt_max if channel else 0:.1f}ms")


def tcp_accept_loop(host, port, handler, name):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(4)
    log(f"[{name}] TCP listen {host}:{port}")
    while True:
        conn, addr = server.accept()
        log(f"[{name}] accept {addr[0]}:{addr[1]}")
        threading.Thread(target=handler, args=(conn, addr), daemon=True, name=f"{name}-{addr[1]}").start()


def get_lan_ip():
    candidates = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            candidates.append(s.getsockname()[0])
        finally:
            s.close()
    except Exception:
        pass
    try:
        candidates.append(socket.gethostbyname(socket.gethostname()))
    except Exception:
        pass
    for ip in candidates:
        if ip and not ip.startswith("127."):
            return ip
    return "127.0.0.1"


def discovery_worker():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", DISCOVERY_PORT))
    log(f"[DISCOVERY] UDP ready 0.0.0.0:{DISCOVERY_PORT}")
    while True:
        try:
            data, addr = sock.recvfrom(128)
            if data.decode("utf-8", errors="ignore").strip() != DISCOVERY_MAGIC:
                continue
            ip = get_lan_ip()
            reply = f"VOCAT_BILI_SERVER 1 {ip} {SERVER_PORT}".encode("ascii")
            sock.sendto(reply, addr)
            log(f"[DISCOVERY] reply to {addr[0]}:{addr[1]} -> {reply.decode()}")
        except Exception as exc:
            log(f"[DISCOVERY] error: {exc}")
            time.sleep(0.2)


@app.route("/bili")
def api_bili():
    name = request.args.get("up_name", "").strip()
    if name:
        result = search_up_videos(name, limit=20)
        log(f"[HTTP][SEARCH] /bili up_name={name!r} videos={len(result)}")
        return jsonify({"list": result})
    return jsonify({"list": fetch_bili_popular(force=False)})


@app.route("/bili/search")
def api_bili_search():
    name = request.args.get("name", "").strip()
    result = search_up_videos(name, limit=20)
    log(f"[HTTP][SEARCH] /bili/search name={name!r} videos={len(result)}")
    return jsonify({"list": result})


@app.route("/bili/health")
def api_bili_health():
    return jsonify({
        "ok": True,
        "service": "vocat-bilibili",
        "search": "/bili?up_name=...",
        "audio": "/bili/audio_stream?bvid=...",
        "visitor_ready": visitor_ready,
        "wbi_ready": bool(wbi_img_key and wbi_sub_key),
        "server_time": int(time.time()),
    })


# Media stays TCP. These HTTP endpoints are diagnostics only.
@app.route("/bili/video")
def api_bili_video():
    return Response("TCP_MEDIA_ONLY", status=409, mimetype="text/plain")


# HTTP 音频流：GET /bili/audio_stream?bvid=BVxxx
# 复用 start_audio_ffmpeg，以 chunked 流式返回 24kHz/mono/s16le PCM。
# 固件用 esp_http_client 拉流 -> PushPcmToPlaybackQueue 直接播放。
# 与 TCP 音频流(9102)的 PCM 格式完全一致，仅传输方式不同。
@app.route("/bili/audio_stream")
def api_bili_audio_stream():
    bvid = request.args.get("bvid", "").strip()
    if not bvid or not bvid.startswith("BV"):
        log(f"[ASTREAM] reject bad bvid={bvid!r}")
        return Response("bad bvid", status=400, mimetype="text/plain")

    request_started = time.monotonic()
    log(f"[ASTREAM] request bvid={bvid} client={request.remote_addr}:{request.environ.get('REMOTE_PORT', '?')}")

    try:
        proc = start_audio_ffmpeg(bvid)
    except Exception as exc:
        log(
            f"[ASTREAM] start failed bvid={bvid} "
            f"type={type(exc).__name__} error={exc}"
        )
        return Response(
            "media unavailable",
            status=503,
            mimetype="text/plain"
        )

    # Do not let Flask delay the first HTTP body until FFmpeg has produced
    # several frames. Pre-buffer exactly 3 x 100 ms PCM blocks, then switch
    # to the generator. This also gives the ESP32 HTTP client a deterministic
    # startup point.
    first = bytearray()
    first_deadline = time.monotonic() + 8.0

    try:
        while len(first) < AUDIO_BYTES * 3:
            if time.monotonic() >= first_deadline:
                rc = proc.poll()
                log(
                    f"[ASTREAM] first-data timeout bvid={bvid} "
                    f"bytes={len(first)} rc={rc}"
                )
                kill_process(proc)
                return Response(
                    "audio startup timeout",
                    status=503,
                    mimetype="text/plain"
                )

            chunk = proc.stdout.read(
                AUDIO_BYTES * 3 - len(first)
            )

            if not chunk:
                rc = proc.poll()
                if rc is not None:
                    log(
                        f"[ASTREAM] ffmpeg EOF before first data "
                        f"bvid={bvid} rc={rc} bytes={len(first)}"
                    )
                    kill_process(proc)
                    return Response(
                        "audio decode failed",
                        status=503,
                        mimetype="text/plain"
                    )
                time.sleep(0.01)
                continue

            first.extend(chunk)

        log(
            f"[ASTREAM] first-data ready bvid={bvid} "
            f"bytes={len(first)}"
        )

    except Exception as exc:
        kill_process(proc)
        log(
            f"[ASTREAM] prebuffer failed bvid={bvid} "
            f"type={type(exc).__name__} error={exc}"
        )
        return Response(
            "audio startup failed",
            status=503,
            mimetype="text/plain"
        )

    def gen():
        sent = 0
        chunks = 0
        last_log = time.monotonic()
        try:
            # Send the deterministic startup buffer first.
            offset = 0
            while offset < len(first):
                end = min(
                    offset + AUDIO_BYTES,
                    len(first)
                )
                payload = bytes(first[offset:end])
                offset = end
                sent += len(payload)
                chunks += 1
                yield payload

            # AudioService consumes the PCM at the hardware playback rate.
            # FFmpeg therefore runs uncapped and the ESP32-side queue provides
            # the small amount of pacing/buffering needed for playback.
            while True:
                chunk = proc.stdout.read(AUDIO_BYTES)
                if not chunk:
                    rc = proc.poll()
                    if rc is not None:
                        log(
                            f"[ASTREAM] ffmpeg EOF bvid={bvid} "
                            f"rc={rc} sent={sent}"
                        )
                        break
                    time.sleep(0.01)
                    continue

                sent += len(chunk)
                chunks += 1
                now = time.monotonic()
                if now - last_log >= 10.0:
                    log(
                        f"[ASTREAM] progress bvid={bvid} sent={sent} chunks={chunks} "
                        f"elapsed_s={now - request_started:.1f} ffmpeg_rc={proc.poll()}"
                    )
                    last_log = now
                yield chunk

        except GeneratorExit:
            log(
                f"[ASTREAM] client disconnected bvid={bvid} "
                f"sent={sent} chunks={chunks} elapsed_s={time.monotonic() - request_started:.1f}"
            )
        except (ConnectionError, BrokenPipeError, OSError) as exc:
            log(
                f"[ASTREAM] client connection lost bvid={bvid} "
                f"error={exc} sent={sent} chunks={chunks} elapsed_s={time.monotonic() - request_started:.1f}"
            )
        except Exception as exc:
            log(
                f"[ASTREAM] generator error bvid={bvid} "
                f"type={type(exc).__name__} error={exc}"
            )
        finally:
            kill_process(proc)
            log(
                f"[ASTREAM] done bvid={bvid} "
                f"sent={sent} chunks={chunks} rc={proc.poll()} elapsed_s={time.monotonic() - request_started:.1f}"
            )

    return Response(
        gen(),
        mimetype="application/octet-stream",
        headers={
            "Cache-Control": "no-store",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )


if __name__ == "__main__":
    load_disk_cache()
    warmup_bilibili()
    threading.Thread(target=discovery_worker, name="bili-discovery", daemon=True).start()
    threading.Thread(target=tcp_accept_loop, args=("0.0.0.0", VIDEO_TCP_PORT, run_video_tcp, "VIDEO"), daemon=True).start()
    threading.Thread(target=tcp_accept_loop, args=("0.0.0.0", AUDIO_TCP_PORT, run_audio_tcp, "AUDIO"), daemon=True).start()
    log(f"server start http={SERVER_HOST}:{SERVER_PORT} lan_ip={get_lan_ip()} media={VIDEO_TCP_PORT}/{AUDIO_TCP_PORT}")
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True, debug=False)
