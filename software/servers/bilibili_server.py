from flask import Flask, Response, jsonify, request
import json
import os
import struct
import subprocess
import threading
import time
from pathlib import Path

import requests

app = Flask(__name__)
SERVER_HOST = "0.0.0.0"
SERVER_PORT = 8000

FFMPEG_EXE = os.environ.get("FFMPEG_EXE", r"D:\ffmpeg\bin\ffmpeg.exe")
BILI_HOME = "https://www.bilibili.com/"
BILI_API = "https://api.bilibili.com"
RANK_CACHE_TTL = 120
VIDEO_INFO_CACHE_TTL = 600
PLAY_URL_CACHE_TTL = 600
CACHE_FILE = Path(__file__).with_name("bili_cache.json")
BILI_COOKIE = os.environ.get("BILI_COOKIE", "").strip()

VIDEO_W = 320
VIDEO_H = 176
VIDEO_FPS = 10
AUDIO_RATE = 24000
AUDIO_SAMPLES = 2400
AUDIO_BYTES = AUDIO_SAMPLES * 2
STREAM_MAGIC = 0x56434D31  # VCM1
STREAM_VERSION = 1
STREAM_VIDEO = 1
STREAM_AUDIO = 2
STREAM_HEADER = struct.Struct("!IHHIQI")

session = requests.Session()
session.headers.update({
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/151.0.0.0 Safari/537.36",
    "Referer": BILI_HOME,
    "Origin": BILI_HOME.rstrip("/"),
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "zh-CN,zh;q=0.9",
    "Connection": "keep-alive",
})
if BILI_COOKIE:
    session.headers["Cookie"] = BILI_COOKIE

state_lock = threading.RLock()
rank_cache = {"timestamp": 0.0, "list": []}
video_info_cache = {}
media_url_cache = {}


def log(msg: str):
    print(time.strftime("[%Y-%m-%d %H:%M:%S]") + " " + msg, flush=True)


def load_disk_cache():
    global rank_cache
    try:
        if CACHE_FILE.exists():
            data = json.loads(CACHE_FILE.read_text(encoding="utf-8"))
            items = data.get("list", [])
            if items:
                rank_cache = {"timestamp": float(data.get("timestamp", 0)), "list": items[:4]}
    except Exception as exc:
        log(f"[CACHE] load failed: {exc}")


def save_disk_cache(items):
    try:
        CACHE_FILE.write_text(
            json.dumps({"timestamp": time.time(), "list": items[:4]}, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    except Exception as exc:
        log(f"[CACHE] save failed: {exc}")


def warmup_bilibili():
    try:
        r = session.get(BILI_HOME, timeout=10)
        log(f"[WARMUP] bilibili status={r.status_code}")
    except Exception as exc:
        log(f"[WARMUP] failed: {exc}")


def bili_get(path, params=None, timeout=10):
    r = session.get(BILI_API.rstrip("/") + "/" + path.lstrip("/"), params=params, timeout=timeout)
    r.raise_for_status()
    data = r.json()
    if data.get("code") != 0:
        raise RuntimeError(f"B站接口失败 code={data.get('code')} message={data.get('message')}")
    return data


def fetch_bili_popular():
    now = time.time()
    with state_lock:
        cached = list(rank_cache["list"])
        ts = rank_cache["timestamp"]
    if cached and now - ts < RANK_CACHE_TTL:
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
        if result:
            with state_lock:
                rank_cache["timestamp"] = now
                rank_cache["list"] = result
            save_disk_cache(result)
            return result
    except Exception as exc:
        log(f"[BILI] popular failed: {exc}")
    return cached[:4]


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
    with state_lock:
        item = media_url_cache.get(bvid)
    if item and now - item["timestamp"] < PLAY_URL_CACHE_TTL:
        return item["video"], item["audio"]

    pages = get_video_info(bvid).get("pages", [])
    if not pages or not pages[0].get("cid"):
        raise RuntimeError("cid empty")
    cid = pages[0]["cid"]
    data = bili_get(
        "/x/player/playurl",
        params={"bvid": bvid, "cid": cid, "fnval": 16, "fnver": 0, "fourk": 0, "qn": 16},
        timeout=15,
    ).get("data", {})
    dash = data.get("dash") or {}
    videos = sorted(dash.get("video") or [], key=lambda x: (x.get("width", 9999), x.get("height", 9999), x.get("bandwidth", 999999999)))
    audios = sorted(dash.get("audio") or [], key=lambda x: x.get("bandwidth", 999999999))
    if not videos or not audios:
        raise RuntimeError("DASH media empty")
    video = videos[0]
    audio = audios[0]
    vu = video.get("baseUrl") or video.get("base_url")
    au = audio.get("baseUrl") or audio.get("base_url")
    if not vu or not au:
        raise RuntimeError("media URL empty")
    log(f"[PLAY] {bvid} source={video.get('width')}x{video.get('height')} audio_bw={audio.get('bandwidth')}")
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
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    log(f"[FFMPEG][{name}] {line}")
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
        "-q:v", "7",
        "-f", "mjpeg",
        "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    drain_stderr(proc, f"video:{bvid}")
    log(f"[VIDEO] ffmpeg pid={proc.pid} start {bvid} {VIDEO_W}x{VIDEO_H}@{VIDEO_FPS}")
    return proc


def start_audio_ffmpeg(bvid):
    _, au = get_bili_media_urls(bvid)
    cmd = [
        FFMPEG_EXE, "-hide_banner", "-loglevel", "warning",
        "-headers", ffmpeg_headers(),
        "-re", "-i", au,
        "-vn", "-af", "aresample=async=1:first_pts=0",
        "-ac", "1", "-ar", str(AUDIO_RATE),
        "-c:a", "pcm_s16le", "-f", "s16le",
        "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    drain_stderr(proc, f"audio:{bvid}")
    log(f"[AUDIO] ffmpeg pid={proc.pid} start {bvid} PCM={AUDIO_RATE}/mono/{AUDIO_BYTES}B")
    return proc


def jpeg_frames(proc, bvid):
    buf = bytearray()
    frame_count = 0
    total_bytes = 0
    try:
        while True:
            chunk = proc.stdout.read(16 * 1024)
            if not chunk:
                break
            buf.extend(chunk)
            while True:
                soi = buf.find(b"\xff\xd8")
                if soi < 0:
                    if len(buf) > 2:
                        del buf[:-2]
                    break
                eoi = buf.find(b"\xff\xd9", soi + 2)
                if eoi < 0:
                    if soi > 0:
                        del buf[:soi]
                    break
                frame = bytes(buf[soi:eoi + 2])
                del buf[:eoi + 2]
                frame_count += 1
                total_bytes += len(frame)
                if frame_count <= 3 or frame_count % 20 == 0:
                    log(f"[VIDEO] frame={frame_count} jpeg={len(frame)} total={total_bytes}")
                yield frame, frame_count
    finally:
        log(f"[VIDEO] ffmpeg end {bvid} frames={frame_count} bytes={total_bytes} rc={proc.poll()}")


def make_video_stream(bvid):
    proc = None
    try:
        proc = start_video_ffmpeg(bvid)
        for frame, seq in jpeg_frames(proc, bvid):
            pts = (seq - 1) * 100_000
            header = STREAM_HEADER.pack(STREAM_MAGIC, STREAM_VERSION, STREAM_VIDEO, seq, pts, len(frame))
            yield header
            yield frame
    except GeneratorExit:
        raise
    except Exception as exc:
        log(f"[VIDEO] stream error bvid={bvid} err={exc}")
    finally:
        kill_process(proc)


def make_audio_stream(bvid):
    proc = None
    seq = 0
    total = 0
    try:
        proc = start_audio_ffmpeg(bvid)
        pending = bytearray()
        while True:
            chunk = proc.stdout.read(AUDIO_BYTES - len(pending))
            if not chunk:
                break
            pending.extend(chunk)
            if len(pending) < AUDIO_BYTES:
                continue
            seq += 1
            payload = bytes(pending[:AUDIO_BYTES])
            del pending[:AUDIO_BYTES]
            pts = (seq - 1) * 100_000
            header = STREAM_HEADER.pack(STREAM_MAGIC, STREAM_VERSION, STREAM_AUDIO, seq, pts, len(payload))
            yield header
            yield payload
            total += len(payload)
            if seq <= 3 or seq % 20 == 0:
                log(f"[AUDIO] chunk={seq} bytes={len(payload)} total={total}")
    except GeneratorExit:
        raise
    except Exception as exc:
        log(f"[AUDIO] stream error bvid={bvid} err={exc}")
    finally:
        kill_process(proc)
        log(f"[AUDIO] end bvid={bvid} chunks={seq} bytes={total}")


@app.get("/bili")
def bili_list():
    items = fetch_bili_popular()
    return jsonify({"list": items[:4]})


@app.get("/bili/video")
def bili_video():
    bvid = request.args.get("bvid", "").strip()
    if not bvid:
        return jsonify({"error": "missing bvid"}), 400
    log(f"[HTTP-VIDEO] start bvid={bvid}")
    return Response(
        make_video_stream(bvid),
        mimetype="application/octet-stream",
        headers={"Cache-Control": "no-cache", "Connection": "keep-alive"},
        direct_passthrough=True,
    )


@app.get("/bili/audio")
def bili_audio():
    bvid = request.args.get("bvid", "").strip()
    if not bvid:
        return jsonify({"error": "missing bvid"}), 400
    log(f"[HTTP-AUDIO] start bvid={bvid}")
    return Response(
        make_audio_stream(bvid),
        mimetype="application/octet-stream",
        headers={"Cache-Control": "no-cache", "Connection": "keep-alive"},
        direct_passthrough=True,
    )


if __name__ == "__main__":
    load_disk_cache()
    warmup_bilibili()
    log(f"server start {SERVER_HOST}:{SERVER_PORT}")
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True, debug=False)
