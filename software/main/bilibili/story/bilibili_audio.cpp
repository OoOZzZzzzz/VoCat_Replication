#include "bilibili_audio.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <vector>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "bilibili_debug.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocat_bilibili.h"

#define TAG "BILI_AUDIO"

namespace {

// The server sends raw PCM. The only correct playback clock is the audio
// sample count: 24,000 mono samples/second, 16-bit little-endian PCM.
constexpr int SAMPLE_RATE = 24000;
constexpr size_t CHUNK_SAMPLES = 2400;               // exactly 100 ms
constexpr size_t CHUNK_BYTES = CHUNK_SAMPLES * sizeof(int16_t);

// Fixed PSRAM jitter buffer. It owns all network-side audio storage for the
// lifetime of the worker and is never dynamically allocated per chunk.
constexpr size_t RING_SLOTS = 20;                    // 2.0 s
constexpr size_t PREBUFFER_SLOTS = 10;               // 1.0 s
constexpr size_t RING_BYTES = RING_SLOTS * CHUNK_BYTES;

constexpr size_t HTTP_BUFFER_BYTES = 4096;
constexpr int HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr int HTTP_STREAM_TIMEOUT_MS = 2000;
constexpr int HTTP_TIMEOUT_RETRY_LIMIT = 10;          // tolerate up to ~20 s
constexpr int HTTP_TIMEOUT_RETRY_DELAY_MS = 20;

constexpr uint32_t WORKER_STACK = 4096;
constexpr UBaseType_t WORKER_PRIORITY = 2;
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
};

struct PcmRing {
    uint8_t *storage = nullptr;
    uint16_t bytes[RING_SLOTS] = {};
    size_t write_index = 0;
    size_t read_index = 0;
    size_t count = 0;

    bool init()
    {
        if (storage) return true;
        storage = static_cast<uint8_t *>(heap_caps_malloc(
            RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!storage) {
            BILI_LOGE(TAG, "[RING] allocation failed bytes=%u", static_cast<unsigned>(RING_BYTES));
            return false;
        }
        clear();
        return true;
    }

    void clear()
    {
        write_index = 0;
        read_index = 0;
        count = 0;
        std::memset(bytes, 0, sizeof(bytes));
    }

    uint8_t *write_ptr()
    {
        return storage + write_index * CHUNK_BYTES;
    }

    uint8_t *read_ptr()
    {
        return storage + read_index * CHUNK_BYTES;
    }

    void push(uint16_t byte_count)
    {
        bytes[write_index] = byte_count;
        write_index = (write_index + 1) % RING_SLOTS;
        ++count;
    }

    uint16_t front_size() const
    {
        return bytes[read_index];
    }

    void pop()
    {
        bytes[read_index] = 0;
        read_index = (read_index + 1) % RING_SLOTS;
        --count;
    }
};

struct State {
    std::atomic<bool> worker_alive{false};
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<uint32_t> generation{0};

    TaskHandle_t worker = nullptr;
    std::mutex mutex;
    char requested_bvid[BILI_BVID_MAX_LEN + 1] = {};
    bilibili_audio_eof_cb_t eof_cb = nullptr;
    bilibili_audio_error_cb_t error_cb = nullptr;
    void *callback_arg = nullptr;
};

State s;
PcmRing ring;

static bool is_current(uint32_t generation)
{
    return s.generation.load(std::memory_order_acquire) == generation;
}

static void log_heap(const char *stage)
{
    BILI_LOGI(TAG,
              "[HEAP] %s internal=%u largest=%u psram=%u psram_largest=%u",
              stage,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

static void snapshot_request(uint32_t &generation,
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

static void close_http(esp_http_client_handle_t &client)
{
    if (!client) return;
    (void)esp_http_client_close(client);
    (void)esp_http_client_cleanup(client);
    client = nullptr;
}

static esp_http_client_handle_t open_stream(const char *bvid, int &error_code)
{
    error_code = AUDIO_ERROR_OPEN;

    char url[512] = {};
    if (!vocat_bilibili_build_audio_url(bvid, url, sizeof(url))) {
        error_code = AUDIO_ERROR_INVALID_URL;
        BILI_LOGE(TAG, "[HTTP] URL build failed bvid=%s", bvid);
        return nullptr;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = HTTP_CONNECT_TIMEOUT_MS;
    config.buffer_size = HTTP_BUFFER_BYTES;
    config.buffer_size_tx = 1024;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;

    const int64_t start_us = esp_timer_get_time();
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        BILI_LOGE(TAG, "[HTTP] init failed bvid=%s", bvid);
        return nullptr;
    }

    (void)esp_http_client_set_header(client, "User-Agent", "VoCat-Bilibili/Audio/8.0");
    (void)esp_http_client_set_header(client, "Accept", "application/octet-stream");

    const esp_err_t open_ret = esp_http_client_open(client, 0);
    const int64_t open_ms = (esp_timer_get_time() - start_us) / 1000;
    if (open_ret != ESP_OK) {
        BILI_LOGW(TAG,
                  "[HTTP] open failed bvid=%s ret=%s errno=%d open_ms=%" PRId64,
                  bvid, esp_err_to_name(open_ret), esp_http_client_get_errno(client), open_ms);
        close_http(client);
        return nullptr;
    }

    const int64_t headers = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    const int64_t length = esp_http_client_get_content_length(client);
    const esp_err_t timeout_ret = esp_http_client_set_timeout_ms(client, HTTP_STREAM_TIMEOUT_MS);

    BILI_LOGI(TAG,
              "[HTTP] connected bvid=%s status=%d length=%" PRId64
              " headers=%" PRId64 " open_ms=%" PRId64 " timeout=%s",
              bvid, status, length, headers, open_ms, esp_err_to_name(timeout_ret));

    if (status != 200) {
        error_code = AUDIO_ERROR_HTTP_STATUS;
        close_http(client);
        return nullptr;
    }

    error_code = 0;
    return client;
}

static void report_terminal(uint32_t generation,
                            const char *bvid,
                            bool eof,
                            int error_code,
                            uint32_t played_chunks,
                            uint64_t received,
                            uint64_t played_samples,
                            int64_t elapsed_ms,
                            bilibili_audio_eof_cb_t eof_cb,
                            bilibili_audio_error_cb_t error_cb,
                            void *callback_arg)
{
    const bool current = is_current(generation);
    if (current) {
        s.running.store(false, std::memory_order_release);
    }

    BILI_LOGI(TAG,
              "[RUN] end gen=%u bvid=%s eof=%d error=%d played_chunks=%u"
              " received=%" PRIu64 " played_ms=%" PRIu64 " elapsed_ms=%" PRId64
              " ring=%u",
              generation, bvid, eof ? 1 : 0, error_code, played_chunks,
              received, played_samples * 1000ULL / SAMPLE_RATE, elapsed_ms,
              static_cast<unsigned>(ring.count));

    if (!current) return;
    if (eof && eof_cb) {
        eof_cb(callback_arg);
    } else if (!eof && error_code != 0 && error_cb) {
        error_cb(callback_arg, error_code);
    }
}

static bool enable_output(AudioCodec *codec)
{
    if (!codec) return false;
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    return codec->output_enabled();
}

static void disable_output(AudioCodec *codec)
{
    if (codec && codec->output_enabled()) {
        codec->EnableOutput(false);
    }
}

static int read_network(esp_http_client_handle_t client,
                        uint32_t generation,
                        const char *bvid,
                        size_t &fill,
                        uint32_t &timeout_failures,
                        uint64_t &received,
                        bool &eof,
                        int &terminal_error)
{
    if (ring.count >= RING_SLOTS) return 0;

    const size_t remaining = CHUNK_BYTES - fill;
    const int n = esp_http_client_read(
        client, reinterpret_cast<char *>(ring.write_ptr() + fill), static_cast<int>(remaining));

    if (n < 0) {
        ++timeout_failures;
        const int err = esp_http_client_get_errno(client);
        if (timeout_failures == 1 || timeout_failures == HTTP_TIMEOUT_RETRY_LIMIT) {
            BILI_LOGW(TAG,
                      "[HTTP] read timeout/error gen=%u bvid=%s attempt=%u/%d"
                      " errno=%d ring=%u",
                      generation, bvid, timeout_failures, HTTP_TIMEOUT_RETRY_LIMIT,
                      err, static_cast<unsigned>(ring.count));
        }
        if (timeout_failures >= HTTP_TIMEOUT_RETRY_LIMIT) {
            terminal_error = AUDIO_ERROR_STREAM;
        } else {
            vTaskDelay(pdMS_TO_TICKS(HTTP_TIMEOUT_RETRY_DELAY_MS));
        }
        return -1;
    }

    timeout_failures = 0;
    if (n == 0) {
        eof = true;
        if (fill > 0) {
            if ((fill & 1U) != 0) {
                --fill;
                BILI_LOGW(TAG, "[HTTP] dropping trailing odd byte bvid=%s", bvid);
            }
            if (fill > 0) {
                ring.push(static_cast<uint16_t>(fill));
            }
            fill = 0;
        }
        return 1;
    }

    fill += static_cast<size_t>(n);
    received += static_cast<uint64_t>(n);
    if (fill == CHUNK_BYTES) {
        ring.push(static_cast<uint16_t>(fill));
        fill = 0;
        return 1;
    }
    return 0;
}

static bool play_chunk(AudioCodec *codec,
                       uint32_t generation,
                       const char *bvid,
                       uint8_t *data,
                       uint16_t bytes,
                       std::vector<int16_t> &scratch,
                       uint32_t &played_chunks,
                       uint64_t &played_samples,
                       int64_t &deadline_us,
                       int64_t session_start_us)
{
    if (!codec || bytes == 0 || (bytes & 1U) != 0 || bytes > CHUNK_BYTES) {
        return false;
    }

    const size_t samples = bytes / sizeof(int16_t);
    scratch.resize(samples);
    std::memcpy(scratch.data(), data, bytes);

    const int64_t write_start_us = esp_timer_get_time();
    if (deadline_us == 0) {
        deadline_us = write_start_us;
    }
    const int64_t duration_us = static_cast<int64_t>((samples * 1000000ULL) / SAMPLE_RATE);
    const int64_t target_end_us = deadline_us + duration_us;

    codec->OutputData(scratch);

    const int64_t now_us = esp_timer_get_time();
    const int64_t write_elapsed_us = now_us - write_start_us;
    ++played_chunks;
    played_samples += samples;
    deadline_us = target_end_us;

    if (now_us < target_end_us) {
        const int64_t wait_us = target_end_us - now_us;
        if (wait_us >= 1000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(wait_us / 1000ULL)));
        } else {
            taskYIELD();
        }
    } else {
        const int64_t late_us = now_us - target_end_us;
        if (late_us > 50000) {
            BILI_LOGW(TAG,
                      "[CLOCK] late gen=%u bvid=%s late_ms=%" PRId64
                      " write_ms=%" PRId64,
                      generation, bvid, late_us / 1000, write_elapsed_us / 1000);
            // Never accelerate audio after an underrun.
            deadline_us = now_us;
        }
    }

    if (played_chunks == 1 || played_chunks % 20U == 0U) {
        const int64_t wall_ms = (esp_timer_get_time() - session_start_us) / 1000;
        const int64_t audio_ms = static_cast<int64_t>(played_samples * 1000ULL / SAMPLE_RATE);
        BILI_LOGI(TAG,
                  "[PCM] gen=%u bvid=%s chunks=%u audio_ms=%" PRId64
                  " wall_ms=%" PRId64 " drift=%" PRId64 "ms write_ms=%" PRId64
                  " ring=%u",
                  generation, bvid, played_chunks, audio_ms, wall_ms,
                  wall_ms - audio_ms, write_elapsed_us / 1000,
                  static_cast<unsigned>(ring.count));
    }
    return true;
}

static void worker_entry(void *)
{
    s.worker_alive.store(true, std::memory_order_release);
    BILI_LOGI(TAG, "[WORKER] start core=%d priority=%lu stack=%lu handle=%p",
              xPortGetCoreID(), static_cast<unsigned long>(uxTaskPriorityGet(nullptr)),
              static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
              static_cast<void *>(xTaskGetCurrentTaskHandle()));

    if (!ring.init()) {
        log_heap("ring_init_failed");
    } else {
        log_heap("worker_ready");
        BILI_LOGI(TAG, "[RING] slots=%u prebuffer=%u bytes=%u",
                  static_cast<unsigned>(RING_SLOTS), static_cast<unsigned>(PREBUFFER_SLOTS),
                  static_cast<unsigned>(RING_BYTES));
    }

    uint32_t handled_generation = 0;
    esp_http_client_handle_t client = nullptr;
    AudioCodec *codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> playback_scratch;
    playback_scratch.reserve(CHUNK_SAMPLES);

    for (;;) {
        uint32_t generation = 0;
        char bvid[BILI_BVID_MAX_LEN + 1] = {};
        bilibili_audio_eof_cb_t eof_cb = nullptr;
        bilibili_audio_error_cb_t error_cb = nullptr;
        void *callback_arg = nullptr;
        snapshot_request(generation, bvid, sizeof(bvid), eof_cb, error_cb, callback_arg);

        if (generation == 0 || generation == handled_generation || bvid[0] == '\0') {
            uint32_t notify = 0;
            (void)xTaskNotifyWait(0, UINT32_MAX, &notify, pdMS_TO_TICKS(250));
            continue;
        }

        handled_generation = generation;
        close_http(client);
        if (ring.storage) ring.clear();

        size_t fill = 0;
        uint32_t timeout_failures = 0;
        uint64_t received = 0;
        uint64_t played_samples = 0;
        uint32_t played_chunks = 0;
        bool eof = false;
        bool output_started = false;
        int terminal_error = 0;
        int64_t deadline_us = 0;
        const int64_t session_start_us = esp_timer_get_time();
        s.running.store(true, std::memory_order_release);

        BILI_LOGI(TAG, "[RUN] begin gen=%u bvid=%s", generation, bvid);
        log_heap("session_begin");

        int open_error = 0;
        client = open_stream(bvid, open_error);
        if (!client) {
            terminal_error = open_error ? open_error : AUDIO_ERROR_OPEN;
            report_terminal(generation, bvid, false, terminal_error, 0, 0, 0,
                            (esp_timer_get_time() - session_start_us) / 1000,
                            eof_cb, error_cb, callback_arg);
            continue;
        }

        BILI_LOGI(TAG, "[BUFFER] start prebuffer target=%u chunks", static_cast<unsigned>(PREBUFFER_SLOTS));

        while (is_current(generation)) {
            if (terminal_error != 0) break;

            const bool paused = s.paused.load(std::memory_order_acquire);
            if (paused) {
                if (output_started) {
                    disable_output(codec);
                    output_started = false;
                    deadline_us = 0;
                    BILI_LOGI(TAG, "[STATE] gen=%u bvid=%s -> paused ring=%u",
                              generation, bvid, static_cast<unsigned>(ring.count));
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (!eof && ring.count < RING_SLOTS) {
                const int read_result = read_network(client, generation, bvid, fill, timeout_failures,
                                                     received, eof, terminal_error);
                if (read_result > 0 && (ring.count == 1 || ring.count == PREBUFFER_SLOTS || ring.count == RING_SLOTS)) {
                    BILI_LOGI(TAG, "[BUFFER] gen=%u bvid=%s ready=%u/%u received=%" PRIu64,
                              generation, bvid, static_cast<unsigned>(ring.count),
                              static_cast<unsigned>(RING_SLOTS), received);
                }
            }

            const bool enough_buffer = ring.count >= PREBUFFER_SLOTS;
            if (!output_started && (enough_buffer || (eof && ring.count > 0))) {
                if (!enable_output(codec)) {
                    terminal_error = AUDIO_ERROR_OPEN;
                    break;
                }
                output_started = true;
                deadline_us = 0;
                BILI_LOGI(TAG,
                          "[STATE] gen=%u bvid=%s -> playing prebuffer=%u received=%" PRIu64,
                          generation, bvid, static_cast<unsigned>(ring.count), received);
            }

            if (output_started && ring.count > 0) {
                const uint16_t bytes = ring.front_size();
                if (!play_chunk(codec, generation, bvid, ring.read_ptr(), bytes,
                                playback_scratch, played_chunks, played_samples,
                                deadline_us, session_start_us)) {
                    terminal_error = AUDIO_ERROR_STREAM;
                    break;
                }
                ring.pop();
                continue;
            }

            if (eof && ring.count == 0 && fill == 0) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
        }

        close_http(client);
        disable_output(codec);

        const bool completed = eof && terminal_error == 0 && is_current(generation) &&
                               ring.count == 0 && fill == 0;
        report_terminal(generation, bvid, completed ? true : false,
                        completed ? 0 : terminal_error,
                        played_chunks, received, played_samples,
                        (esp_timer_get_time() - session_start_us) / 1000,
                        eof_cb, error_cb, callback_arg);

        if (is_current(generation)) {
            s.running.store(false, std::memory_order_release);
        }
    }
}

static bool ensure_worker()
{
    if (s.worker_alive.load(std::memory_order_acquire) && s.worker != nullptr) {
        return true;
    }

    const BaseType_t ret = xTaskCreatePinnedToCore(
        worker_entry, "bili_audio", WORKER_STACK, nullptr,
        WORKER_PRIORITY, &s.worker, WORKER_CORE);
    if (ret != pdPASS) {
        s.worker = nullptr;
        BILI_LOGE(TAG, "[WORKER] create failed core=%d stack=%u", static_cast<int>(WORKER_CORE),
                  static_cast<unsigned>(WORKER_STACK));
        log_heap("worker_create_failed");
        return false;
    }

    BILI_LOGI(TAG, "[WORKER] created handle=%p core=%d priority=%lu stack=%u",
              static_cast<void *>(s.worker), static_cast<int>(WORKER_CORE),
              static_cast<unsigned long>(WORKER_PRIORITY), static_cast<unsigned>(WORKER_STACK));
    return true;
}

} // namespace

extern "C" bool bilibili_audio_start_ex(const char *bvid,
                                         bilibili_audio_eof_cb_t eof_cb,
                                         bilibili_audio_error_cb_t error_cb,
                                         void *user_data)
{
    if (!bvid || !bvid[0]) return false;
    if (!ensure_worker()) return false;
    if (!ring.init()) return false;

    const bool replacing = s.running.load(std::memory_order_acquire);
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        generation = s.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::strncpy(s.requested_bvid, bvid, sizeof(s.requested_bvid) - 1);
        s.requested_bvid[sizeof(s.requested_bvid) - 1] = '\0';
        s.eof_cb = eof_cb;
        s.error_cb = error_cb;
        s.callback_arg = user_data;
    }

    s.paused.store(false, std::memory_order_release);
    s.running.store(true, std::memory_order_release);

    BILI_LOGI(TAG,
              "[REQ] gen=%u bvid=%s replacing=%d worker=%p internal=%u largest=%u",
              generation, bvid, replacing ? 1 : 0, static_cast<void *>(s.worker),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    if (s.worker) {
        (void)xTaskNotify(s.worker, 1U, eSetBits);
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
    s.running.store(false, std::memory_order_release);
    s.paused.store(false, std::memory_order_release);
    BILI_LOGI(TAG, "[STOP] gen=%u", generation);
    if (s.worker) {
        (void)xTaskNotify(s.worker, 1U, eSetBits);
    }
}

extern "C" void bilibili_audio_set_paused(bool paused)
{
    s.paused.store(paused, std::memory_order_release);
    BILI_LOGI(TAG, "[PAUSE] paused=%d gen=%u running=%d",
              paused ? 1 : 0,
              s.generation.load(std::memory_order_acquire),
              s.running.load(std::memory_order_acquire) ? 1 : 0);
    if (s.worker) {
        (void)xTaskNotify(s.worker, 1U, eSetBits);
    }
}

extern "C" bool bilibili_audio_is_running(void)
{
    return s.running.load(std::memory_order_acquire);
}

extern "C" bool bilibili_audio_is_paused(void)
{
    return s.paused.load(std::memory_order_acquire);
}
