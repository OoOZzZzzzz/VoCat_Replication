#include "bilibili_audio.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <algorithm>

#include "application.h"
#include "audio_service.h"
#include "bilibili_debug.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocat_bilibili.h"

#define TAG "BILI_AUDIO"

namespace {

constexpr int SAMPLE_RATE = 24000;
constexpr size_t PCM_CHUNK_SAMPLES = 2400;                  // 100 ms
constexpr size_t PCM_CHUNK_BYTES = PCM_CHUNK_SAMPLES * sizeof(int16_t);
constexpr size_t HTTP_READ_BYTES = 8192;
constexpr size_t HTTP_BUFFER_BYTES = 16384;
constexpr int PREBUFFER_CHUNKS = 3;                         // 300 ms: matches AudioService direct queue
constexpr int HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr int HTTP_STREAM_TIMEOUT_MS = 5000;
constexpr int HTTP_READ_RETRY_LIMIT = 3;
constexpr int HTTP_READ_RETRY_DELAY_MS = 100;
constexpr uint32_t WORKER_STACK = 4096;
constexpr UBaseType_t WORKER_PRIORITY = 3;                 // below LVGL/audio output

#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
constexpr BaseType_t WORKER_CORE = 0;
#else
constexpr BaseType_t WORKER_CORE = 1;
#endif

enum AudioError : int {
    AUDIO_ERROR_INVALID_URL = 1,
    AUDIO_ERROR_OPEN = 2,
    AUDIO_ERROR_HTTP_STATUS = 3,
    AUDIO_ERROR_MEMORY = 4,
    AUDIO_ERROR_STREAM = 5,
    AUDIO_ERROR_PLAYBACK_QUEUE = 6,
};

enum class StreamState : uint8_t {
    Idle,
    Opening,
    Buffering,
    Playing,
    Paused,
    Stopping,
};

struct State {
    std::atomic<bool> worker_alive{false};
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<uint32_t> generation{0};

    TaskHandle_t task = nullptr;
    std::mutex mutex;
    char requested_bvid[BILI_BVID_MAX_LEN + 1] = {};
    bilibili_audio_eof_cb_t eof_cb = nullptr;
    bilibili_audio_error_cb_t error_cb = nullptr;
    void *callback_arg = nullptr;
};

struct Buffers {
    uint8_t *read_buffer = nullptr;
    uint8_t *chunk = nullptr;
    uint8_t *prebuffer = nullptr;
};

State s;

static const char *state_name(StreamState state)
{
    switch (state) {
        case StreamState::Idle: return "idle";
        case StreamState::Opening: return "opening";
        case StreamState::Buffering: return "buffering";
        case StreamState::Playing: return "playing";
        case StreamState::Paused: return "paused";
        case StreamState::Stopping: return "stopping";
    }
    return "unknown";
}

static void log_heap(const char *stage)
{
    BILI_LOGI(TAG,
              "[HEAP] stage=%s internal=%u internal_min=%u psram=%u",
              stage,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

static void read_request(uint32_t &generation,
                         char *bvid,
                         size_t bvid_size,
                         bilibili_audio_eof_cb_t &eof_cb,
                         bilibili_audio_error_cb_t &error_cb,
                         void *&callback_arg)
{
    std::lock_guard<std::mutex> lock(s.mutex);
    generation = s.generation.load(std::memory_order_acquire);
    std::strncpy(bvid, s.requested_bvid, bvid_size - 1);
    bvid[bvid_size - 1] = '\0';
    eof_cb = s.eof_cb;
    error_cb = s.error_cb;
    callback_arg = s.callback_arg;
}

static bool generation_current(uint32_t generation)
{
    return s.generation.load(std::memory_order_acquire) == generation;
}

static void close_client(esp_http_client_handle_t *client)
{
    if (client == nullptr || *client == nullptr) return;
    esp_http_client_close(*client);
    esp_http_client_cleanup(*client);
    *client = nullptr;
}

static esp_http_client_handle_t open_stream(const char *bvid, int &error_code)
{
    error_code = AUDIO_ERROR_OPEN;

    char url[512] = {};
    if (!vocat_bilibili_build_audio_url(bvid, url, sizeof(url))) {
        error_code = AUDIO_ERROR_INVALID_URL;
        BILI_LOGE(TAG, "[HTTP] build URL failed bvid=%s", bvid);
        return nullptr;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = HTTP_CONNECT_TIMEOUT_MS;
    config.buffer_size = HTTP_BUFFER_BYTES;
    config.buffer_size_tx = 2048;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        BILI_LOGE(TAG, "[HTTP] init failed bvid=%s", bvid);
        return nullptr;
    }

    esp_http_client_set_header(client, "User-Agent", "VoCat-Bilibili/Audio/5.0");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    BILI_LOGI(TAG, "[HTTP] open bvid=%s connect_timeout=%d stream_timeout=%d",
              bvid, HTTP_CONNECT_TIMEOUT_MS, HTTP_STREAM_TIMEOUT_MS);

    const int64_t start_us = esp_timer_get_time();
    const esp_err_t open_ret = esp_http_client_open(client, 0);
    const int64_t open_ms = (esp_timer_get_time() - start_us) / 1000;

    if (open_ret != ESP_OK) {
        BILI_LOGW(TAG,
                  "[HTTP] open failed bvid=%s ret=%s errno=%d elapsed=%" PRId64 "ms",
                  bvid,
                  esp_err_to_name(open_ret),
                  esp_http_client_get_errno(client),
                  open_ms);
        close_client(&client);
        return nullptr;
    }

    const int header_ret = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    const int64_t content_length = esp_http_client_get_content_length(client);
    const esp_err_t timeout_ret = esp_http_client_set_timeout_ms(client, HTTP_STREAM_TIMEOUT_MS);

    BILI_LOGI(TAG,
              "[HTTP] connected bvid=%s status=%d length=%" PRId64
              " headers=%d open=%" PRId64 "ms stream_timeout_ret=%s",
              bvid,
              status,
              content_length,
              header_ret,
              open_ms,
              esp_err_to_name(timeout_ret));

    if (status != 200) {
        BILI_LOGW(TAG, "[HTTP] reject bvid=%s status=%d", bvid, status);
        close_client(&client);
        error_code = AUDIO_ERROR_HTTP_STATUS;
        return nullptr;
    }

    error_code = 0;
    return client;
}

static bool allocate_buffers(Buffers &buffers)
{
    buffers.read_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        HTTP_READ_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    buffers.chunk = static_cast<uint8_t *>(heap_caps_malloc(
        PCM_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    buffers.prebuffer = static_cast<uint8_t *>(heap_caps_malloc(
        PCM_CHUNK_BYTES * PREBUFFER_CHUNKS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!buffers.read_buffer || !buffers.chunk || !buffers.prebuffer) {
        BILI_LOGE(TAG, "[BUFFER] alloc failed read=%p chunk=%p prebuffer=%p",
                  buffers.read_buffer, buffers.chunk, buffers.prebuffer);
        return false;
    }

    log_heap("worker_buffers_ready");
    return true;
}

static void free_buffers(Buffers &buffers)
{
    if (buffers.read_buffer) heap_caps_free(buffers.read_buffer);
    if (buffers.chunk) heap_caps_free(buffers.chunk);
    if (buffers.prebuffer) heap_caps_free(buffers.prebuffer);
    buffers = {};
}

static bool push_pcm(const uint8_t *data)
{
    return Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(
        reinterpret_cast<const int16_t *>(data), PCM_CHUNK_SAMPLES, SAMPLE_RATE);
}

static bool push_pcm_until_accepted(const uint8_t *data, uint32_t generation)
{
    constexpr int MAX_RETRIES = 100; // at most 200 ms
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        if (!generation_current(generation)) return false;
        if (s.paused.load(std::memory_order_acquire)) return false;
        if (push_pcm(data)) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    BILI_LOGE(TAG, "[PCM] playback queue unavailable gen=%u", generation);
    return false;
}

static bool append_chunk(uint8_t *chunk,
                         size_t &filled,
                         const uint8_t *src,
                         size_t bytes)
{
    const size_t copy_bytes = std::min(PCM_CHUNK_BYTES - filled, bytes);
    std::memcpy(chunk + filled, src, copy_bytes);
    filled += copy_bytes;
    return copy_bytes == bytes;
}

static void finish_stream(uint32_t generation,
                          const char *bvid,
                          bool eof,
                          int error_code,
                          uint32_t pushed,
                          uint64_t received,
                          int64_t elapsed_ms,
                          bilibili_audio_eof_cb_t eof_cb,
                          bilibili_audio_error_cb_t error_cb,
                          void *callback_arg)
{
    s.running.store(false, std::memory_order_release);

    BILI_LOGI(TAG,
              "[RUN] end gen=%u bvid=%s state=idle eof=%d error=%d pushed=%u received=%" PRIu64
              " elapsed=%" PRId64 "ms internal=%u stack=%lu",
              generation,
              bvid,
              eof ? 1 : 0,
              error_code,
              pushed,
              received,
              elapsed_ms,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    if (!generation_current(generation)) return;

    if (eof) {
        if (eof_cb) {
            BILI_LOGI(TAG, "[CALLBACK] EOF gen=%u bvid=%s", generation, bvid);
            eof_cb(callback_arg);
        }
    } else if (error_code != 0 && error_cb) {
        BILI_LOGW(TAG, "[CALLBACK] ERROR gen=%u bvid=%s error=%d",
                  generation, bvid, error_code);
        error_cb(callback_arg, error_code);
    }
}

static void run_stream(const char *bvid,
                       uint32_t generation,
                       Buffers &buffers,
                       bilibili_audio_eof_cb_t eof_cb,
                       bilibili_audio_error_cb_t error_cb,
                       void *callback_arg)
{
    const int64_t start_us = esp_timer_get_time();
    StreamState state = StreamState::Opening;
    esp_http_client_handle_t client = nullptr;
    int terminal_error = 0;
    bool eof = false;
    uint32_t pushed = 0;
    uint64_t received = 0;
    int read_failures = 0;
    int buffered_chunks = 0;
    size_t filled = 0;

    s.running.store(true, std::memory_order_release);
    BILI_LOGI(TAG, "[RUN] begin gen=%u bvid=%s core=%d priority=%lu stack=%lu",
              generation,
              bvid,
              xPortGetCoreID(),
              static_cast<unsigned long>(uxTaskPriorityGet(nullptr)),
              static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    int open_error = 0;
    client = open_stream(bvid, open_error);
    if (!client) {
        terminal_error = open_error != 0 ? open_error : AUDIO_ERROR_OPEN;
        goto done;
    }

    state = StreamState::Buffering;
    BILI_LOGI(TAG, "[STATE] gen=%u bvid=%s -> %s", generation, bvid, state_name(state));

    while (generation_current(generation) && buffered_chunks < PREBUFFER_CHUNKS) {
        if (s.paused.load(std::memory_order_acquire)) {
            state = StreamState::Paused;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const int n = esp_http_client_read(
            client, reinterpret_cast<char *>(buffers.read_buffer), HTTP_READ_BYTES);
        if (n < 0) {
            ++read_failures;
            BILI_LOGW(TAG, "[HTTP] prebuffer read failed gen=%u attempt=%d/%d errno=%d received=%" PRIu64,
                      generation,
                      read_failures,
                      HTTP_READ_RETRY_LIMIT,
                      esp_http_client_get_errno(client),
                      received);
            if (read_failures >= HTTP_READ_RETRY_LIMIT) {
                terminal_error = AUDIO_ERROR_STREAM;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTP_READ_RETRY_DELAY_MS));
            continue;
        }
        if (n == 0) {
            eof = true;
            if (filled != 0) {
                BILI_LOGW(TAG, "[HTTP] EOF with partial PCM gen=%u bytes=%u",
                          generation, static_cast<unsigned>(filled));
            }
            break;
        }

        read_failures = 0;
        received += static_cast<uint64_t>(n);
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            if (!generation_current(generation)) break;
            const size_t available = static_cast<size_t>(n) - offset;
            const size_t before = filled;
            (void)append_chunk(buffers.chunk, filled, buffers.read_buffer + offset, available);
            const size_t copied = filled - before;
            offset += copied;

            if (filled == PCM_CHUNK_BYTES) {
                std::memcpy(buffers.prebuffer + buffered_chunks * PCM_CHUNK_BYTES,
                            buffers.chunk,
                            PCM_CHUNK_BYTES);
                ++buffered_chunks;
                filled = 0;
                if (buffered_chunks >= PREBUFFER_CHUNKS) break;
            }
        }
    }

    if (terminal_error == 0 && !generation_current(generation)) goto done;
    if (terminal_error == 0 && buffered_chunks == 0 && !eof) {
        terminal_error = AUDIO_ERROR_STREAM;
        goto done;
    }

    if (terminal_error == 0 && buffered_chunks > 0) {
        if (s.paused.load(std::memory_order_acquire)) {
            state = StreamState::Paused;
        } else {
            state = StreamState::Playing;
        }
        BILI_LOGI(TAG, "[BUFFER] ready gen=%u bvid=%s chunks=%d bytes=%u state=%s",
                  generation,
                  bvid,
                  buffered_chunks,
                  static_cast<unsigned>(buffered_chunks * PCM_CHUNK_BYTES),
                  state_name(state));

        for (int i = 0; i < buffered_chunks && generation_current(generation); ++i) {
            if (s.paused.load(std::memory_order_acquire)) {
                state = StreamState::Paused;
                break;
            }
            if (!push_pcm_until_accepted(buffers.prebuffer + i * PCM_CHUNK_BYTES, generation)) {
                if (!generation_current(generation) || s.paused.load(std::memory_order_acquire)) break;
                terminal_error = AUDIO_ERROR_PLAYBACK_QUEUE;
                break;
            }
            ++pushed;
        }
    }

    filled = 0;

    while (terminal_error == 0 && generation_current(generation)) {
        if (s.paused.load(std::memory_order_acquire)) {
            if (state != StreamState::Paused) {
                state = StreamState::Paused;
                BILI_LOGI(TAG, "[STATE] gen=%u bvid=%s -> paused", generation, bvid);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (state == StreamState::Paused) {
            state = StreamState::Playing;
            BILI_LOGI(TAG, "[STATE] gen=%u bvid=%s -> playing", generation, bvid);
        }

        const int n = esp_http_client_read(
            client, reinterpret_cast<char *>(buffers.read_buffer), HTTP_READ_BYTES);
        if (n < 0) {
            ++read_failures;
            BILI_LOGW(TAG,
                      "[HTTP] read failed gen=%u bvid=%s attempt=%d/%d errno=%d elapsed=%" PRId64 "ms received=%" PRIu64,
                      generation,
                      bvid,
                      read_failures,
                      HTTP_READ_RETRY_LIMIT,
                      esp_http_client_get_errno(client),
                      (esp_timer_get_time() - start_us) / 1000,
                      received);
            if (read_failures >= HTTP_READ_RETRY_LIMIT) {
                terminal_error = AUDIO_ERROR_STREAM;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTP_READ_RETRY_DELAY_MS));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }

        read_failures = 0;
        received += static_cast<uint64_t>(n);

        size_t offset = 0;
        while (offset < static_cast<size_t>(n) && generation_current(generation)) {
            const size_t available = static_cast<size_t>(n) - offset;
            const size_t before = filled;
            (void)append_chunk(buffers.chunk, filled, buffers.read_buffer + offset, available);
            const size_t copied = filled - before;
            offset += copied;

            if (filled != PCM_CHUNK_BYTES) continue;
            if (!push_pcm_until_accepted(buffers.chunk, generation)) {
                if (!generation_current(generation) || s.paused.load(std::memory_order_acquire)) break;
                terminal_error = AUDIO_ERROR_PLAYBACK_QUEUE;
                break;
            }

            filled = 0;
            ++pushed;
            if ((pushed % 20U) == 0U) {
                BILI_LOGI(TAG,
                          "[PCM] gen=%u bvid=%s pushed=%u received=%" PRIu64
                          " elapsed=%" PRId64 "ms stack=%lu",
                          generation,
                          bvid,
                          pushed,
                          received,
                          (esp_timer_get_time() - start_us) / 1000,
                          static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
            }
        }
    }

done:
    state = StreamState::Stopping;
    if (!generation_current(generation)) {
        BILI_LOGI(TAG, "[STATE] gen=%u bvid=%s -> stopping (superseded by gen=%u)",
                  generation, bvid, s.generation.load(std::memory_order_acquire));
    }
    close_client(&client);

    finish_stream(generation,
                  bvid,
                  eof && terminal_error == 0,
                  terminal_error,
                  pushed,
                  received,
                  (esp_timer_get_time() - start_us) / 1000,
                  eof_cb,
                  error_cb,
                  callback_arg);
}

static void worker_entry(void *)
{
    BILI_LOGI(TAG, "[WORKER] start core=%d priority=%lu stack=%lu handle=%p",
              xPortGetCoreID(),
              static_cast<unsigned long>(uxTaskPriorityGet(nullptr)),
              static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
              static_cast<void *>(xTaskGetCurrentTaskHandle()));
    log_heap("worker_start");

    Buffers buffers;
    if (!allocate_buffers(buffers)) {
        BILI_LOGE(TAG, "[WORKER] buffer init failed");
        s.worker_alive.store(false, std::memory_order_release);
        s.task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    s.worker_alive.store(true, std::memory_order_release);
    uint32_t processed_generation = 0;

    for (;;) {
        uint32_t notify = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY) != pdTRUE) continue;

        char bvid[BILI_BVID_MAX_LEN + 1] = {};
        bilibili_audio_eof_cb_t eof_cb = nullptr;
        bilibili_audio_error_cb_t error_cb = nullptr;
        void *callback_arg = nullptr;
        uint32_t generation = 0;
        read_request(generation, bvid, sizeof(bvid), eof_cb, error_cb, callback_arg);

        if (generation == 0 || generation == processed_generation || bvid[0] == '\0') {
            continue;
        }

        processed_generation = generation;
        if (s.generation.load(std::memory_order_acquire) != generation) continue;

        run_stream(bvid, generation, buffers, eof_cb, error_cb, callback_arg);
    }
}

static bool ensure_worker()
{
    if (s.worker_alive.load(std::memory_order_acquire)) return true;
    if (s.task != nullptr) return true;

    const BaseType_t result = xTaskCreatePinnedToCore(
        worker_entry,
        "bili_audio",
        WORKER_STACK,
        nullptr,
        WORKER_PRIORITY,
        &s.task,
        WORKER_CORE);
    if (result != pdPASS) {
        s.task = nullptr;
        BILI_LOGE(TAG, "[WORKER] create failed core=%d priority=%lu stack=%u",
                  static_cast<int>(WORKER_CORE),
                  static_cast<unsigned long>(WORKER_PRIORITY),
                  static_cast<unsigned>(WORKER_STACK));
        log_heap("worker_create_failed");
        return false;
    }

    BILI_LOGI(TAG, "[WORKER] created handle=%p core=%d priority=%lu stack=%u",
              static_cast<void *>(s.task),
              static_cast<int>(WORKER_CORE),
              static_cast<unsigned long>(WORKER_PRIORITY),
              static_cast<unsigned>(WORKER_STACK));
    return true;
}

} // namespace

extern "C" bool bilibili_audio_start_ex(const char *bvid,
                                         bilibili_audio_eof_cb_t eof_cb,
                                         bilibili_audio_error_cb_t error_cb,
                                         void *user_data)
{
    if (bvid == nullptr || bvid[0] == '\0') return false;
    if (!ensure_worker()) return false;

    const uint32_t generation = s.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool replacing = s.running.load(std::memory_order_acquire);

    // A new Bilibili track owns the direct PCM output queue. Drop only the
    // previous track's already-buffered PCM; do not reuse stale audio after a
    // track switch.
    Application::GetInstance().GetAudioService().ResetDecoder();

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        std::strncpy(s.requested_bvid, bvid, sizeof(s.requested_bvid) - 1);
        s.requested_bvid[sizeof(s.requested_bvid) - 1] = '\0';
        s.eof_cb = eof_cb;
        s.error_cb = error_cb;
        s.callback_arg = user_data;
    }

    s.paused.store(false, std::memory_order_release);

    BILI_LOGI(TAG,
              "[REQ] gen=%u bvid=%s replace=%d worker=%p internal=%u psram=%u",
              generation,
              bvid,
              replacing ? 1 : 0,
              static_cast<void *>(s.task),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));

    if (xTaskNotify(s.task, 1U, eSetBits) != pdPASS) {
        BILI_LOGE(TAG, "[REQ] notify failed gen=%u task=%p", generation, static_cast<void *>(s.task));
        return false;
    }
    return true;
}

extern "C" bool bilibili_audio_start(const char *bvid,
                                      bilibili_audio_eof_cb_t eof_cb,
                                      void *user_data)
{
    return bilibili_audio_start_ex(bvid, eof_cb, nullptr, user_data);
}

extern "C" void bilibili_audio_stop(void)
{
    const uint32_t generation = s.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    s.paused.store(false, std::memory_order_release);
    BILI_LOGI(TAG, "[STOP] gen=%u running=%d", generation,
              s.running.load(std::memory_order_acquire) ? 1 : 0);
    if (s.task != nullptr) (void)xTaskNotify(s.task, 1U, eSetBits);
}

extern "C" void bilibili_audio_set_paused(bool paused)
{
    s.paused.store(paused, std::memory_order_release);
    BILI_LOGI(TAG, "[PAUSE] paused=%d gen=%u running=%d",
              paused ? 1 : 0,
              s.generation.load(std::memory_order_acquire),
              s.running.load(std::memory_order_acquire) ? 1 : 0);
    if (s.task != nullptr) (void)xTaskNotify(s.task, 1U, eSetBits);
}

extern "C" bool bilibili_audio_is_running(void)
{
    return s.running.load(std::memory_order_acquire);
}

extern "C" bool bilibili_audio_is_paused(void)
{
    return s.paused.load(std::memory_order_acquire);
}
