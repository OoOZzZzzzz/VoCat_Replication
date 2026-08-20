from flask import Flask, Response, request, jsonify
import json
import os
import threading
import time
import subprocess
from pathlib import Path

import requests

app = Flask(__name__)

# ============================================================
# 基础配置
# ============================================================

SERVER_HOST = "0.0.0.0"
SERVER_PORT = 8000

# 你的 FFmpeg 实际路径：D 盘
FFMPEG_EXE = r"D:\ffmpeg\bin\ffmpeg.exe"

# B站
BILI_HOME = "https://www.bilibili.com/"
BILI_API = "https://api.bilibili.com"

# 热门榜缓存 120 秒
RANK_CACHE_TTL = 120

# B站触发 -352 后至少冷却 90 秒
BILI_COOLDOWN_SECONDS = 90

# 视频详情/播放地址缓存 10 分钟
VIDEO_INFO_CACHE_TTL = 600
PLAY_URL_CACHE_TTL = 600

# 本地持久化缓存，防止 Flask 重启后热门列表立即消失
CACHE_FILE = Path(__file__).with_name("bili_cache.json")

# 可选：从环境变量读取 B站 Cookie
# 不要把真实 Cookie 写死进源码并提交到仓库。
BILI_COOKIE = os.environ.get("BILI_COOKIE", "").strip()

# ============================================================
# Session
# ============================================================

session = requests.Session()

session.headers.update({
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/151.0.0.0 Safari/537.36"
    ),
    "Referer": BILI_HOME,
    "Origin": BILI_HOME.rstrip("/"),
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "zh-CN,zh;q=0.9",
    "Connection": "keep-alive",
})

if BILI_COOKIE:
    session.headers["Cookie"] = BILI_COOKIE

# ============================================================
# 缓存/状态
# ============================================================

state_lock = threading.RLock()
media_url_lock = threading.Lock()

rank_cache = {
    "timestamp": 0.0,
    "list": [],
}

video_info_cache = {}
play_url_cache = {}
media_url_cache = {}

bili_block_until = 0.0


# ============================================================
# 日志
# ============================================================

def log(msg):
    print(
        time.strftime("[%Y-%m-%d %H:%M:%S]") + " " + msg,
        flush=True,
    )


# ============================================================
# 本地缓存
# ============================================================

def load_disk_cache():
    global rank_cache

    try:
        if not CACHE_FILE.exists():
            return

        data = json.loads(
            CACHE_FILE.read_text(
                encoding="utf-8"
            )
        )

        cached_list = data.get("list", [])

        if isinstance(cached_list, list) and cached_list:
            rank_cache = {
                "timestamp": float(
                    data.get("timestamp", 0)
                ),
                "list": cached_list[:4],
            }

            log(
                f"[CACHE] loaded {len(rank_cache['list'])} "
                f"videos from disk"
            )

    except Exception as e:
        log(
            f"[CACHE] load failed: {e}"
        )


def save_disk_cache(video_list):
    try:
        payload = {
            "timestamp": time.time(),
            "list": video_list[:4],
        }

        CACHE_FILE.write_text(
            json.dumps(
                payload,
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )

    except Exception as e:
        log(
            f"[CACHE] save failed: {e}"
        )


# ============================================================
# B站冷却
# ============================================================

def bili_in_cooldown():
    with state_lock:
        return time.time() < bili_block_until


def trigger_bili_cooldown():
    global bili_block_until

    with state_lock:
        bili_block_until = (
            time.time()
            + BILI_COOLDOWN_SECONDS
        )

    log(
        f"[BILI] -352 cooldown "
        f"{BILI_COOLDOWN_SECONDS}s"
    )


# ============================================================
# 启动预热
#
# 先访问 bilibili.com，让 Session 获得正常站点上下文。
# ============================================================

def warmup_bilibili():
    try:
        r = session.get(
            BILI_HOME,
            timeout=10,
        )

        log(
            f"[WARMUP] bilibili.com "
            f"status={r.status_code}"
        )

    except Exception as e:
        log(
            f"[WARMUP] failed: {e}"
        )


# ============================================================
# B站 API GET
# ============================================================

def bili_get(
    path,
    *,
    params=None,
    timeout=10,
):
    if bili_in_cooldown():
        raise RuntimeError(
            "B站接口正在冷却"
        )

    url = (
        BILI_API.rstrip("/")
        + "/"
        + path.lstrip("/")
    )

    response = session.get(
        url,
        params=params,
        timeout=timeout,
    )

    response.raise_for_status()

    data = response.json()

    code = data.get("code")

    if code == -352:
        trigger_bili_cooldown()

        raise RuntimeError(
            "B站风控 -352"
        )

    if code != 0:
        raise RuntimeError(
            f"B站接口失败 "
            f"code={code} "
            f"message={data.get('message')}"
        )

    return data


# ============================================================
# 获取综合热门
#
# 使用 popular，而不是 ranking/v2。
# 只在缓存过期时请求 B站。
# ============================================================

def fetch_bili_popular(force=False):

    now = time.time()

    with state_lock:
        cached_list = list(
            rank_cache["list"]
        )
        cached_time = rank_cache["timestamp"]

    if (
        not force
        and cached_list
        and now - cached_time < RANK_CACHE_TTL
    ):
        log(
            "[CACHE] use memory popular cache"
        )

        return cached_list[:4]

    if bili_in_cooldown():

        log(
            "[CACHE] B站 cooldown -> "
            "return last cache"
        )

        return cached_list[:4]

    try:

        data = bili_get(
            "/x/web-interface/popular",
            params={
                "ps": 4,
                "pn": 1,
            },
            timeout=10,
        )

        items = (
            data
            .get("data", {})
            .get("list", [])
        )

        result = []

        for item in items[:4]:

            stat = item.get(
                "stat",
                {}
            )

            play = stat.get(
                "view",
                stat.get("vv", 0)
            )

            result.append({
                "title": item.get(
                    "title",
                    ""
                ),
                "bvid": item.get(
                    "bvid",
                    ""
                ),
                "play": int(play or 0),
                "pic": item.get(
                    "pic",
                    ""
                ),
            })

        if not result:
            raise RuntimeError(
                "popular list empty"
            )

        with state_lock:
            rank_cache["timestamp"] = now
            rank_cache["list"] = result

        save_disk_cache(result)

        log(
            f"[BILI] popular refreshed "
            f"count={len(result)}"
        )

        return result

    except Exception as e:

        log(
            f"[BILI] popular failed: {e}"
        )

        # 关键：失败时返回旧缓存，而不是让 ESP32 得到空列表
        return cached_list[:4]


# ============================================================
# /bili
# ============================================================

@app.route("/bili")
def api_bili():

    result = fetch_bili_popular(
        force=False
    )

    return jsonify({
        "list": result
    })


# ============================================================
# 视频详情
# ============================================================

def get_video_info(bvid):

    now = time.time()

    with state_lock:
        item = video_info_cache.get(
            bvid
        )

    if item:
        timestamp = item["timestamp"]

        if (
            now - timestamp
            < VIDEO_INFO_CACHE_TTL
        ):
            log(
                f"[CACHE] video info "
                f"{bvid}"
            )

            return item["data"]

    data = bili_get(
        "/x/web-interface/view",
        params={
            "bvid": bvid,
        },
        timeout=10,
    ).get("data")

    if not data:
        raise RuntimeError(
            "video info empty"
        )

    with state_lock:
        video_info_cache[bvid] = {
            "timestamp": now,
            "data": data,
        }

    return data


# ============================================================
# 获取 CID
# ============================================================

def get_video_cid(bvid):

    data = get_video_info(
        bvid
    )

    pages = data.get(
        "pages",
        []
    )

    if not pages:
        raise RuntimeError(
            "pages empty"
        )

    cid = pages[0].get(
        "cid"
    )

    if not cid:
        raise RuntimeError(
            "cid empty"
        )

    log(
        f"[PLAY] bvid={bvid} cid={cid}"
    )

    return cid


# ============================================================
# 获取播放地址
# ============================================================

def get_bili_media_urls(bvid):
    """Resolve video/audio DASH URLs exactly once per cache window.

    The video and audio HTTP endpoints are concurrent Flask requests. A lock
    prevents them from racing and making two /x/player/playurl requests, which
    previously delayed audio startup and could produce two different CDN URLs.
    """
    now = time.time()

    with media_url_lock:
        with state_lock:
            item = media_url_cache.get(bvid)

        if item and now - item["timestamp"] < PLAY_URL_CACHE_TTL:
            log(f"[CACHE] media urls {bvid}")
            return item["video"], item["audio"]

        cid = get_video_cid(bvid)

        data = bili_get(
            "/x/player/playurl",
            params={
                "bvid": bvid,
                "cid": cid,
                "fnval": 16,
                "fnver": 0,
                "fourk": 0,
                "qn": 16,
            },
            timeout=15,
        ).get("data", {})

        dash = data.get("dash") or {}
        videos = dash.get("video") or []
        audios = dash.get("audio") or []

        if not videos:
            raise RuntimeError("DASH video empty")
        if not audios:
            raise RuntimeError("DASH audio empty")

        videos = sorted(
            videos,
            key=lambda x: (
                x.get("width", 9999),
                x.get("height", 9999),
                x.get("bandwidth", 999999999),
            ),
        )
        audios = sorted(
            audios,
            key=lambda x: x.get("bandwidth", 999999999),
        )

        video = videos[0]
        audio = audios[0]

        video_url = video.get("baseUrl") or video.get("base_url")
        audio_url = audio.get("baseUrl") or audio.get("base_url")

        if not video_url:
            raise RuntimeError("video baseUrl empty")
        if not audio_url:
            raise RuntimeError("audio baseUrl empty")

        log(
            f"[PLAY] source selected: "
            f"{video.get('width')}x{video.get('height')} "
            f"audio_bandwidth={audio.get('bandwidth')}"
        )

        with state_lock:
            media_url_cache[bvid] = {
                "timestamp": now,
                "video": video_url,
                "audio": audio_url,
            }
            play_url_cache[bvid] = {
                "timestamp": now,
                "url": video_url,
            }

        return video_url, audio_url

def get_bili_play_url(bvid):
    return get_bili_media_urls(bvid)[0]


def get_bili_audio_url(bvid):
    return get_bili_media_urls(bvid)[1]


# ============================================================
# JPEG/MJPEG 生成器
# ============================================================

# def mjpeg_generator(bvid):

#     ffmpeg_process = None

#     try:

#         if not os.path.isfile(
#             FFMPEG_EXE
#         ):
#             raise RuntimeError(
#                 "FFmpeg not found: "
#                 + FFMPEG_EXE
#             )

#         source_url = get_bili_play_url(
#             bvid
#         )

#         log(
#             f"[PLAY] {bvid} "
#             "source url acquired"
#         )

#         ffmpeg_cmd = [
#             FFMPEG_EXE,

#             "-hide_banner",
#             "-loglevel",
#             "error",

#             "-headers",
#             (
#                 "User-Agent: "
#                 + session.headers["User-Agent"]
#                 + "\r\n"
#                 "Referer: "
#                 + BILI_HOME
#                 + "\r\n"
#             ),

#             # Read the DASH source at its native wall-clock rate. Without -re
#             # FFmpeg can burst frames into the pipe, making the ESP32 appear to
#             # play too fast and then stall.
#             "-re",
#             "-i",
#             source_url,

#             # Fixed 10 FPS output for the ESP32 player.
#             "-vf",
#             (
#                 "fps=6,"
#                 "scale=320:176:"
#                 "force_original_aspect_ratio=decrease,"
#                 "pad=320:176:"
#                 "(320-iw)/2:"
#                 "(176-ih)/2"
#             ),

#             # JPEG质量
#             "-q:v",
#             "12",

#             "-f",
#             "mjpeg",

#             "pipe:1",
#         ]

#         log(
#             "[PLAY] starting FFmpeg"
#         )

#         ffmpeg_process = subprocess.Popen(
#             ffmpeg_cmd,
#             stdout=subprocess.PIPE,
#             stderr=subprocess.DEVNULL,
#             bufsize=0,
#         )

#         buffer = bytearray()
#         frame_count = 0

#         while True:

#             chunk = (
#                 ffmpeg_process.stdout.read(
#                     16 * 1024
#                 )
#             )

#             if not chunk:
#                 break

#             buffer.extend(chunk)

#             while True:

#                 start = buffer.find(
#                     b"\xff\xd8"
#                 )

#                 if start < 0:

#                     if len(buffer) > 1024 * 1024:
#                         buffer.clear()

#                     break

#                 end = buffer.find(
#                     b"\xff\xd9",
#                     start + 2,
#                 )

#                 if end < 0:

#                     if start > 0:
#                         del buffer[:start]

#                     break

#                 jpeg = bytes(
#                     buffer[
#                         start:end + 2
#                     ]
#                 )

#                 del buffer[
#                     :end + 2
#                 ]

#                 frame_count += 1

#                 if frame_count <= 3:
#                     log(
#                         f"[PLAY] JPEG "
#                         f"{len(jpeg)} bytes"
#                     )
#                 elif frame_count % 30 == 0:
#                     log(
#                         f"[PLAY] frames="
#                         f"{frame_count}"
#                     )

#                 yield jpeg

#     except GeneratorExit:

#         log(
#             f"[PLAY] {bvid} "
#             "client disconnected"
#         )

#     except Exception as e:

#         log(
#             f"[PLAY] {bvid} "
#             f"exception: {e}"
#         )

#     finally:

#         if ffmpeg_process is not None:

#             try:

#                 if (
#                     ffmpeg_process.poll()
#                     is None
#                 ):
#                     log(
#                         "[PLAY] terminate FFmpeg"
#                     )

#                     ffmpeg_process.terminate()

#                     try:
#                         ffmpeg_process.wait(
#                             timeout=2
#                         )

#                     except subprocess.TimeoutExpired:
#                         ffmpeg_process.kill()

#             except Exception as e:

#                 log(
#                     f"[PLAY] cleanup error: {e}"
#                 )
def mjpeg_generator(bvid):
    ffmpeg_process = None
    frame_count = 0
    last_log_time = time.time()
    read_blocks = 0

    try:
        if not os.path.isfile(FFMPEG_EXE):
            raise RuntimeError("FFmpeg not found: " + FFMPEG_EXE)

        source_url = get_bili_play_url(bvid)
        log(f"[PLAY] {bvid} source url acquired")

        ffmpeg_cmd = [
            FFMPEG_EXE,
            "-hide_banner",
            "-loglevel", "error",
            "-headers",
            (
                "User-Agent: " + session.headers["User-Agent"] + "\r\n"
                "Referer: " + BILI_HOME + "\r\n"
            ),
            "-re",
            "-i", source_url,
            "-vf",
            (
                "fps=10,"
                "scale=320:176:"
                "force_original_aspect_ratio=decrease,"
                "pad=320:176:"
                "(320-iw)/2:"
                "(176-ih)/2"
            ),
            "-q:v", "7",
            "-f", "mjpeg",
            "pipe:1",
        ]

        log("[PLAY] starting FFmpeg")
        ffmpeg_process = subprocess.Popen(
            ffmpeg_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )

        buffer = bytearray()
        frame_count = 0

        while True:
            # ---- 新增：记录 read() 耗时 ----
            read_start = time.time()
            chunk = ffmpeg_process.stdout.read(16 * 1024)
            read_end = time.time()
            read_elapsed = read_end - read_start
            if read_elapsed > 0.2:
                print(f"[PC Perf] ffmpeg.stdout.read blocked {read_elapsed:.3f}s")

            # 检查进程状态
            if ffmpeg_process.poll() is not None:
                print(f"[PC Error] FFmpeg process exited with code {ffmpeg_process.returncode}")
                break

            if not chunk:
                if ffmpeg_process.poll() is None:
                    # 可能暂时无数据，等待一下
                    time.sleep(0.01)
                    continue
                else:
                    break

            buffer.extend(chunk)
            read_blocks += 1
            if read_blocks % 10 == 0:
                print(f"[PC Perf] read blocks={read_blocks}, buffer size={len(buffer)}")

            while True:
                start = buffer.find(b"\xff\xd8")
                if start < 0:
                    if len(buffer) > 1024 * 1024:
                        buffer.clear()
                    break

                end = buffer.find(b"\xff\xd9", start + 2)
                if end < 0:
                    if start > 0:
                        del buffer[:start]
                    break

                jpeg = bytes(buffer[start:end + 2])
                del buffer[:end + 2]

                frame_count += 1
                now = time.time()

                # ---- 新增：每5帧打印帧间隔 ----
                if frame_count % 5 == 0:
                    interval = now - last_log_time
                    print(f"[PC Perf] Frame {frame_count}, interval since last log: {interval:.3f}s (avg {interval/5:.3f}s/frame)")
                    last_log_time = now

                # ---- 新增：记录 yield 阻塞 ----
                yield_start = time.time()
                yield jpeg
                yield_end = time.time()
                yield_elapsed = yield_end - yield_start
                if yield_elapsed > 0.1:
                    print(f"[PC Perf] yield jpeg blocked {yield_elapsed:.3f}s (client may be slow)")

    except GeneratorExit:
        log(f"[PLAY] {bvid} client disconnected")
    except Exception as e:
        log(f"[PLAY] {bvid} exception: {e}")
    finally:
        if ffmpeg_process is not None:
            try:
                if ffmpeg_process.poll() is None:
                    log("[PLAY] terminate FFmpeg")
                    ffmpeg_process.terminate()
                    try:
                        ffmpeg_process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        ffmpeg_process.kill()
            except Exception as e:
                log(f"[PLAY] cleanup error: {e}")

# ============================================================
# PCM audio generator
# ============================================================

# def pcm_audio_generator(bvid):
#     ffmpeg_process = None

#     try:
#         if not os.path.isfile(FFMPEG_EXE):
#             raise RuntimeError("FFmpeg not found: " + FFMPEG_EXE)

#         source_url = get_bili_audio_url(bvid)
#         log(f"[AUDIO] {bvid} source url acquired")

#         ffmpeg_cmd = [
#             FFMPEG_EXE,
#             "-hide_banner",
#             "-loglevel", "error",
#             "-headers",
#             (
#                 "User-Agent: " + session.headers["User-Agent"] + "\r\n"
#                 "Referer: " + BILI_HOME + "\r\n"
#             ),
#             "-re",
#             "-i", source_url,
#             "-vn",
#             "-af", "aresample=async=1:first_pts=0",
#             "-ac", "1",
#             "-ar", "24000",
#             "-c:a", "pcm_s16le",
#             "-f", "s16le",
#             "-flush_packets", "1",
#             "pipe:1",
#         ]

#         log("[AUDIO] starting FFmpeg")
#         ffmpeg_process = subprocess.Popen(
#             ffmpeg_cmd,
#             stdout=subprocess.PIPE,
#             stderr=subprocess.DEVNULL,
#             bufsize=0,
#         )

#         chunk_count = 0
#         pending = bytearray()
#         target_bytes = 2400 * 2  # 100 ms @ 24 kHz / 16-bit / mono

#         while True:
#             raw = ffmpeg_process.stdout.read(8192)
#             if not raw:
#                 break

#             pending.extend(raw)

#             while len(pending) >= target_bytes:
#                 packet = bytes(pending[:target_bytes])
#                 del pending[:target_bytes]

#                 chunk_count += 1
#                 if chunk_count <= 2 or chunk_count % 30 == 0:
#                     log(
#                         f"[AUDIO] PCM bytes={len(packet)} "
#                         f"chunks={chunk_count}"
#                     )
#                 yield packet

#         # Ignore a final partial packet. It is not a full 100 ms media block
#         # and feeding it would introduce a timing discontinuity.

#     except GeneratorExit:
#         log(f"[AUDIO] {bvid} client disconnected")
#     except Exception as e:
#         log(f"[AUDIO] {bvid} exception: {e}")
#     finally:
#         if ffmpeg_process is not None:
#             try:
#                 if ffmpeg_process.poll() is None:
#                     ffmpeg_process.terminate()
#                     try:
#                         ffmpeg_process.wait(timeout=2)
#                     except subprocess.TimeoutExpired:
#                         ffmpeg_process.kill()
#             except Exception as e:
#                 log(f"[AUDIO] cleanup error: {e}")

def pcm_audio_generator(bvid):
    ffmpeg_process = None
    chunk_count = 0
    last_log_time = time.time()

    try:
        if not os.path.isfile(FFMPEG_EXE):
            raise RuntimeError("FFmpeg not found: " + FFMPEG_EXE)

        source_url = get_bili_audio_url(bvid)
        log(f"[AUDIO] {bvid} source url acquired")

        ffmpeg_cmd = [
            FFMPEG_EXE,
            "-hide_banner",
            "-loglevel", "error",
            "-headers",
            (
                "User-Agent: " + session.headers["User-Agent"] + "\r\n"
                "Referer: " + BILI_HOME + "\r\n"
            ),
            "-re",
            "-i", source_url,
            "-vn",
            "-af", "aresample=async=1:first_pts=0",
            "-ac", "1",
            "-ar", "24000",
            "-c:a", "pcm_s16le",
            "-f", "s16le",
            "-flush_packets", "1",
            "pipe:1",
        ]

        log("[AUDIO] starting FFmpeg")
        ffmpeg_process = subprocess.Popen(
            ffmpeg_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )

        pending = bytearray()
        target_bytes = 2400 * 2

        while True:
            # ---- 记录 ffmpeg.stdout.read 耗时 ----
            read_start = time.time()
            raw = ffmpeg_process.stdout.read(8192)
            read_end = time.time()
            read_elapsed = read_end - read_start
            if read_elapsed > 0.2:
                print(f"[PC Perf Audio] ffmpeg.read blocked {read_elapsed:.3f}s")

            if ffmpeg_process.poll() is not None:
                print(f"[PC Error Audio] FFmpeg exited with code {ffmpeg_process.returncode}")
                break

            if not raw:
                time.sleep(0.01)
                continue

            pending.extend(raw)

            while len(pending) >= target_bytes:
                packet = bytes(pending[:target_bytes])
                del pending[:target_bytes]

                chunk_count += 1
                now = time.time()

                # ---- 每10个块打印间隔 ----
                if chunk_count % 10 == 0:
                    interval = now - last_log_time
                    print(f"[PC Perf Audio] chunk {chunk_count}, interval {interval:.3f}s (avg {interval/10:.3f}s)")
                    last_log_time = now

                # ---- 记录 yield 阻塞 ----
                yield_start = time.time()
                yield packet
                yield_end = time.time()
                yield_elapsed = yield_end - yield_start
                if yield_elapsed > 0.1:
                    print(f"[PC Perf Audio] yield blocked {yield_elapsed:.3f}s")

    except GeneratorExit:
        log(f"[AUDIO] {bvid} client disconnected")
    except Exception as e:
        log(f"[AUDIO] {bvid} exception: {e}")
    finally:
        if ffmpeg_process is not None:
            try:
                if ffmpeg_process.poll() is None:
                    ffmpeg_process.terminate()
                    try:
                        ffmpeg_process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        ffmpeg_process.kill()
            except Exception as e:
                log(f"[AUDIO] cleanup error: {e}")

# ============================================================
# /bili/audio
# ============================================================

@app.route("/bili/audio")
def api_bili_audio():
    bvid = request.args.get("bvid", "").strip()
    if not bvid:
        return jsonify({"code": -1, "message": "missing bvid"}), 400

    log(f"[AUDIO] request bvid={bvid}")
    return Response(
        pcm_audio_generator(bvid),
        mimetype="application/octet-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "close",
            "X-Accel-Buffering": "no",
        },
    )


# ============================================================
# /bili/play
# ============================================================

@app.route("/bili/play")
def api_bili_play():

    bvid = request.args.get(
        "bvid",
        "",
    ).strip()

    if not bvid:
        return jsonify({
            "code": -1,
            "message": "missing bvid",
        }), 400

    log(
        "========================================"
    )

    log(
        f"[PLAY] request bvid={bvid}"
    )

    log(
        "========================================"
    )

    return Response(
        mjpeg_generator(bvid),
        mimetype="image/jpeg",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "close",
            "X-Accel-Buffering": "no",
        },
    )


# ============================================================
# /bili/refresh
#
# 仅供开发调试使用。
# 不要让 ESP32 每次打开都调用它。
# ============================================================

@app.route("/bili/refresh")
def api_bili_refresh():

    # 触发人工刷新前，如果刚刚被风控，仍然不请求
    if bili_in_cooldown():
        return jsonify({
            "code": -352,
            "message": "B站正在冷却",
            "list": rank_cache["list"],
        }), 429

    result = fetch_bili_popular(
        force=True
    )

    return jsonify({
        "code": 0,
        "list": result,
    })


# ============================================================
# 启动
# ============================================================

if __name__ == "__main__":

    print(
        "========================================"
    )

    print(
        "VoCat Bilibili Proxy"
    )

    print(
        f"FFmpeg: {FFMPEG_EXE}"
    )

    print(
        f"FFmpeg exists: "
        f"{os.path.isfile(FFMPEG_EXE)}"
    )

    print(
        f"Cache file: {CACHE_FILE}"
    )

    print(
        "========================================"
    )

    if not os.path.isfile(
        FFMPEG_EXE
    ):
        raise RuntimeError(
            "找不到 FFmpeg："
            + FFMPEG_EXE
        )

    load_disk_cache()

    warmup_bilibili()

    # 启动时只做一次预热刷新：
    # 有持久缓存时不会强制频繁刷新
    if not rank_cache["list"]:
        fetch_bili_popular(
            force=True
        )

    print(
        f"[SERVER] http://{SERVER_HOST}:{SERVER_PORT}"
    )

    app.run(
        host=SERVER_HOST,
        port=SERVER_PORT,
        debug=False,
        threaded=True,
    )
