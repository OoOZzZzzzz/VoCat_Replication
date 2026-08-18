#include "vocat_bilibili_ui.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
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
#define BILI_PLAY_URL_BASE "http://192.168.31.106:8000/bili/play?bvid="

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
    int selected = -1;

    bili_video_t videos[BILI_ROW_COUNT] = {};
    uint8_t count = 0;

    TaskHandle_t fetch_task = nullptr;
    TaskHandle_t player_task = nullptr;
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

static bool start_player_task(int index);

static bool decode_and_publish(const uint8_t *jpeg, int jpeg_len)
{
    if (!jpeg || jpeg_len < 4 || jpeg_len > BILI_JPEG_BUF_SIZE ||
        !s.player || s.stop_player) {
        return false;
    }

    // Latest-frame-wins: never let the producer block behind the UI.
    // If all buffers are busy, drop this frame instead of accumulating latency.
    const int next = acquire_decode_buffer();
    if (next < 0) {
        return false;
    }

    uint8_t *out = s.frame_buf[next];
    if (!out) return false;

    uint8_t *decoded = nullptr;
    size_t decoded_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;

    esp_err_t ret = jpeg_to_image(
        jpeg,
        static_cast<size_t>(jpeg_len),
        &decoded,
        &decoded_len,
        &width,
        &height,
        &stride
    );

    if (ret != ESP_OK || decoded == nullptr) {
        ESP_LOGW(TAG, "[Player] jpeg_to_image failed=%d size=%d", (int)ret, jpeg_len);
        return false;
    }

    if (width != BILI_VIDEO_W || height != BILI_VIDEO_H ||
        stride < BILI_VIDEO_STRIDE ||
        decoded_len < static_cast<size_t>(BILI_VIDEO_STRIDE) * BILI_VIDEO_H) {
        ESP_LOGW(TAG,
                 "[Player] decoded size unexpected: %zux%zu stride=%zu len=%zu",
                 width, height, stride, decoded_len);
        jpeg_free_align(decoded);
        return false;
    }

    const size_t pixel_count =
        static_cast<size_t>(BILI_VIDEO_W) * BILI_VIDEO_H;

    const uint16_t *src =
        reinterpret_cast<const uint16_t *>(decoded);
    uint16_t *dst =
        reinterpret_cast<uint16_t *>(out);

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t v = src[i];
        dst[i] = static_cast<uint16_t>(
            static_cast<uint16_t>(v << 8) |
            static_cast<uint16_t>(v >> 8)
        );
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
    // If a newer frame is already waiting, this older one should never be queued.
    // Replacing pending_index is the latency-control mechanism.
    s.pending_index = next;
    if (!s.ui_update_scheduled) {
        s.ui_update_scheduled = true;
        need_schedule = true;
    }
    portEXIT_CRITICAL(&s.frame_mux);

    if (need_schedule) {
        schedule_pending_frame_update();
    }

    return true;
}

static void player_task(void *)
{
    const int index = s.selected;
    if (index < 0 || index >= s.count) {
        s.player_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s%s", BILI_PLAY_URL_BASE, s.videos[index].bvid);

    ESP_LOGI(TAG, "[Player] open %s", url);
    update_player_status("正在连接视频流...");

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 16 * 1024;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[Player] http init failed");
        update_player_status("视频连接失败");
        s.player_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[Player] open failed=%d", ret);
        esp_http_client_cleanup(client);
        update_player_status("视频连接失败");
        s.player_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "[Player] connected status=%d content_len=%lld",
             esp_http_client_get_status_code(client), content_length);

    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        update_player_status("服务器返回错误");
        s.player_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    update_player_status("视频加载中...");

    uint8_t *jpeg_buf = static_cast<uint8_t *>(
        heap_caps_malloc(BILI_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (!jpeg_buf) {
        jpeg_buf = static_cast<uint8_t *>(malloc(BILI_JPEG_BUF_SIZE));
    }

    if (!jpeg_buf) {
        ESP_LOGE(TAG, "[Player] JPEG buffer alloc failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        update_player_status("内存不足");
        s.player_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int used = 0;
    bool first_frame = true;
    uint8_t rx[8192];

    while (!s.stop_player && s.active && s.player) {
        int n = esp_http_client_read(client, reinterpret_cast<char *>(rx), sizeof(rx));
        if (n < 0) {
            ESP_LOGE(TAG, "[Player] stream read error=%d", n);
            update_player_status("视频流读取失败");
            break;
        }

        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGI(TAG, "[Player] stream ended");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (used + n > BILI_JPEG_BUF_SIZE) {
            ESP_LOGW(TAG, "[Player] JPEG buffer overflow, reset parser");
            used = 0;
        }

        memcpy(jpeg_buf + used, rx, n);
        used += n;

        while (!s.stop_player && s.active && s.player) {
            int start = -1;
            for (int i = 0; i + 1 < used; ++i) {
                if (jpeg_buf[i] == 0xFF && jpeg_buf[i + 1] == 0xD8) {
                    start = i;
                    break;
                }
            }

            if (start < 0) {
                if (used > 1) {
                    jpeg_buf[0] = jpeg_buf[used - 1];
                    used = 1;
                }
                break;
            }

            if (start > 0) {
                memmove(jpeg_buf, jpeg_buf + start, used - start);
                used -= start;
            }

            int end = -1;
            for (int i = 2; i + 1 < used; ++i) {
                if (jpeg_buf[i] == 0xFF && jpeg_buf[i + 1] == 0xD9) {
                    end = i + 1;
                    break;
                }
            }

            if (end < 0) break;

            int frame_len = end + 1;
            if (decode_and_publish(jpeg_buf, frame_len)) {
                if (first_frame) {
                    first_frame = false;
                    update_player_status("");
                }
            }

            if (frame_len < used) {
                memmove(jpeg_buf, jpeg_buf + frame_len, used - frame_len);
                used -= frame_len;
            } else {
                used = 0;
            }
        }
    }

    free(jpeg_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "[Player] task exit");
    s.player_task = nullptr;
    vTaskDelete(nullptr);
}

static bool start_player_task(int index)
{
    if (s.player_task != nullptr) return true;

    s.selected = index;
    s.stop_player = false;

    BaseType_t ret = xTaskCreatePinnedToCore(
        player_task,
        "bili_player",
        BILI_PLAYER_STACK_SIZE,
        nullptr,
        4,
        &s.player_task,
        1
    );

    if (ret != pdPASS) {
        s.player_task = nullptr;
        ESP_LOGE(TAG, "[Player] task create failed");
        return false;
    }

    ESP_LOGI(TAG, "[Player] task created index=%d", index);
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
            emote_set_anim_visible(h, true);
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
