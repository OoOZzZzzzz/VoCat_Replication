#include "vocat_bilibili_ui.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "application.h"
#include "board.h"
#include "display/emote_display.h"
#include "expression_emote.h"
#include "gfx.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "audio_service.h"

#define TAG "BILI_UI"
#define BILI_TASK_STACK_SIZE (12 * 1024)
#define BILI_PLAYER_STACK_SIZE (16 * 1024)
#define BILI_ROW_COUNT BILI_RECORD_MAX
#define BILI_VIDEO_W 320
#define BILI_VIDEO_H 176
#define BILI_VIDEO_STRIDE (BILI_VIDEO_W * 2)
#define BILI_FRAME_BUF_SIZE (BILI_VIDEO_W * BILI_VIDEO_H * 2)
#define BILI_FRAME_BUF_COUNT 4
#define BILI_JPEG_BUF_SIZE (96 * 1024)
#define BILI_RX_CHUNK_SIZE (16 * 1024)
#define BILI_JPEG_SLOT_SIZE (32 * 1024)
#define BILI_JPEG_SLOT_COUNT 8
#define BILI_JPEG_QUEUE_DEPTH 6
#define BILI_PLAY_URL_BASE "http://192.168.31.106:8000/bili/play?bvid="
#define BILI_AUDIO_URL_BASE "http://192.168.31.106:8000/bili/audio?bvid="
#define BILI_AUDIO_SAMPLE_RATE 24000
#define BILI_AUDIO_CHUNK_SAMPLES 2400
#define BILI_AUDIO_PREBUFFER_CHUNKS 2
#define BILI_AUDIO_CHUNK_BYTES (BILI_AUDIO_CHUNK_SAMPLES * sizeof(int16_t))

namespace {

struct State {
    emote::EmoteDisplay *display = nullptr;
    emote_handle_t emote = nullptr;

    gfx_obj_t *page = nullptr;
    gfx_obj_t *header = nullptr;
    gfx_obj_t *tabs = nullptr;
    gfx_obj_t *rows[BILI_ROW_COUNT] = {};
    gfx_obj_t *video_img = nullptr;
    gfx_obj_t *back = nullptr;
    gfx_obj_t *player_title = nullptr;
    gfx_obj_t *player_status = nullptr;

    gfx_image_dsc_t frame_dsc[BILI_FRAME_BUF_COUNT] = {};
    uint8_t *frame_buf[BILI_FRAME_BUF_COUNT] = {};
    volatile int display_index = 0;
    volatile int previous_index = -1;
    volatile int pending_index = -1;
    volatile bool ui_update_scheduled = false;
    portMUX_TYPE frame_mux = portMUX_INITIALIZER_UNLOCKED;

    bool inited = false;
    volatile bool active = false;
    volatile bool player = false;
    volatile bool stop_player = false;
    volatile bool stop_audio = false;
    volatile bool audio_ready = false;
    volatile bool audio_failed = false;
    int selected = -1;

    bili_video_t videos[BILI_ROW_COUNT] = {};
    uint8_t count = 0;

    TaskHandle_t fetch_task = nullptr;
    TaskHandle_t player_task = nullptr;
    TaskHandle_t video_rx_task = nullptr;
    TaskHandle_t audio_task = nullptr;

    QueueHandle_t jpeg_queue = nullptr;
    uint8_t *jpeg_slots[BILI_JPEG_SLOT_COUNT] = {};
    size_t jpeg_slot_len[BILI_JPEG_SLOT_COUNT] = {};
    uint8_t jpeg_slot_state[BILI_JPEG_SLOT_COUNT] = {};
    portMUX_TYPE jpeg_mux = portMUX_INITIALIZER_UNLOCKED;

    bool media_mode = false;
    wifi_ps_type_t saved_wifi_ps = WIFI_PS_MAX_MODEM;
};

static State s;

static emote_handle_t get_emote_handle()
{
    if (s.emote) return s.emote;

    Display *display = Board::GetInstance().GetDisplay();
    if (!display) {
        ESP_LOGE(TAG, "[UI] Board display is null");
        return nullptr;
    }

    auto *ed = dynamic_cast<emote::EmoteDisplay *>(display);
    if (!ed) {
        ESP_LOGE(TAG, "[UI] display is not EmoteDisplay");
        return nullptr;
    }

    s.display = ed;
    s.emote = ed->GetEmoteHandle();
    return s.emote;
}

static gfx_obj_t *label(emote_handle_t h, const char *name)
{
    return emote_create_obj_by_type(h, EMOTE_OBJ_TYPE_LABEL, name);
}

static gfx_obj_t *image(emote_handle_t h, const char *name)
{
    return emote_create_obj_by_type(h, EMOTE_OBJ_TYPE_IMAGE, name);
}

static void hide(gfx_obj_t *o)
{
    if (o) gfx_obj_set_visible(o, false);
}

static void show(gfx_obj_t *o)
{
    if (o) gfx_obj_set_visible(o, true);
}

static void set_text(gfx_obj_t *o, const char *str, uint32_t color,
                     int x, int y, int w, int h,
                     uint32_t bg, bool bg_on)
{
    if (!o) return;

    gfx_label_set_text(o, str ? str : "");
    gfx_label_set_color(o, GFX_COLOR_HEX(color));
    gfx_label_set_bg_color(o, GFX_COLOR_HEX(bg));
    gfx_label_set_bg_enable(o, bg_on);
    gfx_obj_set_pos(o, x, y);
    gfx_obj_set_size(o, w, h);
    gfx_obj_set_visible(o, true);
}

static void enter_media_network_mode()
{
    if (s.media_mode) return;
    if (esp_wifi_get_ps(&s.saved_wifi_ps) != ESP_OK) s.saved_wifi_ps = WIFI_PS_MAX_MODEM;
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "[NET] WiFi PS -> NONE (%s)", esp_err_to_name(err));
    s.media_mode = true;
}

static void maybe_leave_media_network_mode()
{
    if (!s.media_mode) return;
    if (s.player_task || s.video_rx_task || s.audio_task) return;
    Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(false);
    esp_wifi_set_ps(s.saved_wifi_ps);
    ESP_LOGI(TAG, "[NET] WiFi PS restored -> %d", (int)s.saved_wifi_ps);
    s.media_mode = false;
}

static bool alloc_jpeg_slots()
{
    if (!s.jpeg_queue) {
        s.jpeg_queue = xQueueCreate(BILI_JPEG_QUEUE_DEPTH, sizeof(uint8_t));
        if (!s.jpeg_queue) return false;
    }
    for (int i = 0; i < BILI_JPEG_SLOT_COUNT; ++i) {
        if (!s.jpeg_slots[i]) {
            s.jpeg_slots[i] = static_cast<uint8_t *>(
                heap_caps_malloc(BILI_JPEG_SLOT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!s.jpeg_slots[i]) return false;
        }
        s.jpeg_slot_len[i] = 0;
        s.jpeg_slot_state[i] = 0;
    }
    uint8_t idx;
    while (xQueueReceive(s.jpeg_queue, &idx, 0) == pdTRUE) {
        if (idx < BILI_JPEG_SLOT_COUNT) {
            s.jpeg_slot_state[idx] = 0;
            s.jpeg_slot_len[idx] = 0;
        }
    }
    return true;
}

static int acquire_jpeg_fill_slot()
{
    portENTER_CRITICAL(&s.jpeg_mux);
    for (int i = 0; i < BILI_JPEG_SLOT_COUNT; ++i) {
        if (s.jpeg_slot_state[i] == 0) {
            s.jpeg_slot_state[i] = 1;
            s.jpeg_slot_len[i] = 0;
            portEXIT_CRITICAL(&s.jpeg_mux);
            return i;
        }
    }
    portEXIT_CRITICAL(&s.jpeg_mux);

    uint8_t old = 0;
    if (xQueueReceive(s.jpeg_queue, &old, 0) == pdTRUE && old < BILI_JPEG_SLOT_COUNT) {
        portENTER_CRITICAL(&s.jpeg_mux);
        if (s.jpeg_slot_state[old] == 2) {
            s.jpeg_slot_state[old] = 1;
            s.jpeg_slot_len[old] = 0;
            portEXIT_CRITICAL(&s.jpeg_mux);
            return old;
        }
        portEXIT_CRITICAL(&s.jpeg_mux);
    }
    return -1;
}

static void queue_jpeg_slot(int slot)
{
    if (slot < 0 || slot >= BILI_JPEG_SLOT_COUNT) return;
    uint8_t idx = static_cast<uint8_t>(slot);
    portENTER_CRITICAL(&s.jpeg_mux);
    s.jpeg_slot_state[slot] = 2;
    portEXIT_CRITICAL(&s.jpeg_mux);

    if (xQueueSend(s.jpeg_queue, &idx, 0) == pdTRUE) return;

    uint8_t old = 0;
    if (xQueueReceive(s.jpeg_queue, &old, 0) == pdTRUE && old < BILI_JPEG_SLOT_COUNT) {
        portENTER_CRITICAL(&s.jpeg_mux);
        s.jpeg_slot_state[old] = 0;
        s.jpeg_slot_len[old] = 0;
        portEXIT_CRITICAL(&s.jpeg_mux);
    }

    if (xQueueSend(s.jpeg_queue, &idx, 0) != pdTRUE) {
        portENTER_CRITICAL(&s.jpeg_mux);
        s.jpeg_slot_state[slot] = 0;
        s.jpeg_slot_len[slot] = 0;
        portEXIT_CRITICAL(&s.jpeg_mux);
    }
}

static bool alloc_frame_buffers()
{
    for (int i = 0; i < BILI_FRAME_BUF_COUNT; ++i) {
        if (s.frame_buf[i]) continue;
        s.frame_buf[i] = static_cast<uint8_t *>(
            jpeg_calloc_align(BILI_FRAME_BUF_SIZE, 16)
        );
        if (!s.frame_buf[i]) {
            ESP_LOGE(TAG, "[Player] frame buffer %d alloc failed", i);
            for (int j = 0; j < BILI_FRAME_BUF_COUNT; ++j) {
                if (s.frame_buf[j]) {
                    jpeg_free_align(s.frame_buf[j]);
                    s.frame_buf[j] = nullptr;
                }
            }
            return false;
        }
    }

    for (int i = 0; i < BILI_FRAME_BUF_COUNT; ++i) {
        s.frame_dsc[i].header.magic = C_ARRAY_HEADER_MAGIC;
        s.frame_dsc[i].header.cf = GFX_COLOR_FORMAT_RGB565;
        s.frame_dsc[i].header.flags = 0;
        s.frame_dsc[i].header.w = BILI_VIDEO_W;
        s.frame_dsc[i].header.h = BILI_VIDEO_H;
        s.frame_dsc[i].header.stride = BILI_VIDEO_STRIDE;
        s.frame_dsc[i].header.reserved = 0;
        s.frame_dsc[i].data_size = BILI_FRAME_BUF_SIZE;
        s.frame_dsc[i].data = s.frame_buf[i];
        s.frame_dsc[i].reserved = nullptr;
        s.frame_dsc[i].reserved_2 = nullptr;
    }

    s.display_index = 0;
    s.previous_index = -1;
    s.pending_index = -1;
    s.ui_update_scheduled = false;
    return true;
}

static int acquire_decode_buffer()
{
    int chosen = -1;
    portENTER_CRITICAL(&s.frame_mux);
    for (int i = 0; i < BILI_FRAME_BUF_COUNT; ++i) {
        if (i == s.display_index || i == s.previous_index || i == s.pending_index) continue;
        chosen = i;
        break;
    }
    portEXIT_CRITICAL(&s.frame_mux);
    return chosen;
}

static void schedule_pending_frame_update()
{
    Application::GetInstance().Schedule([]() {
        int idx = -1;
        bool need_again = false;

        portENTER_CRITICAL(&s.frame_mux);
        idx = s.pending_index;
        s.pending_index = -1;
        if (idx >= 0) {
            // Reserve the new frame before touching GFX so the decoder can never
            // reuse the buffer while this callback is publishing it.
            s.previous_index = s.display_index;
            s.display_index = idx;
        }
        portEXIT_CRITICAL(&s.frame_mux);

        emote_handle_t h = get_emote_handle();
        if (h && idx >= 0 && s.active && s.player && !s.stop_player) {
            emote_lock(h);
            gfx_img_set_src(s.video_img, &s.frame_dsc[idx]);
            emote_notify_all_refresh(h);
            emote_unlock(h);

            portENTER_CRITICAL(&s.frame_mux);
            need_again = (s.pending_index >= 0);
            if (!need_again) {
                s.ui_update_scheduled = false;
            }
            portEXIT_CRITICAL(&s.frame_mux);
        } else {
            portENTER_CRITICAL(&s.frame_mux);
            s.ui_update_scheduled = false;
            need_again = (s.pending_index >= 0);
            portEXIT_CRITICAL(&s.frame_mux);
        }

        if (need_again) {
            schedule_pending_frame_update();
        }
    });
}

static bool init_ui_locked(emote_handle_t h)
{
    if (s.inited) return true;

    s.page = label(h, "bili_page");
    s.header = label(h, "bili_header");
    s.tabs = label(h, "bili_tabs");
    s.video_img = image(h, "bili_video_img");
    s.back = label(h, "bili_back");
    s.player_title = label(h, "bili_player_title");
    s.player_status = label(h, "bili_player_status");

    if (!s.page || !s.header || !s.tabs || !s.video_img ||
        !s.back || !s.player_title || !s.player_status) {
        ESP_LOGE(TAG, "[UI] create fixed object failed");
        return false;
    }

    for (int i = 0; i < BILI_ROW_COUNT; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "bili_row_%d", i);
        s.rows[i] = label(h, name);
        if (!s.rows[i]) {
            ESP_LOGE(TAG, "[UI] create row %d failed", i);
            return false;
        }
        gfx_label_set_long_mode(s.rows[i], GFX_LABEL_LONG_WRAP);
    }

    if (!alloc_frame_buffers()) return false;
    if (!alloc_jpeg_slots()) return false;

    // Full-screen opaque background.
    set_text(s.page, "", 0x202020, 0, 0, 360, 360, 0xF4F4F4, true);

    set_text(s.header, "哔哩哔哩", 0xFFFFFF, 0, 0, 360, 42, 0xFB7299, true);
    set_text(s.tabs, "首页       热门       推荐", 0x444444, 8, 44, 344, 30, 0xFFFFFF, true);

    for (int i = 0; i < BILI_ROW_COUNT; ++i) {
        set_text(s.rows[i], "", 0x202020, 8, 80 + i * 68, 344, 60,
                 (i % 2) ? 0xFFFFFF : 0xFAFAFA, true);
    }

    // Player image occupies the central 320x176 area.
    gfx_obj_set_pos(s.video_img, 20, 44);
    gfx_obj_set_size(s.video_img, BILI_VIDEO_W, BILI_VIDEO_H);

    set_text(s.back, "‹ 返回", 0xFFFFFF, 6, 6, 74, 30, 0xD94F79, true);
    set_text(s.player_title, "", 0xFFFFFF, 8, 228, 344, 56, 0x202020, true);
    set_text(s.player_status, "", 0xCCCCCC, 8, 288, 344, 28, 0x202020, true);

    hide(s.page);
    hide(s.header);
    hide(s.tabs);
    hide(s.video_img);
    hide(s.back);
    hide(s.player_title);
    hide(s.player_status);
    for (auto *r : s.rows) hide(r);

    s.inited = true;
    return true;
}

static void clear_all_locked()
{
    hide(s.page);
    hide(s.header);
    hide(s.tabs);
    hide(s.video_img);
    hide(s.back);
    hide(s.player_title);
    hide(s.player_status);
    for (auto *r : s.rows) hide(r);
}

static void enter_bili_locked(emote_handle_t h)
{
    emote_stop_anim_dialog(h);
    // Keep the original VoCat Emote animation running. The Bilibili page is
    // rendered as an opaque 360x360 overlay, so the face does not need to be
    // disabled and the base EmoteDisplay state remains intact.
    s.active = true;
}

static void show_loading_locked(emote_handle_t h)
{
    clear_all_locked();
    enter_bili_locked(h);
    s.player = false;
    s.selected = -1;

    set_text(s.page, "", 0x202020, 0, 0, 360, 360, 0xF4F4F4, true);
    set_text(s.header, "哔哩哔哩", 0xFFFFFF, 0, 0, 360, 42, 0xFB7299, true);
    set_text(s.tabs, "正在加载热门榜单...", 0x666666, 12, 54, 336, 30, 0xF4F4F4, true);

    show(s.page);
    show(s.header);
    show(s.tabs);
    emote_notify_all_refresh(h);
}

static void show_list_locked(emote_handle_t h)
{
    clear_all_locked();
    enter_bili_locked(h);
    s.player = false;
    s.selected = -1;

    set_text(s.tabs, "首页       热门       推荐", 0x444444, 8, 44, 344, 30, 0xFFFFFF, true);

    show(s.page);
    show(s.header);
    show(s.tabs);

    for (int i = 0; i < BILI_ROW_COUNT; ++i) {
        if (i < s.count) {
            char buf[240];
            snprintf(buf, sizeof(buf), "%s\n▶ %lu 次播放   %s",
                     s.videos[i].title,
                     (unsigned long)s.videos[i].play_count,
                     s.videos[i].bvid);
            set_text(s.rows[i], buf, 0x202020,
                     8, 80 + i * 68, 344, 60,
                     (i % 2) ? 0xFFFFFF : 0xFAFAFA, true);
            gfx_label_set_long_mode(s.rows[i], GFX_LABEL_LONG_WRAP);
            show(s.rows[i]);
        }
    }

    emote_notify_all_refresh(h);
}

static void update_player_status(const char *status)
{
    Application::GetInstance().Schedule([status_copy = std::string(status ? status : "")]() {
        emote_handle_t h = get_emote_handle();
        if (!h || !s.active || !s.player) return;
        emote_lock(h);
        set_text(s.player_status, status_copy.c_str(), 0xD0D0D0, 8, 288, 344, 28, 0x202020, true);
        show(s.player_status);
        emote_notify_all_refresh(h);
        emote_unlock(h);
    });
}

static void show_player_locked(emote_handle_t h, int index)
{
    if (index < 0 || index >= s.count) return;

    clear_all_locked();
    enter_bili_locked(h);
    s.player = true;
    s.selected = index;
    s.stop_player = false;
    s.stop_audio = false;

    // Give the video decoder the CPU headroom it needs while Bilibili media is playing.
    Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(true);
    s.audio_ready = false;
    s.audio_failed = false;

    set_text(s.page, "", 0x101010, 0, 0, 360, 360, 0x101010, true);
    show(s.page);
    show(s.video_img);
    show(s.back);
    show(s.player_title);
    show(s.player_status);

    set_text(s.player_title, s.videos[index].title,
             0xFFFFFF, 8, 228, 344, 56, 0x202020, true);
    set_text(s.player_status, "正在连接视频流...",
             0xD0D0D0, 8, 288, 344, 28, 0x202020, true);

    emote_notify_all_refresh(h);
}

static void stop_bili_audio()
{
    s.stop_audio = true;
    s.audio_ready = true;
    s.audio_failed = true;
    // Drop any queued Bilibili PCM immediately. The AudioService playback
    // worker may still be draining the previous chunk, but no new media
    // audio will be accepted after this point.
    Application::GetInstance().GetAudioService().ResetDecoder();
    maybe_leave_media_network_mode();
}

static void audio_task(void*)
{
    const int index = s.selected;
    if (index < 0 || index >= s.count) {
        s.audio_failed = true;
        s.audio_ready = true;
        s.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s%s", BILI_AUDIO_URL_BASE, s.videos[index].bvid);
    ESP_LOGI(TAG, "[Audio] open %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 32768;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[Audio] http init failed");
        s.audio_failed = true;
        s.audio_ready = true;
        s.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "[Audio] open failed");
        esp_http_client_cleanup(client);
        s.audio_failed = true;
        s.audio_ready = true;
        s.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[Audio] connected status=%d content_len=%lld", status, content_length);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s.audio_failed = true;
        s.audio_ready = true;
        s.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // IMPORTANT: keep all large buffers on heap, not on the FreeRTOS task stack.
    uint8_t* rx = static_cast<uint8_t*>(heap_caps_malloc(BILI_RX_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* pcm_bytes = static_cast<uint8_t*>(heap_caps_malloc(BILI_AUDIO_CHUNK_BYTES, MALLOC_CAP_8BIT));
    if (!rx || !pcm_bytes) {
        ESP_LOGE(TAG, "[Audio] heap buffer allocation failed");
        if (rx) heap_caps_free(rx);
        if (pcm_bytes) heap_caps_free(pcm_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s.audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    size_t pcm_used = 0;
    uint32_t chunks = 0;
    size_t prebuffer_samples = 0;
    std::vector<int16_t> prebuffer;
    prebuffer.reserve(BILI_AUDIO_CHUNK_SAMPLES * BILI_AUDIO_PREBUFFER_CHUNKS);

    auto& audio = Application::GetInstance().GetAudioService();

    while (!s.stop_audio && s.active && s.player) {
        const int64_t read_t0 = esp_timer_get_time();
        int n = esp_http_client_read(client, reinterpret_cast<char*>(rx), BILI_RX_CHUNK_SIZE);
        const int64_t read_us = esp_timer_get_time() - read_t0;
        if (read_us > 100000) {
            ESP_LOGW(TAG, "[BLOCK] [AudioRX] http_read blocked %lld ms, got %d",
                     (long long)(read_us / 1000), n);
        }

        if (n < 0) {
            if (n == -ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ESP_LOGE(TAG, "[Audio] stream read error=%d", n);
            s.audio_failed = true;
            break;
        }

        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGI(TAG, "[Audio] stream ended");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t off = 0;
        while (off < static_cast<size_t>(n) && !s.stop_audio && s.active && s.player) {
            const size_t copy_len = std::min(
                BILI_AUDIO_CHUNK_BYTES - pcm_used,
                static_cast<size_t>(n) - off);

            memcpy(pcm_bytes + pcm_used, rx + off, copy_len);
            pcm_used += copy_len;
            off += copy_len;

            if (pcm_used != BILI_AUDIO_CHUNK_BYTES) {
                continue;
            }

            /*
             * Keep the first 200 ms locally. Video is allowed to publish its
             * first frame only after this prebuffer is ready, so both streams
             * start from approximately the same media time.
             */
            if (prebuffer_samples < BILI_AUDIO_CHUNK_SAMPLES * BILI_AUDIO_PREBUFFER_CHUNKS) {
                const auto* src = reinterpret_cast<const int16_t*>(pcm_bytes);
                prebuffer.insert(
                    prebuffer.end(),
                    src,
                    src + BILI_AUDIO_CHUNK_SAMPLES);
                prebuffer_samples += BILI_AUDIO_CHUNK_SAMPLES;

                if (prebuffer_samples ==
                    BILI_AUDIO_CHUNK_SAMPLES * BILI_AUDIO_PREBUFFER_CHUNKS) {

                    for (int block = 0; block < BILI_AUDIO_PREBUFFER_CHUNKS; ++block) {
                        const int16_t* block_ptr =
                            prebuffer.data() + block * BILI_AUDIO_CHUNK_SAMPLES;

                        if (audio.PushPcmToPlaybackQueue(
                                block_ptr,
                                BILI_AUDIO_CHUNK_SAMPLES,
                                BILI_AUDIO_SAMPLE_RATE)) {
                            ++chunks;
                        } else {
                            ESP_LOGW(TAG, "[Audio] prebuffer chunk dropped");
                        }
                    }

                    prebuffer.clear();
                    s.audio_ready = true;
                }
            } else {
                const int16_t* block_ptr =
                    reinterpret_cast<const int16_t*>(pcm_bytes);

                if (audio.PushPcmToPlaybackQueue(
                        block_ptr,
                        BILI_AUDIO_CHUNK_SAMPLES,
                        BILI_AUDIO_SAMPLE_RATE)) {
                    ++chunks;
                }
            }

            if (chunks > 0 &&
                (chunks <= 2 || (chunks % 20) == 0)) {
                ESP_LOGI(TAG, "[Audio] PCM chunk=%lu",
                         (unsigned long)chunks);
            }

            pcm_used = 0;
        }
    }

    /*
     * If audio died before the 200 ms prebuffer completed, unblock the video
     * so the device can still show video without waiting forever.
     */
    if (!s.audio_ready) {
        s.audio_ready = true;
    }

    heap_caps_free(rx);
    heap_caps_free(pcm_bytes);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "[Audio] task exit chunks=%lu", (unsigned long)chunks);
    s.audio_task = nullptr;
    maybe_leave_media_network_mode();
    vTaskDelete(nullptr);
}

static bool start_audio_task(int index)
{
    if (s.audio_task != nullptr) return true;
    s.stop_audio = false;
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_task, "bili_audio", 8192, nullptr, 3, &s.audio_task, 0);
    if (ret != pdPASS) {
        s.audio_task = nullptr;
        ESP_LOGE(TAG, "[Audio] task create failed");
        return false;
    }
    ESP_LOGI(TAG, "[Audio] task created index=%d", index);
    return true;
}

static bool decode_and_publish(const uint8_t *jpeg, int jpeg_len)
{
    if (!jpeg || jpeg_len < 4 || jpeg_len > BILI_JPEG_SLOT_SIZE || !s.player || s.stop_player) return false;

    const int next = acquire_decode_buffer();
    if (next < 0) return false;
    uint8_t *out = s.frame_buf[next];
    if (!out) return false;

    uint8_t *decoded = nullptr;
    size_t decoded_len = 0, width = 0, height = 0, stride = 0;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t ret = jpeg_to_image(jpeg, (size_t)jpeg_len, &decoded, &decoded_len, &width, &height, &stride);
    const int64_t decode_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "[PERF] JPEG decode %lld ms", (long long)(decode_us / 1000));

    if (ret != ESP_OK || !decoded) {
        ESP_LOGW(TAG, "[Player] jpeg_to_image failed=%d size=%d", (int)ret, jpeg_len);
        return false;
    }
    if (width != BILI_VIDEO_W || height != BILI_VIDEO_H ||
        stride < BILI_VIDEO_STRIDE || decoded_len < (size_t)BILI_VIDEO_STRIDE * BILI_VIDEO_H) {
        jpeg_free_align(decoded);
        return false;
    }

    const size_t pixel_count = (size_t)BILI_VIDEO_W * BILI_VIDEO_H;
    const uint16_t *src = reinterpret_cast<const uint16_t *>(decoded);
    uint16_t *dst = reinterpret_cast<uint16_t *>(out);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t v = src[i];
        dst[i] = (uint16_t)((v << 8) | (v >> 8));
    }
    jpeg_free_align(decoded);

    s.frame_dsc[next].header.magic = C_ARRAY_HEADER_MAGIC;
    s.frame_dsc[next].header.cf = GFX_COLOR_FORMAT_RGB565;
    s.frame_dsc[next].header.flags = 0;
    s.frame_dsc[next].header.w = BILI_VIDEO_W;
    s.frame_dsc[next].header.h = BILI_VIDEO_H;
    s.frame_dsc[next].header.stride = BILI_VIDEO_STRIDE;
    s.frame_dsc[next].header.reserved = 0;
    s.frame_dsc[next].data_size = BILI_FRAME_BUF_SIZE;
    s.frame_dsc[next].data = out;
    s.frame_dsc[next].reserved = nullptr;
    s.frame_dsc[next].reserved_2 = nullptr;

    bool need_schedule = false;
    portENTER_CRITICAL(&s.frame_mux);
    if (s.pending_index >= 0) {
        const int old = s.pending_index;
        s.pending_index = -1;
        (void)old;
    }
    s.pending_index = next;
    if (!s.ui_update_scheduled) {
        s.ui_update_scheduled = true;
        need_schedule = true;
    }
    portEXIT_CRITICAL(&s.frame_mux);
    if (need_schedule) schedule_pending_frame_update();
    return true;
}

static void video_decode_task(void *)
{
    ESP_LOGI(TAG, "[VDEC] task start");
    uint32_t frames = 0, dropped = 0;
    while (!s.stop_player && s.active && s.player) {
        uint8_t slot = 0;
        if (xQueueReceive(s.jpeg_queue, &slot, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (slot >= BILI_JPEG_SLOT_COUNT) continue;

        size_t len = 0;
        portENTER_CRITICAL(&s.jpeg_mux);
        if (s.jpeg_slot_state[slot] != 2) { portEXIT_CRITICAL(&s.jpeg_mux); continue; }
        s.jpeg_slot_state[slot] = 3;
        len = s.jpeg_slot_len[slot];
        portEXIT_CRITICAL(&s.jpeg_mux);

        if (!s.stop_player && s.active && s.player && len) {
            if (decode_and_publish(s.jpeg_slots[slot], (int)len)) {
                ++frames;
                if (frames <= 3 || frames % 10 == 0)
                    ESP_LOGI(TAG, "[VDEC] frame=%lu q=%u", (unsigned long)frames,
                             (unsigned)uxQueueMessagesWaiting(s.jpeg_queue));
            } else {
                ++dropped;
            }
        }

        portENTER_CRITICAL(&s.jpeg_mux);
        s.jpeg_slot_state[slot] = 0;
        s.jpeg_slot_len[slot] = 0;
        portEXIT_CRITICAL(&s.jpeg_mux);
    }
    s.player_task = nullptr;
    ESP_LOGI(TAG, "[VDEC] exit frames=%lu dropped=%lu", (unsigned long)frames, (unsigned long)dropped);
    maybe_leave_media_network_mode();
    vTaskDelete(nullptr);
}

static void video_rx_task(void *)
{
    const int index = s.selected;
    esp_http_client_handle_t client = nullptr;
    uint8_t *rx = nullptr;
    int slot = -1;
    uint32_t frames = 0, dropped = 0;

    do {
        if (index < 0 || index >= s.count) break;
        if (!alloc_jpeg_slots()) break;

        char url[320];
        snprintf(url, sizeof(url), "%s%s", BILI_PLAY_URL_BASE, s.videos[index].bvid);
        ESP_LOGI(TAG, "[RX] open %s", url);

        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 32768;
        cfg.buffer_size_tx = 1024;
        cfg.keep_alive_enable = true;

        client = esp_http_client_init(&cfg);
        if (!client) break;
        if (esp_http_client_open(client, 0) != ESP_OK) break;

        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "[RX] connected status=%d content_len=%d", status, content_length);
        if (status != 200) break;

        rx = static_cast<uint8_t *>(heap_caps_malloc(BILI_RX_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rx) break;

        slot = acquire_jpeg_fill_slot();
        if (slot < 0) break;
        size_t used = 0;
        bool in_frame = false;
        bool prev_ff = false;

        while (!s.stop_player && s.active && s.player) {
            const int64_t t0 = esp_timer_get_time();
            int n = esp_http_client_read(client, reinterpret_cast<char *>(rx), BILI_RX_CHUNK_SIZE);
            const int64_t wait_us = esp_timer_get_time() - t0;
            if (wait_us > 100000) {
                ESP_LOGW(TAG, "[BLOCK] VideoRX read=%lldms n=%d q=%u", (long long)(wait_us/1000), n,
                         (unsigned)uxQueueMessagesWaiting(s.jpeg_queue));
            }
            if (n < 0) {
                if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
                break;
            }
            if (n == 0) {
                if (esp_http_client_is_complete_data_received(client)) break;
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            for (int i = 0; i < n; ++i) {
                const uint8_t b = rx[i];
                if (!in_frame) {
                    if (prev_ff && b == 0xD8) {
                        slot = acquire_jpeg_fill_slot();
                        if (slot < 0) { ++dropped; prev_ff = false; continue; }
                        used = 0;
                        s.jpeg_slots[slot][used++] = 0xFF;
                        s.jpeg_slots[slot][used++] = 0xD8;
                        in_frame = true;
                        prev_ff = false;
                        continue;
                    }
                    prev_ff = (b == 0xFF);
                    continue;
                }

                if (used >= BILI_JPEG_SLOT_SIZE) {
                    portENTER_CRITICAL(&s.jpeg_mux);
                    s.jpeg_slot_state[slot] = 0;
                    s.jpeg_slot_len[slot] = 0;
                    portEXIT_CRITICAL(&s.jpeg_mux);
                    ++dropped;
                    in_frame = false;
                    prev_ff = (b == 0xFF);
                    slot = -1;
                    continue;
                }
                s.jpeg_slots[slot][used++] = b;
                if (prev_ff && b == 0xD9) {
                    s.jpeg_slot_len[slot] = used;
                    queue_jpeg_slot(slot);
                    ++frames;
                    if (frames <= 3 || frames % 30 == 0)
                        ESP_LOGI(TAG, "[RX] queued=%lu q=%u dropped=%lu", (unsigned long)frames,
                                 (unsigned)uxQueueMessagesWaiting(s.jpeg_queue), (unsigned long)dropped);
                    in_frame = false;
                    prev_ff = false;
                    slot = -1;
                    continue;
                }
                prev_ff = (b == 0xFF);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    } while (false);

    if (slot >= 0 && slot < BILI_JPEG_SLOT_COUNT) {
        portENTER_CRITICAL(&s.jpeg_mux);
        if (s.jpeg_slot_state[slot] == 1) {
            s.jpeg_slot_state[slot] = 0;
            s.jpeg_slot_len[slot] = 0;
        }
        portEXIT_CRITICAL(&s.jpeg_mux);
    }
    if (rx) heap_caps_free(rx);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    s.stop_player = true;
    s.stop_audio = true;
    Application::GetInstance().GetAudioService().ResetDecoder();
    s.video_rx_task = nullptr;
    ESP_LOGI(TAG, "[RX] exit frames=%lu dropped=%lu", (unsigned long)frames, (unsigned long)dropped);
    maybe_leave_media_network_mode();
    vTaskDelete(nullptr);
}

static bool start_player_task(int index)
{
    if (s.player_task || s.video_rx_task) return true;
    enter_media_network_mode();
    s.selected = index;
    s.stop_player = false;
    s.stop_audio = false;
    if (!alloc_jpeg_slots()) return false;

    BaseType_t ret = xTaskCreatePinnedToCore(
        video_decode_task, "bili_vdec", BILI_PLAYER_STACK_SIZE,
        nullptr, 5, &s.player_task, 1);
    if (ret != pdPASS) { s.player_task = nullptr; return false; }

    ret = xTaskCreatePinnedToCore(
        video_rx_task, "bili_vrx", 10 * 1024,
        nullptr, 8, &s.video_rx_task, 1);
    if (ret != pdPASS) { s.video_rx_task = nullptr; s.stop_player = true; return false; }
    ESP_LOGI(TAG, "[Player] RX/Decode pipeline started index=%d", index);
    return true;
}

static void schedule_player(int index)
{
    Application::GetInstance().Schedule([index]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;

        emote_lock(h);
        if (init_ui_locked(h)) {
            show_player_locked(h, index);
        }
        emote_unlock(h);

        // Start audio first. The server-side media URL cache is shared between
        // /bili/play and /bili/audio, so both streams now start from the same
        // resolved DASH source without a second B站 playurl request.
        start_audio_task(index);
        start_player_task(index);
    });
}

static void schedule_list()
{
    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;
        emote_lock(h);
        if (init_ui_locked(h)) show_list_locked(h);
        emote_unlock(h);
    });
}

static void fetch_task(void *)
{
    bili_video_t tmp[BILI_RECORD_MAX] = {};
    uint8_t n = 0;

    if (vocat_bilibili_check_wifi()) {
        n = vocat_bilibili_get_recommend(tmp, BILI_RECORD_MAX);
    }

    if (n > 0) {
        memcpy(s.videos, tmp, sizeof(bili_video_t) * n);
        s.count = n;
        ESP_LOGI(TAG, "[Fetch] result count=%d", n);
        schedule_list();
    } else {
        ESP_LOGE(TAG, "[Fetch] result count=0");
        Application::GetInstance().Schedule([]() {
            emote_handle_t h = get_emote_handle();
            if (!h) return;
            emote_lock(h);
            if (init_ui_locked(h)) {
                clear_all_locked();
                enter_bili_locked(h);
                set_text(s.page, "", 0x202020, 0, 0, 360, 360, 0xF4F4F4, true);
                set_text(s.header, "哔哩哔哩", 0xFFFFFF, 0, 0, 360, 42, 0xFB7299, true);
                set_text(s.tabs, "B站数据获取失败，请检查电脑代理", 0xCC4444, 10, 54, 340, 32, 0xF4F4F4, true);
                show(s.page); show(s.header); show(s.tabs);
                emote_notify_all_refresh(h);
            }
            emote_unlock(h);
        });
    }

    s.fetch_task = nullptr;
    ESP_LOGI(TAG, "[Fetch] task exit");
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void vocat_bilibili_render_screen_async(void)
{
    if (s.active) {
        ESP_LOGI(TAG, "[UI] Bilibili already active");
        return;
    }

    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;
        emote_lock(h);
        if (init_ui_locked(h)) show_loading_locked(h);
        emote_unlock(h);
    });

    if (s.fetch_task != nullptr) return;

    BaseType_t ret = xTaskCreatePinnedToCore(
        fetch_task,
        "bili_fetch",
        BILI_TASK_STACK_SIZE,
        nullptr,
        3,
        &s.fetch_task,
        1
    );

    if (ret != pdPASS) {
        s.fetch_task = nullptr;
        ESP_LOGE(TAG, "[Fetch] create task failed");
    }
}

extern "C" void vocat_bilibili_ui_clear(void)
{
    s.stop_player = true;
    stop_bili_audio();

    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;

        emote_lock(h);
        if (s.inited) {
            clear_all_locked();
            s.active = false;
            s.player = false;
            s.selected = -1;
            emote_stop_anim_dialog(h);
            emote_notify_all_refresh(h);
        }
        emote_unlock(h);
    });
}

extern "C" bool vocat_bilibili_ui_handle_touch(int x, int y)
{
    if (!s.active) return false;

    ESP_LOGI(TAG, "[Touch] x=%d y=%d player=%d selected=%d", x, y, s.player, s.selected);

    if (s.player) {
        if (x <= 100 && y <= 50) {
            s.stop_player = true;
            stop_bili_audio();
            Application::GetInstance().Schedule([]() {
                emote_handle_t h = get_emote_handle();
                if (!h) return;
                emote_lock(h);
                if (init_ui_locked(h)) show_list_locked(h);
                emote_unlock(h);
            });
            return true;
        }
        return true;
    }

    if (y >= 80 && y < 80 + BILI_ROW_COUNT * 68) {
        int idx = (y - 80) / 68;
        if (idx >= 0 && idx < s.count) {
            ESP_LOGI(TAG, "[Touch] select row=%d bvid=%s", idx, s.videos[idx].bvid);
            schedule_player(idx);
        }
        return true;
    }

    // When Bilibili is active, swallow every touch so ToggleChatState is not called.
    return true;
}

extern "C" void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count)
{
    if (!videos) return;
    if (count > BILI_RECORD_MAX) count = BILI_RECORD_MAX;

    memcpy(s.videos, videos, sizeof(bili_video_t) * count);
    s.count = count;
    schedule_list();
}

extern "C" bool vocat_bilibili_render_screen(void)
{
    vocat_bilibili_render_screen_async();
    return true;
}

extern "C" bool vocat_bilibili_ui_is_active(void)
{
    return s.active;
}
