#include "bilibili_audio.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>

#include "application.h"
#include "audio_service.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bilibili_debug.h"
#include "vocat_bilibili.h"

#define TAG "BILI_AUDIO"

namespace {
constexpr int SAMPLE_RATE = 24000;
constexpr size_t PCM_CHUNK_SAMPLES = 2400;
constexpr size_t PCM_CHUNK_BYTES = PCM_CHUNK_SAMPLES * sizeof(int16_t);
constexpr size_t HTTP_READ_BYTES = 2400;
constexpr size_t HTTP_BUFFER_BYTES = 8192;
constexpr int PREBUFFER_CHUNKS = 3;
constexpr int HTTP_START_TIMEOUT_MS = 5000;
constexpr int HTTP_STREAM_TIMEOUT_MS = 600;
constexpr int STOP_WAIT_MS = 1200;

enum : int {
    AUDIO_ERROR_INVALID_URL = 1,
    AUDIO_ERROR_OPEN = 2,
    AUDIO_ERROR_HTTP_STATUS = 3,
    AUDIO_ERROR_MEMORY = 4,
    AUDIO_ERROR_STREAM = 5,
    AUDIO_ERROR_PLAYBACK_BACKPRESSURE = 6,
};

struct State {
    std::atomic<bool> running{false};
    std::atomic<bool> task_alive{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<uint32_t> generation{0};
    TaskHandle_t task = nullptr;
    std::mutex mutex;
    char bvid[BILI_BVID_MAX_LEN + 1] = {};
    bilibili_audio_eof_cb_t eof_cb = nullptr;
    bilibili_audio_error_cb_t error_cb = nullptr;
    void *callback_arg = nullptr;
};

State s;

static void copy_callbacks(bilibili_audio_eof_cb_t *eof,
                           bilibili_audio_error_cb_t *error,
                           void **arg)
{
    std::lock_guard<std::mutex> lock(s.mutex);
    *eof = s.eof_cb;
    *error = s.error_cb;
    *arg = s.callback_arg;
}

static esp_http_client_handle_t open_stream(const char *bvid, int *error_code)
{
    char url[512];
    if (!vocat_bilibili_build_audio_url(bvid, url, sizeof(url))) {
        if (error_code) *error_code = AUDIO_ERROR_INVALID_URL;
        return nullptr;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = HTTP_START_TIMEOUT_MS;
    config.buffer_size = HTTP_BUFFER_BYTES;
    config.buffer_size_tx = 2048;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;

    BILI_LOGI(TAG, "[HTTP] GET %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (error_code) *error_code = AUDIO_ERROR_OPEN;
        return nullptr;
    }

    esp_http_client_set_header(client, "User-Agent", "VoCat-Bilibili/4.0");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        BILI_LOGW(TAG, "[HTTP] open failed ret=%s errno=%d", esp_err_to_name(ret), esp_http_client_get_errno(client));
        esp_http_client_cleanup(client);
        if (error_code) *error_code = AUDIO_ERROR_OPEN;
        return nullptr;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    const int64_t length = esp_http_client_get_content_length(client);
    BILI_LOGI(TAG, "[HTTP] connected status=%d length=%" PRId64, status, length);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (error_code) *error_code = AUDIO_ERROR_HTTP_STATUS;
        return nullptr;
    }

    ret = esp_http_client_set_timeout_ms(client, HTTP_STREAM_TIMEOUT_MS);
    BILI_LOGI(TAG, "[HTTP] stream timeout ret=%s", esp_err_to_name(ret));
    return client;
}

static bool push_pcm(const uint8_t *data)
{
    return Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(
        reinterpret_cast<const int16_t *>(data), PCM_CHUNK_SAMPLES, SAMPLE_RATE);
}

static bool push_pcm_with_timeout(const uint8_t *data, uint32_t generation)
{
    constexpr int MAX_PUSH_RETRIES = 250;
    for (int retry = 0; retry < MAX_PUSH_RETRIES; ++retry) {
        if (s.stop.load(std::memory_order_acquire) ||
            s.generation.load(std::memory_order_acquire) != generation) {
            return false;
        }
        if (push_pcm(data)) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static void task_entry(void *arg)
{
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    char bvid[BILI_BVID_MAX_LEN + 1] = {};
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        std::strncpy(bvid, s.bvid, sizeof(bvid) - 1);
        bvid[sizeof(bvid) - 1] = '\0';
    }

    int terminal_error = 0;
    bool eof = false;
    uint32_t pushed = 0;
    esp_http_client_handle_t client = open_stream(bvid, &terminal_error);

    if (client != nullptr) {
        uint8_t *read_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            HTTP_READ_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        uint8_t *chunk = static_cast<uint8_t *>(heap_caps_malloc(
            PCM_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint8_t *prebuffer = static_cast<uint8_t *>(heap_caps_malloc(
            PCM_CHUNK_BYTES * PREBUFFER_CHUNKS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (read_buffer == nullptr || chunk == nullptr || prebuffer == nullptr) {
            terminal_error = AUDIO_ERROR_MEMORY;
            BILI_LOGE(TAG, "[BUFFER] allocation failed read=%p chunk=%p pre=%p",
                      read_buffer, chunk, prebuffer);
        } else {
            size_t filled = 0;
            int pre_count = 0;
            int read_failures = 0;

            while (!s.stop.load(std::memory_order_acquire) &&
                   s.generation.load(std::memory_order_acquire) == generation) {
                if (s.paused.load(std::memory_order_acquire)) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }

                const int n = esp_http_client_read(
                    client, reinterpret_cast<char *>(read_buffer), HTTP_READ_BYTES);

                if (n < 0) {
                    ++read_failures;
                    BILI_LOGW(TAG, "[HTTP] read failed errno=%d retry=%d/4",
                              esp_http_client_get_errno(client), read_failures);
                    if (read_failures >= 4) {
                        terminal_error = AUDIO_ERROR_STREAM;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                read_failures = 0;
                if (n == 0) {
                    eof = true;
                    break;
                }

                size_t offset = 0;
                while (offset < static_cast<size_t>(n) &&
                       !s.stop.load(std::memory_order_acquire) &&
                       s.generation.load(std::memory_order_acquire) == generation) {
                    const size_t available = static_cast<size_t>(n) - offset;
                    const size_t copy_size =
                        (PCM_CHUNK_BYTES - filled < available) ?
                            (PCM_CHUNK_BYTES - filled) : available;
                    std::memcpy(chunk + filled, read_buffer + offset, copy_size);
                    filled += copy_size;
                    offset += copy_size;
                    if (filled != PCM_CHUNK_BYTES) continue;

                    if (pre_count < PREBUFFER_CHUNKS) {
                        std::memcpy(prebuffer + pre_count * PCM_CHUNK_BYTES,
                                    chunk, PCM_CHUNK_BYTES);
                        ++pre_count;
                        filled = 0;
                        if (pre_count < PREBUFFER_CHUNKS) continue;

                        BILI_LOGI(TAG, "[BUFFER] ready %d x 100ms", PREBUFFER_CHUNKS);
                        for (int i = 0; i < PREBUFFER_CHUNKS; ++i) {
                            if (!push_pcm_with_timeout(prebuffer + i * PCM_CHUNK_BYTES, generation)) {
                                terminal_error = AUDIO_ERROR_PLAYBACK_BACKPRESSURE;
                                break;
                            }
                        }
                        if (terminal_error != 0) break;
                    } else {
                        if (!push_pcm_with_timeout(chunk, generation)) {
                            terminal_error = AUDIO_ERROR_PLAYBACK_BACKPRESSURE;
                            break;
                        }
                    }

                    filled = 0;
                    ++pushed;
                }
            }
        }

        if (read_buffer) heap_caps_free(read_buffer);
        if (chunk) heap_caps_free(chunk);
        if (prebuffer) heap_caps_free(prebuffer);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    BILI_LOGI(TAG, "[TASK] end gen=%u eof=%d pushed=%u error=%d",
              generation, eof ? 1 : 0, pushed, terminal_error);

    if (s.generation.load(std::memory_order_acquire) == generation &&
        !s.stop.load(std::memory_order_acquire)) {
        bilibili_audio_eof_cb_t eof_cb = nullptr;
        bilibili_audio_error_cb_t error_cb = nullptr;
        void *callback_arg = nullptr;
        copy_callbacks(&eof_cb, &error_cb, &callback_arg);
        if (eof && eof_cb) eof_cb(callback_arg);
        else if (terminal_error != 0 && error_cb) error_cb(callback_arg, terminal_error);
        s.running.store(false);
        s.stop.store(false);
    }

    s.task_alive.store(false);
    s.task = nullptr;
    vTaskDelete(nullptr);
}
} // namespace

extern "C" bool bilibili_audio_start_ex(const char *bvid,
                                         bilibili_audio_eof_cb_t eof_cb,
                                         bilibili_audio_error_cb_t error_cb,
                                         void *user_data)
{
    if (!bvid || bvid[0] == '\0') return false;

    if (s.task_alive.load()) {
        s.stop.store(true);
        s.generation.fetch_add(1);
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(STOP_WAIT_MS) * 1000;
        while (s.task_alive.load() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(5));
        if (s.task_alive.load()) {
            BILI_LOGE(TAG, "[TASK] previous stream did not terminate safely");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        std::strncpy(s.bvid, bvid, sizeof(s.bvid) - 1);
        s.bvid[sizeof(s.bvid) - 1] = '\0';
        s.eof_cb = eof_cb;
        s.error_cb = error_cb;
        s.callback_arg = user_data;
    }

    s.stop.store(false);
    s.paused.store(false);
    const uint32_t generation = s.generation.fetch_add(1) + 1;
    s.task_alive.store(true);
    s.running.store(true);

    if (xTaskCreatePinnedToCore(task_entry, "bili_audio", 8192,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(generation)),
                                6, &s.task, 0) != pdPASS) {
        s.task_alive.store(false);
        s.running.store(false);
        BILI_LOGE(TAG, "[TASK] create failed");
        return false;
    }
    BILI_LOGI(TAG, "[TASK] create gen=%u bvid=%s", generation, bvid);
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
    if (!s.task_alive.load()) return;
    s.stop.store(true);
    s.generation.fetch_add(1);
}

extern "C" void bilibili_audio_set_paused(bool paused)
{
    s.paused.store(paused);
    BILI_LOGI(TAG, "[AUDIO] paused=%d", paused ? 1 : 0);
}

extern "C" bool bilibili_audio_is_running(void) { return s.running.load(); }
extern "C" bool bilibili_audio_is_paused(void) { return s.paused.load(); }
