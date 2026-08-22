#include "bili_h264_player.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <string>

#include "application.h"
#include "audio_service.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "BILI_MEDIA";
constexpr char MEDIA_BASE_URL[] = "http://192.168.31.106:8000";

constexpr int VIDEO_W = 320;
constexpr int VIDEO_H = 176;
constexpr int VIDEO_STRIDE = VIDEO_W * 2;
constexpr size_t VIDEO_PIXELS = static_cast<size_t>(VIDEO_W) * VIDEO_H;
constexpr size_t VIDEO_RGB565_BYTES = VIDEO_PIXELS * 2;

constexpr size_t VIDEO_SLOT_BYTES = 16 * 1024;
constexpr int VIDEO_SLOT_COUNT = 4;
constexpr int VIDEO_QUEUE_DEPTH = VIDEO_SLOT_COUNT;
constexpr int VIDEO_HTTP_RX = 16 * 1024;

constexpr int AUDIO_HTTP_RX = 16 * 1024;
constexpr int AUDIO_SAMPLE_RATE = 24000;
constexpr int AUDIO_CHUNK_SAMPLES = 2400; // 100 ms
constexpr int AUDIO_CHUNK_BYTES = AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
constexpr int AUDIO_PREBUFFER_CHUNKS = 3;

constexpr int HTTP_TIMEOUT_MS = 8000;
constexpr int HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr int64_t SLOW_NET_US = 250000;
constexpr int64_t SLOW_DECODE_US = 30000;

constexpr uint32_t STREAM_MAGIC = 0x56434D31U; // VCM1
constexpr uint16_t STREAM_VERSION = 1;
constexpr uint16_t STREAM_VIDEO = 1;
constexpr uint16_t STREAM_AUDIO = 2;
constexpr size_t STREAM_HEADER_BYTES = 24;

struct StreamHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t stream;
    uint32_t seq;
    uint64_t pts_us;
    uint32_t payload_len;
};

struct VideoSlot {
    uint8_t *data = nullptr;
    size_t len = 0;
    uint32_t seq = 0;
    uint64_t pts_us = 0;
};

struct PlayerState {
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> video_started{false};
    std::atomic<int> tasks_alive{0};
    std::atomic<uint32_t> generation{0};

    std::mutex mutex;
    bili_player_frame_cb_t frame_cb = nullptr;
    bili_player_status_cb_t status_cb = nullptr;
    void *user_data = nullptr;
    std::string bvid;

    QueueHandle_t video_free_q = nullptr;
    QueueHandle_t video_ready_q = nullptr;
    VideoSlot video_slots[VIDEO_SLOT_COUNT]{};

    wifi_ps_type_t saved_wifi_ps = WIFI_PS_MAX_MODEM;
    bool wifi_changed = false;
    bool audio_mode = false;
};

PlayerState s;

static int64_t now_us() {
    return esp_timer_get_time();
}

static void status(const char *text) {
    bili_player_status_cb_t cb = nullptr;
    void *arg = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        cb = s.status_cb;
        arg = s.user_data;
    }
    if (cb && text) cb(text, arg);
    ESP_LOGI(TAG, "[STATUS] %s", text ? text : "");
}

static void force_wifi_performance(const char *where) {
    if (!s.running.load() || s.stopping.load()) return;
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) != ESP_OK) return;
    if (ps != WIFI_PS_NONE) {
        ESP_LOGW(TAG, "[WIFI-PS] override where=%s current=%d -> NONE", where, static_cast<int>(ps));
        const esp_err_t e = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "[WIFI-PS] set NONE=%s", esp_err_to_name(e));
    }
}

static void set_media_mode(bool enable) {
    if (enable) {
        if (!s.wifi_changed) {
            if (esp_wifi_get_ps(&s.saved_wifi_ps) != ESP_OK) {
                s.saved_wifi_ps = WIFI_PS_MAX_MODEM;
            }
            const esp_err_t e = esp_wifi_set_ps(WIFI_PS_NONE);
            s.wifi_changed = true;
            ESP_LOGI(TAG, "[MEDIA] wifi_ps NONE result=%s saved=%d", esp_err_to_name(e), static_cast<int>(s.saved_wifi_ps));
        }
        if (!s.audio_mode) {
            Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(true);
            s.audio_mode = true;
        }
    } else {
        if (s.audio_mode) {
            Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(false);
            s.audio_mode = false;
        }
        if (s.wifi_changed) {
            const esp_err_t e = esp_wifi_set_ps(s.saved_wifi_ps);
            ESP_LOGI(TAG, "[MEDIA] restore wifi_ps=%d result=%s", static_cast<int>(s.saved_wifi_ps), esp_err_to_name(e));
            s.wifi_changed = false;
        }
    }
}

static void destroy_video_queues() {
    if (s.video_free_q) {
        vQueueDelete(s.video_free_q);
        s.video_free_q = nullptr;
    }
    if (s.video_ready_q) {
        vQueueDelete(s.video_ready_q);
        s.video_ready_q = nullptr;
    }
    for (auto &slot : s.video_slots) {
        if (slot.data) {
            heap_caps_free(slot.data);
            slot.data = nullptr;
        }
        slot.len = 0;
    }
}

static bool create_video_queues() {
    destroy_video_queues();
    s.video_free_q = xQueueCreate(VIDEO_QUEUE_DEPTH, sizeof(uint8_t));
    s.video_ready_q = xQueueCreate(VIDEO_QUEUE_DEPTH, sizeof(uint8_t));
    if (!s.video_free_q || !s.video_ready_q) {
        ESP_LOGE(TAG, "[MEM] queue create failed free=%p ready=%p heap=%u psram=%u", (void *)s.video_free_q,
                 (void *)s.video_ready_q,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        destroy_video_queues();
        return false;
    }

    for (uint8_t i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        s.video_slots[i].data = static_cast<uint8_t *>(heap_caps_malloc(VIDEO_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s.video_slots[i].data) {
            ESP_LOGE(TAG, "[MEM] video slot %u alloc failed", static_cast<unsigned>(i));
            destroy_video_queues();
            return false;
        }
        xQueueSend(s.video_free_q, &i, 0);
    }
    ESP_LOGI(TAG, "[MEM] video ring %d x %u bytes", VIDEO_SLOT_COUNT, static_cast<unsigned>(VIDEO_SLOT_BYTES));
    return true;
}

static void task_done() {
    if (s.tasks_alive.fetch_sub(1) == 1) {
        s.running.store(false);
        s.stopping.store(false);
        s.video_started.store(false);
        set_media_mode(false);
        destroy_video_queues();
        ESP_LOGI(TAG, "[PLAYER] all media tasks exited");
    }
}

static void shutdown_http(esp_http_client_handle_t client) {
    if (!client) return;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static esp_http_client_handle_t open_http(const char *path, uint32_t gen) {
    char url[384];
    snprintf(url, sizeof(url), "%s%s?bvid=%s", MEDIA_BASE_URL, path, s.bvid.c_str());

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 16 * 1024;
    cfg.buffer_size_tx = 2048;
    cfg.keep_alive_enable = true;
    cfg.disable_auto_redirect = true;
    cfg.disable_auto_redirect = true;

    ESP_LOGI(TAG, "[HTTP] open gen=%u url=%s", static_cast<unsigned>(gen), url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[HTTP] init failed");
        return nullptr;
    }

    const int64_t t0 = now_us();
    if (esp_http_client_open(client, 0) != ESP_OK) {
        const int64_t dt = now_us() - t0;
        ESP_LOGE(TAG, "[HTTP] open failed dt=%" PRId64 "ms errno=%d", dt / 1000, esp_http_client_get_errno(client));
        shutdown_http(client);
        return nullptr;
    }
    const int64_t dt = now_us() - t0;
    const int64_t hdr = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "[HTTP] connected status=%d hdr=%" PRId64 "ms content_len=%d chunked=%d open=%" PRId64 "ms",
             esp_http_client_get_status_code(client), hdr, esp_http_client_get_content_length(client),
             esp_http_client_is_chunked_response(client) ? 1 : 0, dt / 1000);
    if (esp_http_client_get_status_code(client) != 200) {
        shutdown_http(client);
        return nullptr;
    }
    return client;
}

static bool http_read_some(esp_http_client_handle_t client, uint8_t *buf, size_t cap, size_t *out, const char *label) {
    *out = 0;
    const int64_t t0 = now_us();
    const int n = esp_http_client_read(client, reinterpret_cast<char *>(buf), static_cast<int>(cap));
    const int64_t dt = now_us() - t0;
    if (dt > SLOW_NET_US) {
        ESP_LOGW(TAG, "[HTTP][%s] read block=%" PRId64 "ms got=%d", label, dt / 1000, n);
    }
    if (n > 0) {
        *out = static_cast<size_t>(n);
        return true;
    }
    if (n == 0) return false;
    if (n == ESP_ERR_HTTP_EAGAIN || n == ESP_ERR_HTTP_READ_TIMEOUT || n == -ESP_ERR_HTTP_EAGAIN) {
        return true;
    }
    ESP_LOGE(TAG, "[HTTP][%s] read error=%d errno=%d", label, n, esp_http_client_get_errno(client));
    return false;
}

static bool read_exact(esp_http_client_handle_t client, uint8_t *dst, size_t len, const char *label, uint32_t *slow) {
    size_t got = 0;
    while (got < len && !s.stopping.load()) {
        force_wifi_performance(label);
        size_t n = 0;
        if (!http_read_some(client, dst + got, len - got, &n, label)) return false;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        got += n;
        if (n < len - got && slow && n < 1024) ++(*slow);
    }
    return got == len;
}

static uint16_t read_be16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
static uint32_t read_be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
static uint64_t read_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static bool parse_header(const uint8_t *p, StreamHeader *h) {
    h->magic = read_be32(p + 0);
    h->version = read_be16(p + 4);
    h->stream = read_be16(p + 6);
    h->seq = read_be32(p + 8);
    h->pts_us = read_be64(p + 12);
    h->payload_len = read_be32(p + 20);
    return h->magic == STREAM_MAGIC && h->version == STREAM_VERSION;
}

static bool decode_jpeg_and_publish(const uint8_t *jpeg, size_t jpeg_len, uint64_t pts_us, uint32_t seq) {
    uint8_t *decoded = nullptr;
    size_t decoded_len = 0, width = 0, height = 0, stride = 0;
    const int64_t t0 = now_us();
    const esp_err_t ret = jpeg_to_image(jpeg, jpeg_len, &decoded, &decoded_len, &width, &height, &stride);
    const int64_t dt = now_us() - t0;
    if (dt > SLOW_DECODE_US) {
        ESP_LOGW(TAG, "[VDEC] slow=%" PRId64 "ms seq=%u jpeg=%u", dt / 1000, static_cast<unsigned>(seq), static_cast<unsigned>(jpeg_len));
    }
    if (ret != ESP_OK || !decoded || width != VIDEO_W || height != VIDEO_H || stride < VIDEO_STRIDE || decoded_len < VIDEO_RGB565_BYTES) {
        ESP_LOGE(TAG, "[VDEC] failed ret=%d seq=%u len=%u decoded=%u w=%u h=%u stride=%u", static_cast<int>(ret),
                 static_cast<unsigned>(seq), static_cast<unsigned>(jpeg_len), static_cast<unsigned>(decoded_len),
                 static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(stride));
        if (decoded) jpeg_free_align(decoded);
        return false;
    }

    uint16_t *pixels = reinterpret_cast<uint16_t *>(decoded);
    for (size_t i = 0; i < VIDEO_PIXELS; ++i) {
        const uint16_t v = pixels[i];
        pixels[i] = static_cast<uint16_t>((v << 8) | (v >> 8));
    }

    bili_player_frame_cb_t cb = nullptr;
    void *arg = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        cb = s.frame_cb;
        arg = s.user_data;
    }
    if (cb && !s.stopping.load()) {
        bili_player_frame_t frame{};
        frame.rgb565 = decoded;
        frame.width = VIDEO_W;
        frame.height = VIDEO_H;
        frame.pts_us = pts_us;
        cb(&frame, arg);
    }
    jpeg_free_align(decoded);
    return cb != nullptr;
}

static void video_decode_task(void *arg) {
    s.tasks_alive.fetch_add(1);
    const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    uint32_t decoded = 0, dropped = 0;
    ESP_LOGI(TAG, "[VDEC] start gen=%u core=%d", static_cast<unsigned>(gen), xPortGetCoreID());

    while (!s.stopping.load() && s.generation.load() == gen) {
        uint8_t slot_id = 0;
        if (xQueueReceive(s.video_ready_q, &slot_id, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (slot_id >= VIDEO_SLOT_COUNT) continue;
        VideoSlot &slot = s.video_slots[slot_id];

        const bool shown = decode_jpeg_and_publish(slot.data, slot.len, slot.pts_us, slot.seq);
        if (shown) {
            ++decoded;
            if (!s.video_started.exchange(true)) status("正在播放");
        } else {
            ++dropped;
        }

        slot.len = 0;
        xQueueSend(s.video_free_q, &slot_id, 0);
        if (decoded <= 3 || decoded % 20 == 0) {
            ESP_LOGI(TAG, "[VDEC] shown=%u dropped=%u free=%u ready=%u",
                     static_cast<unsigned>(decoded), static_cast<unsigned>(dropped),
                     static_cast<unsigned>(uxQueueMessagesWaiting(s.video_free_q)),
                     static_cast<unsigned>(uxQueueMessagesWaiting(s.video_ready_q)));
        }
    }
    ESP_LOGI(TAG, "[VDEC] exit shown=%u dropped=%u", static_cast<unsigned>(decoded), static_cast<unsigned>(dropped));
    task_done();
    vTaskDelete(nullptr);
}

static void video_rx_task(void *arg) {
    s.tasks_alive.fetch_add(1);
    const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    uint32_t frames = 0, dropped = 0, slow = 0, bytes = 0;
    uint8_t header[STREAM_HEADER_BYTES];
    ESP_LOGI(TAG, "[VRX] start gen=%u core=%d", static_cast<unsigned>(gen), xPortGetCoreID());

    esp_http_client_handle_t client = open_http("/bili/video", gen);
    if (!client) {
        status("视频连接失败");
        task_done();
        vTaskDelete(nullptr);
        return;
    }
    status("视频已连接");

    while (!s.stopping.load() && s.generation.load() == gen) {
        uint8_t slot_id = 0;
        if (xQueueReceive(s.video_free_q, &slot_id, pdMS_TO_TICKS(200)) != pdTRUE) {
            ++dropped;
            ESP_LOGW(TAG, "[VRX] no free slot ready=%u dropped=%u", static_cast<unsigned>(uxQueueMessagesWaiting(s.video_ready_q)), static_cast<unsigned>(dropped));
            continue;
        }
        VideoSlot &slot = s.video_slots[slot_id];

        if (!read_exact(client, header, sizeof(header), "VIDEO_HDR", &slow)) {
            xQueueSend(s.video_free_q, &slot_id, 0);
            break;
        }
        StreamHeader h{};
        if (!parse_header(header, &h) || h.stream != STREAM_VIDEO || h.payload_len == 0 || h.payload_len > VIDEO_SLOT_BYTES) {
            ESP_LOGE(TAG, "[VRX] bad header magic=%08" PRIx32 " ver=%u stream=%u seq=%u len=%u", h.magic, h.version,
                     h.stream, h.seq, h.payload_len);
            xQueueSend(s.video_free_q, &slot_id, 0);
            break;
        }

        const int64_t t0 = now_us();
        if (!read_exact(client, slot.data, h.payload_len, "VIDEO_PAYLOAD", &slow)) {
            xQueueSend(s.video_free_q, &slot_id, 0);
            break;
        }
        const int64_t dt = now_us() - t0;
        slot.len = h.payload_len;
        slot.seq = h.seq;
        slot.pts_us = h.pts_us;
        ++frames;
        bytes += h.payload_len;

        if (dt > SLOW_NET_US) {
            ESP_LOGW(TAG, "[VRX] payload seq=%u wait=%" PRId64 "ms bytes=%u ready=%u free=%u", static_cast<unsigned>(h.seq), dt / 1000,
                     static_cast<unsigned>(h.payload_len), static_cast<unsigned>(uxQueueMessagesWaiting(s.video_ready_q)),
                     static_cast<unsigned>(uxQueueMessagesWaiting(s.video_free_q)));
        }
        if (xQueueSend(s.video_ready_q, &slot_id, pdMS_TO_TICKS(200)) != pdTRUE) {
            ++dropped;
            slot.len = 0;
            xQueueSend(s.video_free_q, &slot_id, 0);
        }
        if (frames <= 3 || frames % 10 == 0) {
            ESP_LOGI(TAG, "[VRX] frame=%u bytes=%u seq=%u ready=%u free=%u net=%" PRId64 "ms slow=%u", static_cast<unsigned>(frames),
                     static_cast<unsigned>(h.payload_len), static_cast<unsigned>(h.seq),
                     static_cast<unsigned>(uxQueueMessagesWaiting(s.video_ready_q)),
                     static_cast<unsigned>(uxQueueMessagesWaiting(s.video_free_q)), dt / 1000, static_cast<unsigned>(slow));
        }
    }

    shutdown_http(client);
    ESP_LOGI(TAG, "[VRX] exit frames=%u dropped=%u bytes=%u slow=%u", static_cast<unsigned>(frames), static_cast<unsigned>(dropped),
             static_cast<unsigned>(bytes), static_cast<unsigned>(slow));
    task_done();
    vTaskDelete(nullptr);
}

static void audio_task(void *arg) {
    s.tasks_alive.fetch_add(1);
    const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    ESP_LOGI(TAG, "[ARX] start gen=%u core=%d", static_cast<unsigned>(gen), xPortGetCoreID());

    esp_http_client_handle_t client = open_http("/bili/audio", gen);
    if (!client) {
        status("音频连接失败");
        task_done();
        vTaskDelete(nullptr);
        return;
    }

    uint8_t header[STREAM_HEADER_BYTES];
    uint8_t *pcm = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_CHUNK_BYTES, MALLOC_CAP_8BIT));
    uint8_t *pre = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_PREBUFFER_CHUNKS * AUDIO_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm || !pre) {
        ESP_LOGE(TAG, "[ARX] alloc failed pcm=%p pre=%p heap=%u psram=%u", pcm, pre,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (pcm) heap_caps_free(pcm);
        if (pre) heap_caps_free(pre);
        shutdown_http(client);
        task_done();
        vTaskDelete(nullptr);
        return;
    }

    int pre_count = 0;
    bool audio_started = false;
    uint32_t chunks = 0, slow = 0, underrun = 0;

    while (!s.stopping.load() && s.generation.load() == gen) {
        if (!read_exact(client, header, sizeof(header), "AUDIO_HDR", &slow)) break;
        StreamHeader h{};
        if (!parse_header(header, &h) || h.stream != STREAM_AUDIO || h.payload_len != AUDIO_CHUNK_BYTES) {
            ESP_LOGE(TAG, "[ARX] bad header stream=%u seq=%u len=%u", h.stream, h.seq, h.payload_len);
            break;
        }
        const int64_t t0 = now_us();
        if (!read_exact(client, pcm, AUDIO_CHUNK_BYTES, "AUDIO_PAYLOAD", &slow)) break;
        const int64_t dt = now_us() - t0;
        ++chunks;

        if (!audio_started) {
            memcpy(pre + pre_count * AUDIO_CHUNK_BYTES, pcm, AUDIO_CHUNK_BYTES);
            ++pre_count;
            if (s.video_started.load() && pre_count >= AUDIO_PREBUFFER_CHUNKS) {
                audio_started = true;
                auto &audio = Application::GetInstance().GetAudioService();
                for (int i = 0; i < pre_count; ++i) {
                    if (!audio.PushPcmToPlaybackQueue(reinterpret_cast<const int16_t *>(pre + i * AUDIO_CHUNK_BYTES),
                                                      AUDIO_CHUNK_SAMPLES, AUDIO_SAMPLE_RATE)) {
                        ++underrun;
                    }
                }
                pre_count = 0;
                ESP_LOGI(TAG, "[ARX] audio gate open chunks=%u", static_cast<unsigned>(chunks));
            }
        } else {
            if (!Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(
                    reinterpret_cast<const int16_t *>(pcm), AUDIO_CHUNK_SAMPLES, AUDIO_SAMPLE_RATE)) {
                ++underrun;
                ESP_LOGW(TAG, "[AOUT] push failed seq=%u underrun=%u", static_cast<unsigned>(h.seq), static_cast<unsigned>(underrun));
            }
        }

        if (dt > SLOW_NET_US) {
            ESP_LOGW(TAG, "[ARX] payload seq=%u wait=%" PRId64 "ms bytes=%u", static_cast<unsigned>(h.seq), dt / 1000, static_cast<unsigned>(h.payload_len));
        }
        if (chunks <= 3 || chunks % 20 == 0) {
            ESP_LOGI(TAG, "[ARX] chunk=%u pts=%" PRIu64 " net=%" PRId64 "ms slow=%u gate=%d", static_cast<unsigned>(chunks), h.pts_us, dt / 1000,
                     static_cast<unsigned>(slow), audio_started ? 1 : 0);
        }
    }

    heap_caps_free(pre);
    heap_caps_free(pcm);
    shutdown_http(client);
    ESP_LOGI(TAG, "[ARX] exit chunks=%u slow=%u underrun=%u", static_cast<unsigned>(chunks), static_cast<unsigned>(slow), static_cast<unsigned>(underrun));
    task_done();
    vTaskDelete(nullptr);
}

static bool start_tasks() {
    if (!create_video_queues()) return false;
    s.tasks_alive.store(0);
    const uint32_t gen = s.generation.load();

    if (xTaskCreatePinnedToCore(video_rx_task, "bili_vrx", 10 * 1024,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(gen)), 7, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "[PLAYER] video rx task create failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(video_decode_task, "bili_vdec", 12 * 1024,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(gen)), 6, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "[PLAYER] video decode task create failed");
        s.stopping.store(true);
        return false;
    }
    if (xTaskCreatePinnedToCore(audio_task, "bili_arx", 8 * 1024,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(gen)), 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "[PLAYER] audio task create failed");
        s.stopping.store(true);
        return false;
    }
    return true;
}

} // namespace

extern "C" bool bili_player_start(const char *bvid,
                                   bili_player_frame_cb_t frame_cb,
                                   bili_player_status_cb_t status_cb,
                                   void *user_data) {
    if (!bvid || !*bvid || !frame_cb) return false;

    if (s.running.load()) {
        bili_player_stop();
        const int64_t deadline = now_us() + 1000000;
        while (s.running.load() && now_us() < deadline) vTaskDelay(pdMS_TO_TICKS(10));
        if (s.running.load()) {
            ESP_LOGE(TAG, "[PLAYER] previous session failed to stop");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.frame_cb = frame_cb;
        s.status_cb = status_cb;
        s.user_data = user_data;
        s.bvid = bvid;
    }

    s.generation.fetch_add(1);
    s.running.store(true);
    s.stopping.store(false);
    s.video_started.store(false);

    ESP_LOGI(TAG, "[PLAYER] START gen=%u bvid=%s heap=%u psram=%u", static_cast<unsigned>(s.generation.load()), bvid,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    set_media_mode(true);
    status("建立媒体通道");

    if (!start_tasks()) {
        s.stopping.store(true);
        const int64_t deadline = now_us() + 1000000;
        while (s.tasks_alive.load() > 0 && now_us() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s.tasks_alive.load() == 0) {
            s.running.store(false);
            set_media_mode(false);
            destroy_video_queues();
        }
        status("播放器启动失败");
        return false;
    }
    return true;
}

extern "C" void bili_player_stop(void) {
    if (!s.running.load()) return;
    ESP_LOGI(TAG, "[PLAYER] STOP requested");
    s.stopping.store(true);
    s.generation.fetch_add(1);
    s.video_started.store(false);
    status("正在停止");
}

extern "C" bool bili_player_is_running(void) {
    return s.running.load() || s.tasks_alive.load() > 0;
}
