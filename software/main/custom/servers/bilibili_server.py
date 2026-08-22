import os
import socket
import struct
import subprocess
import threading
import time
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
FFMPEG_EXE = os.environ.get("FFMPEG_EXE", r"D:\ffmpeg\bin\ffmpeg.exe")
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
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/151 Safari/537.36",
    "Referer": BILI_HOME,
    "Accept": "*/*",
})

state_lock = threading.Lock()
media_url_lock = threading.Lock()
rank_cache = {"timestamp": 0.0, "list": []}
video_info_cache = {}
media_url_cache = {}
bili_block_until = 0.0


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
        log(f"[WARMUP] failed: {exc}")


def bili_get(path, *, params=None, timeout=10):
    if bili_in_cooldown():
        raise RuntimeError("B站接口冷却中")
    r = session.get(BILI_API.rstrip("/") + "/" + path.lstrip("/"), params=params, timeout=timeout)
    r.raise_for_status()
    data = r.json()
    if data.get("code") == -352:
        trigger_bili_cooldown()
        raise RuntimeError("B站风控 -352")
    if data.get("code") != 0:
        raise RuntimeError(f"B站接口失败 code={data.get('code')} message={data.get('message')}")
    return data


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
        "-re", "-i", au,
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
    return jsonify({"list": fetch_bili_popular(force=False)})


# Media stays TCP. These HTTP endpoints are diagnostics only.
@app.route("/bili/video")
def api_bili_video():
    return Response("TCP_MEDIA_ONLY", status=409, mimetype="text/plain")


@app.route("/bili/audio")
def api_bili_audio():
    return Response("TCP_MEDIA_ONLY", status=409, mimetype="text/plain")


if __name__ == "__main__":
    load_disk_cache()
    warmup_bilibili()
    threading.Thread(target=discovery_worker, name="bili-discovery", daemon=True).start()
    threading.Thread(target=tcp_accept_loop, args=("0.0.0.0", VIDEO_TCP_PORT, run_video_tcp, "VIDEO"), daemon=True).start()
    threading.Thread(target=tcp_accept_loop, args=("0.0.0.0", AUDIO_TCP_PORT, run_audio_tcp, "AUDIO"), daemon=True).start()
    log(f"server start http={SERVER_HOST}:{SERVER_PORT} lan_ip={get_lan_ip()} media={VIDEO_TCP_PORT}/{AUDIO_TCP_PORT}")
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True, debug=False)
