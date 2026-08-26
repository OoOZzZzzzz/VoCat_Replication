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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bilibili_debug.h"
#include "vocat_bilibili.h"

#define TAG "BILI_AUDIO"

namespace {

constexpr int SAMPLE_RATE = 24000;
constexpr size_t PCM_CHUNK_SAMPLES = 2400;
constexpr size_t PCM_CHUNK_BYTES =
    PCM_CHUNK_SAMPLES * sizeof(int16_t);

/*
 * Server output is 24 kHz / mono / s16le in 4800-byte blocks.
 * Read only 2400 bytes each time. A 16 KB HTTP read waits for multiple
 * server blocks and was the source of the observed 300-380 ms stalls.
 */
constexpr size_t HTTP_READ_BYTES = 2400;
constexpr size_t HTTP_BUFFER_BYTES = 8192;

constexpr int PREBUFFER_CHUNKS = 3;
constexpr int HTTP_START_TIMEOUT_MS = 5000;
constexpr int HTTP_STREAM_TIMEOUT_MS = 1200;
constexpr int STOP_WAIT_MS = 1500;

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
    void *eof_arg = nullptr;

    wifi_ps_type_t saved_wifi_ps =
        WIFI_PS_MAX_MODEM;

    bool wifi_changed = false;
    bool external_media = false;
};

State s;

static void enter_media_mode()
{
    if (!s.wifi_changed) {
        if (
            esp_wifi_get_ps(
                &s.saved_wifi_ps
            ) != ESP_OK
        ) {
            s.saved_wifi_ps =
                WIFI_PS_MAX_MODEM;
        }

        const esp_err_t ret =
            esp_wifi_set_ps(
                WIFI_PS_NONE
            );

        s.wifi_changed = true;

        BILI_LOGI(
            TAG,
            "[POWER] wifi_ps=NONE ret=%s saved=%d",
            esp_err_to_name(ret),
            static_cast<int>(
                s.saved_wifi_ps
            )
        );
    }

    if (!s.external_media) {
        Application::GetInstance()
            .GetAudioService()
            .SetExternalMediaPlaybackMode(true);

        s.external_media = true;
    }
}

static void leave_media_mode()
{
    if (s.external_media) {
        Application::GetInstance()
            .GetAudioService()
            .SetExternalMediaPlaybackMode(false);

        s.external_media = false;
    }

    if (s.wifi_changed) {
        const esp_err_t ret =
            esp_wifi_set_ps(
                s.saved_wifi_ps
            );

        BILI_LOGI(
            TAG,
            "[POWER] wifi_ps restore=%d ret=%s",
            static_cast<int>(
                s.saved_wifi_ps
            ),
            esp_err_to_name(ret)
        );

        s.wifi_changed = false;
    }
}

static esp_http_client_handle_t open_stream(
    const char *bvid
)
{
    char url[512];

    if (
        !vocat_bilibili_build_audio_url(
            bvid,
            url,
            sizeof(url)
        )
    ) {
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

    BILI_LOGI(
        TAG,
        "[HTTP] GET %s",
        url
    );

    esp_http_client_handle_t client =
        esp_http_client_init(
            &config
        );

    if (!client) {
        return nullptr;
    }

    const esp_err_t timeout_ret =
        esp_http_client_set_timeout_ms(
            client,
            HTTP_START_TIMEOUT_MS
        );

    BILI_LOGI(
        TAG,
        "[HTTP] start_timeout=%dms set_ret=%s",
        HTTP_START_TIMEOUT_MS,
        esp_err_to_name(timeout_ret)
    );

    esp_http_client_set_header(
        client,
        "User-Agent",
        "VoCat-Bilibili/3.2"
    );

    esp_http_client_set_header(
        client,
        "Accept",
        "application/octet-stream"
    );

    const esp_err_t ret =
        esp_http_client_open(
            client,
            0
        );

    if (ret != ESP_OK) {
        BILI_LOGE(
            TAG,
            "[HTTP] open failed ret=%s errno=%d",
            esp_err_to_name(ret),
            esp_http_client_get_errno(client)
        );

        esp_http_client_cleanup(
            client
        );

        return nullptr;
    }

    esp_http_client_fetch_headers(
        client
    );

    const int status =
        esp_http_client_get_status_code(
            client
        );

    const int64_t length =
        esp_http_client_get_content_length(
            client
        );

    BILI_LOGI(
        TAG,
        "[HTTP] connected status=%d length=%" PRId64,
        status,
        length
    );

    if (status != 200) {
        BILI_LOGE(
            TAG,
            "[HTTP] unexpected status=%d errno=%d",
            status,
            esp_http_client_get_errno(client)
        );
        esp_http_client_close(
            client
        );
        esp_http_client_cleanup(
            client
        );
        return nullptr;
    }

    const esp_err_t stream_timeout_ret =
        esp_http_client_set_timeout_ms(
            client,
            HTTP_STREAM_TIMEOUT_MS
        );

    BILI_LOGI(
        TAG,
        "[HTTP] stream_timeout=%dms set_ret=%s",
        HTTP_STREAM_TIMEOUT_MS,
        esp_err_to_name(stream_timeout_ret)
    );

    return client;
}

static bool push_pcm(
    const uint8_t *data
)
{
    return Application::GetInstance()
        .GetAudioService()
        .PushPcmToPlaybackQueue(
            reinterpret_cast<
                const int16_t *
            >(data),
            PCM_CHUNK_SAMPLES,
            SAMPLE_RATE
        );
}

static void task_entry(void *arg)
{
    const uint32_t generation =
        static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(arg)
        );

    char bvid[BILI_BVID_MAX_LEN + 1] = {};

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        strncpy(bvid, s.bvid, sizeof(bvid) - 1);
        bvid[sizeof(bvid) - 1] = '\0';
    }

    BILI_LOGI(
        TAG,
        "[TASK] start generation=%u bvid=%s",
        static_cast<unsigned>(generation),
        bvid
    );

    enter_media_mode();

    esp_http_client_handle_t client = open_stream(bvid);
    if (!client) {
        if (s.generation.load() == generation) {
            s.running.store(false);
            leave_media_mode();
        }
        s.task_alive.store(false);
        s.task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint8_t *read_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(
            HTTP_READ_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )
    );

    uint8_t *chunk = static_cast<uint8_t *>(
        heap_caps_malloc(
            PCM_CHUNK_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
    );

    uint8_t *prebuffer = static_cast<uint8_t *>(
        heap_caps_malloc(
            PCM_CHUNK_BYTES * PREBUFFER_CHUNKS,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
    );

    if (!read_buffer || !chunk || !prebuffer) {
        BILI_LOGE(
            TAG,
            "[BUFFER] allocation failed read=%p chunk=%p pre=%p",
            read_buffer,
            chunk,
            prebuffer
        );

        if (read_buffer) {
            heap_caps_free(read_buffer);
        }
        if (chunk) {
            heap_caps_free(chunk);
        }
        if (prebuffer) {
            heap_caps_free(prebuffer);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (s.generation.load() == generation) {
            s.running.store(false);
            leave_media_mode();
        }

        s.task_alive.store(false);
        s.task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    size_t chunk_filled = 0;
    int prebuffered = 0;
    uint32_t chunk_count = 0;
    uint32_t push_failures = 0;
    bool eof = false;
    int read_timeout_retries = 0;

    while (
        !s.stop.load() &&
        s.generation.load() == generation
    ) {
        if (s.paused.load()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int64_t read_start = esp_timer_get_time();
        const int n = esp_http_client_read(
            client,
            reinterpret_cast<char *>(read_buffer),
            HTTP_READ_BYTES
        );
        const int64_t read_elapsed =
            esp_timer_get_time() - read_start;

#if BILIBILI_DEBUG
        if (read_elapsed > 120000) {
            BILI_LOGW(
                TAG,
                "[HTTP] read=%" PRId64 "ms bytes=%d",
                read_elapsed / 1000,
                n
            );
        }
#endif

        if (n < 0) {
            const int err =
                esp_http_client_get_errno(client);

            ++read_timeout_retries;
            BILI_LOGW(
                TAG,
                "[HTTP] read failed n=%d errno=%d retry=%d/%d",
                n,
                err,
                read_timeout_retries,
                4
            );

            if (read_timeout_retries <= 4 && !s.stop.load()) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            BILI_LOGE(
                TAG,
                "[HTTP] read aborted after retries errno=%d",
                err
            );
            break;
        }

        read_timeout_retries = 0;

        if (n == 0) {
            eof = true;
            break;
        }

        size_t offset = 0;
        while (
            offset < static_cast<size_t>(n) &&
            !s.stop.load() &&
            s.generation.load() == generation
        ) {
            const size_t copy = std::min(
                PCM_CHUNK_BYTES - chunk_filled,
                static_cast<size_t>(n) - offset
            );

            memcpy(
                chunk + chunk_filled,
                read_buffer + offset,
                copy
            );

            chunk_filled += copy;
            offset += copy;

            if (chunk_filled < PCM_CHUNK_BYTES) {
                continue;
            }

            if (prebuffered < PREBUFFER_CHUNKS) {
                memcpy(
                    prebuffer + prebuffered * PCM_CHUNK_BYTES,
                    chunk,
                    PCM_CHUNK_BYTES
                );

                ++prebuffered;
                chunk_filled = 0;

                if (prebuffered < PREBUFFER_CHUNKS) {
                    continue;
                }

                BILI_LOGI(
                    TAG,
                    "[BUFFER] ready=%d x 100ms",
                    PREBUFFER_CHUNKS
                );

                for (int i = 0; i < PREBUFFER_CHUNKS; ++i) {
                    while (
                        !s.stop.load() &&
                        s.generation.load() == generation
                    ) {
                        if (push_pcm(
                                prebuffer + i * PCM_CHUNK_BYTES
                            )) {
                            break;
                        }

                        ++push_failures;
                        vTaskDelay(pdMS_TO_TICKS(2));
                    }
                }
            } else {
                while (
                    !s.stop.load() &&
                    s.generation.load() == generation
                ) {
                    if (push_pcm(chunk)) {
                        break;
                    }

                    ++push_failures;
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }

            chunk_filled = 0;
            ++chunk_count;

#if BILIBILI_DEBUG
            if (chunk_count <= 3 || chunk_count % 20 == 0) {
                BILI_LOGI(
                    TAG,
                    "[PCM] chunk=%u failures=%u",
                    static_cast<unsigned>(chunk_count),
                    static_cast<unsigned>(push_failures)
                );
            }
#endif
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    heap_caps_free(read_buffer);
    heap_caps_free(chunk);
    heap_caps_free(prebuffer);

    const bool current =
        s.generation.load() == generation;

    if (current) {
        s.running.store(false);
        s.stop.store(false);
        leave_media_mode();
    }

    BILI_LOGI(
        TAG,
        "[TASK] end gen=%u current=%d eof=%d chunks=%u failures=%u",
        static_cast<unsigned>(generation),
        current ? 1 : 0,
        eof ? 1 : 0,
        static_cast<unsigned>(chunk_count),
        static_cast<unsigned>(push_failures)
    );

    if (current && eof && !s.stop.load()) {
        bilibili_audio_eof_cb_t callback = nullptr;
        void *callback_arg = nullptr;

        {
            std::lock_guard<std::mutex> lock(s.mutex);
            callback = s.eof_cb;
            callback_arg = s.eof_arg;
        }

        if (callback) {
            callback(callback_arg);
        }
    }

    s.task_alive.store(false);
    s.task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" bool bilibili_audio_start(
    const char *bvid,
    bilibili_audio_eof_cb_t eof_cb,
    void *user_data
)
{
    if (
        !bvid ||
        bvid[0] == '\0'
    ) {
        return false;
    }

    if (s.task_alive.load()) {
        BILI_LOGI(
            TAG,
            "[TASK] stop previous"
        );

        s.stop.store(true);
        s.generation.fetch_add(1);

        const int64_t deadline =
            esp_timer_get_time() +
            static_cast<int64_t>(
                STOP_WAIT_MS
            ) *
                1000;

        while (
            s.task_alive.load() &&
            esp_timer_get_time() <
                deadline
        ) {
            vTaskDelay(
                pdMS_TO_TICKS(5)
            );
        }

        if (s.task_alive.load()) {
            BILI_LOGE(
                TAG,
                "[TASK] previous did not terminate"
            );
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(
            s.mutex
        );

        strncpy(
            s.bvid,
            bvid,
            sizeof(s.bvid) - 1
        );

        s.bvid[
            sizeof(s.bvid) - 1
        ] = '\0';

        s.eof_cb = eof_cb;
        s.eof_arg = user_data;
    }

    s.stop.store(false);
    s.paused.store(false);

    const uint32_t generation =
        s.generation.fetch_add(1) + 1;

    s.task_alive.store(true);
    s.running.store(true);

    BILI_LOGI(
        TAG,
        "[TASK] create gen=%u bvid=%s",
        static_cast<unsigned>(
            generation
        ),
        bvid
    );

    if (
        xTaskCreatePinnedToCore(
            task_entry,
            "bili_audio",
            8192,
            reinterpret_cast<void *>(
                static_cast<uintptr_t>(
                    generation
                )
            ),
            6,
            &s.task,
            0
        ) != pdPASS
    ) {
        s.task_alive.store(false);
        s.running.store(false);

        BILI_LOGE(
            TAG,
            "[TASK] create failed"
        );

        return false;
    }

    return true;
}

extern "C" void bilibili_audio_stop(void)
{
    if (!s.task_alive.load()) {
        return;
    }

    BILI_LOGI(
        TAG,
        "[TASK] stop requested"
    );

    s.stop.store(true);
    s.generation.fetch_add(1);
}

extern "C" void bilibili_audio_set_paused(
    bool paused
)
{
    s.paused.store(paused);

    BILI_LOGI(
        TAG,
        "[AUDIO] paused=%d",
        paused ? 1 : 0
    );
}

extern "C" bool bilibili_audio_is_running(void)
{
    return s.running.load();
}

extern "C" bool bilibili_audio_is_paused(void)
{
    return s.paused.load();
}
