#include "vocat_bilibili_ui.hpp"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "display/emote_display.h"
#include "expression_emote.h"
#include "gfx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bili_h264_player.h"

#define TAG "BILI_UI"
#define BILI_TASK_STACK_SIZE (10 * 1024)
#define BILI_ROW_COUNT BILI_RECORD_MAX

#define UI_W 360
#define UI_H 360
#define BILI_VIDEO_W 320
#define BILI_VIDEO_H 176
#define BILI_FRAME_BYTES (BILI_VIDEO_W * BILI_VIDEO_H * 2)

// Coordinates intentionally remain in the positions already confirmed correct.
#define PLAYER_BACK_X 159
#define PLAYER_BACK_Y 8
#define PLAYER_BACK_SIZE 42
#define PLAYER_X 20
#define PLAYER_Y 70
#define PLAYER_TITLE_Y 254
#define PLAYER_STATUS_Y 308

#define LIST_START_Y 72
#define LIST_ROW_H 62
#define LIST_ROW_GAP 6
#define LIST_ROW_X 24
#define LIST_ROW_W 312

namespace {

enum class View : uint8_t { None = 0, Loading, List, Error, Player };

struct State {
    emote::EmoteDisplay *display = nullptr;
    emote_handle_t emote = nullptr;

    gfx_obj_t *page = nullptr;
    gfx_obj_t *header = nullptr;
    gfx_obj_t *title = nullptr;
    gfx_obj_t *back = nullptr;
    gfx_obj_t *back_shadow = nullptr;
    gfx_obj_t *close = nullptr;
    gfx_obj_t *close_shadow = nullptr;
    gfx_obj_t *accent = nullptr;
    gfx_obj_t *status_dot = nullptr;
    gfx_obj_t *rows[BILI_ROW_COUNT] = {};
    gfx_obj_t *video_img = nullptr;
    gfx_obj_t *player_title = nullptr;
    gfx_obj_t *player_status = nullptr;
    gfx_obj_t *player_time = nullptr;

    uint8_t *frame_buf[2] = {};
    gfx_image_dsc_t frame_dsc[2] = {};
    int frame_front = 0;
    int frame_back = 1;
    bool frame_pending = false;
    bool frame_schedule_pending = false;
    portMUX_TYPE frame_mux = portMUX_INITIALIZER_UNLOCKED;

    bool inited = false;
    bool active = false;
    bool player = false;
    View view = View::None;
    bili_video_t videos[BILI_ROW_COUNT] = {};
    uint8_t count = 0;
    int selected = -1;
    TaskHandle_t fetch_task = nullptr;
    uint32_t generation = 0;
};

State s;

static gfx_obj_t *label(emote_handle_t h, const char *name) {
    return emote_create_obj_by_type(h, EMOTE_OBJ_TYPE_LABEL, name);
}
static gfx_obj_t *image(emote_handle_t h, const char *name) {
    return emote_create_obj_by_type(h, EMOTE_OBJ_TYPE_IMAGE, name);
}
static void hide(gfx_obj_t *o) { if (o) gfx_obj_set_visible(o, false); }
static void show(gfx_obj_t *o) { if (o) gfx_obj_set_visible(o, true); }

static void set_label(gfx_obj_t *o, const char *text, uint32_t fg,
                      int x, int y, int w, int h, uint32_t bg, bool bg_on) {
    if (!o) return;
    gfx_label_set_text(o, text ? text : "");
    gfx_label_set_color(o, GFX_COLOR_HEX(fg));
    gfx_label_set_bg_color(o, GFX_COLOR_HEX(bg));
    gfx_label_set_bg_enable(o, bg_on);
    gfx_obj_set_pos(o, x, y);
    gfx_obj_set_size(o, w, h);
    gfx_obj_set_visible(o, true);
}

static bool hit(int x, int y, int l, int t, int w, int h) {
    return x >= l && x < l + w && y >= t && y < t + h;
}

static std::string ellipsize_utf8(const char *src, size_t max_chars) {
    if (!src) return {};
    std::string t(src);
    size_t n = 0;
    for (size_t i = 0; i < t.size();) {
        const uint8_t c = static_cast<uint8_t>(t[i]);
        size_t step = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (n >= max_chars) { t.resize(i); t += "..."; break; }
        i += std::min(step, t.size() - i);
        ++n;
    }
    return t;
}

static emote_handle_t get_emote_handle() {
    if (s.emote) return s.emote;
    Display *display = Board::GetInstance().GetDisplay();
    if (!display) return nullptr;
    auto *ed = dynamic_cast<emote::EmoteDisplay *>(display);
    if (!ed) return nullptr;
    s.display = ed;
    s.emote = ed->GetEmoteHandle();
    return s.emote;
}

static void make_frame_desc(int i) {
    s.frame_dsc[i].header.magic = C_ARRAY_HEADER_MAGIC;
    s.frame_dsc[i].header.cf = GFX_COLOR_FORMAT_RGB565;
    s.frame_dsc[i].header.flags = 0;
    s.frame_dsc[i].header.w = BILI_VIDEO_W;
    s.frame_dsc[i].header.h = BILI_VIDEO_H;
    s.frame_dsc[i].header.stride = BILI_VIDEO_W * 2;
    s.frame_dsc[i].header.reserved = 0;
    s.frame_dsc[i].data_size = BILI_FRAME_BYTES;
    s.frame_dsc[i].data = s.frame_buf[i];
    s.frame_dsc[i].reserved = nullptr;
    s.frame_dsc[i].reserved_2 = nullptr;
}

static bool alloc_frames() {
    for (int i = 0; i < 2; ++i) {
        if (!s.frame_buf[i]) {
            s.frame_buf[i] = static_cast<uint8_t *>(
                heap_caps_malloc(BILI_FRAME_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (!s.frame_buf[i]) {
            ESP_LOGE(TAG, "[UI] DMA frame buffer %d alloc failed", i);
            return false;
        }
        make_frame_desc(i);
    }
    return true;
}

static bool init_locked(emote_handle_t h) {
    if (s.inited) return true;

    s.page = label(h, "bili_page");
    s.header = label(h, "bili_header");
    s.title = label(h, "bili_title");
    s.back = label(h, "bili_back");
    s.back_shadow = label(h, "bili_back_shadow");
    s.close = label(h, "bili_close");
    s.close_shadow = label(h, "bili_close_shadow");
    s.accent = label(h, "bili_accent");
    s.status_dot = label(h, "bili_status_dot");
    s.video_img = image(h, "bili_video_img");
    s.player_title = label(h, "bili_player_title");
    s.player_status = label(h, "bili_player_status");
    s.player_time = label(h, "bili_player_time");

    if (!s.page || !s.header || !s.title || !s.back || !s.back_shadow ||
        !s.close || !s.close_shadow || !s.accent || !s.status_dot ||
        !s.video_img || !s.player_title || !s.player_status || !s.player_time) {
        ESP_LOGE(TAG, "[UI] create fixed object failed");
        return false;
    }

    for (int i = 0; i < BILI_ROW_COUNT; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "bili_row_%d", i);
        s.rows[i] = label(h, name);
        if (!s.rows[i]) return false;
        gfx_label_set_long_mode(s.rows[i], GFX_LABEL_LONG_WRAP);
    }
    if (!alloc_frames()) return false;

    // Player image is always the confirmed 320x176 window.
    gfx_obj_set_pos(s.video_img, PLAYER_X, PLAYER_Y);
    gfx_obj_set_size(s.video_img, BILI_VIDEO_W, BILI_VIDEO_H);

    s.inited = true;
    return true;
}

static void clear_overlay_locked() {
    hide(s.header); hide(s.title); hide(s.back); hide(s.back_shadow);
    hide(s.close); hide(s.close_shadow); hide(s.accent); hide(s.status_dot);
    hide(s.video_img); hide(s.player_title); hide(s.player_status); hide(s.player_time);
    for (auto *r : s.rows) hide(r);
    show(s.page);
}

static void glass_top_locked(const char *heading, bool back, bool close, bool dark) {
    const uint32_t panel = dark ? 0x171B21 : 0xE9EDF2;
    const uint32_t fg = dark ? 0xF2F4F7 : 0x343A43;
    const uint32_t accent = dark ? 0x7FD6C5 : 0xFB7299;

    // Frosted-card feel: softly separated surface, no hard button rectangle.
    set_label(s.header, "", fg, 40, 5, 280, 42, panel, true);
    set_label(s.accent, "", dark ? 0x343A42 : 0xD8DEE6, 78, 46, 204, 1,
              dark ? 0x343A42 : 0xD8DEE6, true);
    show(s.header);
    show(s.accent);

    if (heading && *heading) {
        set_label(s.title, heading, fg, 78, 10, 204, 26, panel, false);
        gfx_label_set_long_mode(s.title, GFX_LABEL_LONG_CLIP);
        show(s.title);
    } else {
        hide(s.title);
    }

    // Confirmed position retained. ASCII glyphs avoid missing-glyph font issues.
    if (back) {
        set_label(s.back, "<", accent, PLAYER_BACK_X, PLAYER_BACK_Y,
                  PLAYER_BACK_SIZE, PLAYER_BACK_SIZE, 0x000000, false);
        show(s.back);
    } else hide(s.back);

    if (close) {
        set_label(s.close, "x", accent, 305, 8, 28, 28, 0x000000, false);
        show(s.close);
    } else hide(s.close);

    hide(s.back_shadow);
    hide(s.close_shadow);
}

static void show_loading_locked(emote_handle_t h) {
    clear_overlay_locked();
    s.view = View::Loading; s.player = false; s.selected = -1;
    set_label(s.page, "", 0, 0, 0, UI_W, UI_H, 0xF0F2F6, true);
    glass_top_locked("哔哩哔哩", true, true, false);
    set_label(s.rows[0], "正在加载", 0x4F5660, 70, 144, 220, 40, 0xE9ECF1, true);
    set_label(s.rows[1], "·  ·  ·", 0xA9B0BA, 120, 190, 120, 28, 0xF0F2F6, false);
    show(s.rows[0]); show(s.rows[1]);
    emote_notify_all_refresh(h);
}

static void show_error_locked(emote_handle_t h, const char *msg) {
    clear_overlay_locked();
    s.view = View::Error; s.player = false;
    set_label(s.page, "", 0, 0, 0, UI_W, UI_H, 0xF0F2F6, true);
    glass_top_locked("哔哩哔哩", true, true, false);
    set_label(s.rows[0], "!", 0xFB7299, 146, 132, 68, 48, 0xF0F2F6, false);
    set_label(s.rows[1], msg ? msg : "暂时无法加载", 0x545B65, 54, 200, 252, 42, 0xE9ECF1, true);
    show(s.rows[0]); show(s.rows[1]);
    emote_notify_all_refresh(h);
}

static void show_list_locked(emote_handle_t h) {
    clear_overlay_locked();
    s.view = View::List; s.player = false; s.selected = -1;
    set_label(s.page, "", 0, 0, 0, UI_W, UI_H, 0xF3F5F8, true);
    glass_top_locked("哔哩哔哩", true, true, false);

    for (int i = 0; i < BILI_ROW_COUNT; ++i) {
        if (i >= s.count) continue;
        const int y = LIST_START_Y + i * (LIST_ROW_H + LIST_ROW_GAP);
        const std::string t = ellipsize_utf8(s.videos[i].title, 24);
        char buf[192];
        snprintf(buf, sizeof(buf), "%s\n%lu 次播放", t.c_str(),
                 (unsigned long)s.videos[i].play_count);
        // Low-contrast alternating surfaces emulate a frosted card.
        set_label(s.rows[i], buf, 0x20252B, LIST_ROW_X, y, LIST_ROW_W, LIST_ROW_H,
                  i == 0 ? 0xF8F9FB : 0xE8ECF2, true);
        gfx_label_set_long_mode(s.rows[i], GFX_LABEL_LONG_WRAP);
        show(s.rows[i]);
    }
    if (s.count == 0) {
        set_label(s.rows[0], "暂无内容", 0x69717C, 70, 150, 220, 44, 0xE8ECF2, true);
        show(s.rows[0]);
    }
    emote_notify_all_refresh(h);
}

static void show_player_locked(emote_handle_t h, int idx) {
    if (idx < 0 || idx >= s.count) return;
    clear_overlay_locked();
    s.view = View::Player; s.player = true; s.selected = idx;
    set_label(s.page, "", 0, 0, 0, UI_W, UI_H, 0x0B0D10, true);
    glass_top_locked("", true, true, true);

    gfx_obj_set_pos(s.video_img, PLAYER_X, PLAYER_Y);
    gfx_obj_set_size(s.video_img, BILI_VIDEO_W, BILI_VIDEO_H);
    hide(s.video_img);

    const std::string title = ellipsize_utf8(s.videos[idx].title, 28);
    // Bright centered title directly under the video.
    set_label(s.player_title, title.c_str(), 0xF2F4F6,
              24, PLAYER_TITLE_Y, 312, 44, 0x171B21, true);
    gfx_label_set_long_mode(s.player_title, GFX_LABEL_LONG_WRAP);
    show(s.player_title);

    // Dark translucent-style pill, centered below the title.
    set_label(s.player_status, "视频连接中", 0xCFD6DE,
              92, PLAYER_STATUS_Y, 176, 24, 0x20262D, true);
    set_label(s.status_dot, "•", 0x65E394,
              111, PLAYER_STATUS_Y - 1, 18, 24, 0x171C22, true);
    set_label(s.player_time, "", 0x66707A, 140, PLAYER_STATUS_Y + 28, 80, 16, 0x000000, false);
    show(s.player_status); show(s.status_dot); show(s.player_time);

    ESP_LOGI(TAG, "[UI] Player layout back=(%d,%d) video=(%d,%d,%d,%d) title_y=%d status_y=%d",
             PLAYER_BACK_X, PLAYER_BACK_Y, PLAYER_X, PLAYER_Y, BILI_VIDEO_W, BILI_VIDEO_H,
             PLAYER_TITLE_Y, PLAYER_STATUS_Y);
    emote_notify_all_refresh(h);
}

static void update_status_ui(const char *status_text) {
    const std::string text = status_text ? status_text : "";
    Application::GetInstance().Schedule([text]() {
        emote_handle_t h = get_emote_handle();
        if (!h || !s.active || !s.player || s.view != View::Player) return;
        emote_lock(h);
        set_label(s.player_status, text.c_str(), 0xC7D0D9,
                  92, PLAYER_STATUS_Y, 176, 24, 0x171C22, true);
        const bool ok = text.find("连接") != std::string::npos || text.find("显示") != std::string::npos || text.find("播放") != std::string::npos;
        set_label(s.status_dot, "•", ok ? 0x65E394 : 0xF3C86B,
                  111, PLAYER_STATUS_Y - 1, 18, 24, 0x171C22, true);
        show(s.status_dot);
        emote_notify_all_refresh(h);
        emote_unlock(h);
    });
}

static void schedule_frame_refresh() {
    portENTER_CRITICAL(&s.frame_mux);
    if (s.frame_schedule_pending) { portEXIT_CRITICAL(&s.frame_mux); return; }
    s.frame_schedule_pending = true;
    portEXIT_CRITICAL(&s.frame_mux);

    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h || !s.active || !s.player || s.view != View::Player) {
            portENTER_CRITICAL(&s.frame_mux);
            s.frame_schedule_pending = false;
            portEXIT_CRITICAL(&s.frame_mux);
            return;
        }
        int front = 0;
        bool again = false;
        portENTER_CRITICAL(&s.frame_mux);
        if (s.frame_pending) {
            front = s.frame_back;
            s.frame_front = front;
            s.frame_pending = false;
            s.frame_back = 1 - front;
        }
        portEXIT_CRITICAL(&s.frame_mux);

        const int64_t t0 = esp_timer_get_time();
        emote_lock(h);
        gfx_img_set_src(s.video_img, &s.frame_dsc[front]);
        show(s.video_img);
        emote_notify_all_refresh(h);
        emote_unlock(h);
        const int64_t dt = esp_timer_get_time() - t0;
        if (dt > 30000) ESP_LOGW(TAG, "[UI][FRAME] refresh BLOCK %lldms", (long long)(dt / 1000));

        portENTER_CRITICAL(&s.frame_mux);
        again = s.frame_pending;
        s.frame_schedule_pending = false;
        portEXIT_CRITICAL(&s.frame_mux);
        if (again) schedule_frame_refresh();
    });
}

static void player_frame_cb(const bili_player_frame_t *frame, void *) {
    if (!frame || !frame->rgb565 || frame->width != BILI_VIDEO_W || frame->height != BILI_VIDEO_H) return;
    int back;
    portENTER_CRITICAL(&s.frame_mux);
    back = s.frame_back;
    portEXIT_CRITICAL(&s.frame_mux);
    if (!s.frame_buf[back]) return;
    const int64_t t0 = esp_timer_get_time();
    memcpy(s.frame_buf[back], frame->rgb565, BILI_FRAME_BYTES);
    const int64_t dt = esp_timer_get_time() - t0;
    if (dt > 20000) ESP_LOGW(TAG, "[UI][FRAME] memcpy BLOCK %lldms", (long long)(dt / 1000));
    portENTER_CRITICAL(&s.frame_mux);
    s.frame_pending = true;
    portEXIT_CRITICAL(&s.frame_mux);
    schedule_frame_refresh();
}

static void player_status_cb(const char *text, void *) { update_status_ui(text); }

static void exit_bilibili_async() {
    bili_player_stop();
    ++s.generation;
    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;
        emote_lock(h);
        clear_overlay_locked();
        hide(s.page);
        s.active = false; s.player = false; s.view = View::None; s.selected = -1;
        emote_notify_all_refresh(h);
        emote_unlock(h);
    });
}

static void schedule_list() {
    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;
        emote_lock(h);
        if (s.inited && s.active) show_list_locked(h);
        emote_unlock(h);
    });
}

static void fetch_task(void *) {
    const uint32_t gen = s.generation;
    bili_video_t tmp[BILI_RECORD_MAX] = {};
    uint8_t n = 0;
    ESP_LOGI(TAG, "[LIST] fetch start generation=%u", gen);
    if (vocat_bilibili_check_wifi()) n = vocat_bilibili_get_recommend(tmp, BILI_RECORD_MAX);
    ESP_LOGI(TAG, "[LIST] fetch result count=%u generation=%u current=%u", n, gen, s.generation);
    if (gen != s.generation || !s.active) { s.fetch_task = nullptr; vTaskDelete(nullptr); return; }
    if (n) { memcpy(s.videos, tmp, sizeof(bili_video_t) * n); s.count = n; schedule_list(); }
    else {
        Application::GetInstance().Schedule([]() {
            emote_handle_t h = get_emote_handle();
            if (!h) return;
            emote_lock(h); if (s.inited && s.active) show_error_locked(h, "无法获取视频列表"); emote_unlock(h);
        });
    }
    s.fetch_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void vocat_bilibili_render_screen_async(void) {
    if (s.active || s.fetch_task) return;
    ++s.generation;
    s.active = true; s.player = false; s.view = View::Loading; s.selected = -1;
    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) return;
        emote_lock(h); if (init_locked(h)) show_loading_locked(h); emote_unlock(h);
    });
    if (xTaskCreatePinnedToCore(fetch_task, "bili_fetch", BILI_TASK_STACK_SIZE, nullptr, 3, &s.fetch_task, 1) != pdPASS) {
        s.fetch_task = nullptr;
        Application::GetInstance().Schedule([]() {
            emote_handle_t h = get_emote_handle(); if (!h) return;
            emote_lock(h); if (s.inited) show_error_locked(h, "界面加载失败"); emote_unlock(h);
        });
    }
}

extern "C" bool vocat_bilibili_render_screen(void) { vocat_bilibili_render_screen_async(); return true; }
extern "C" void vocat_bilibili_ui_clear(void) { exit_bilibili_async(); }

extern "C" bool vocat_bilibili_ui_handle_touch(int x, int y) {
    ESP_LOGI(TAG, "[TOUCH] x=%d y=%d view=%d active=%d", x, y, (int)s.view, s.active ? 1 : 0);
    if (!s.active) return false;

    if (s.view == View::Player && hit(x, y, PLAYER_BACK_X - 8, PLAYER_BACK_Y - 8, PLAYER_BACK_SIZE + 16, PLAYER_BACK_SIZE + 16)) {
        ESP_LOGI(TAG, "[TOUCH] player back -> nonblocking stop + list");
        bili_player_stop();
        schedule_list();
        return true;
    }

    if (s.view != View::Player && hit(x, y, 148, 0, 64, 58)) {
        exit_bilibili_async();
        return true;
    }
    if (hit(x, y, 296, 0, 52, 52)) {
        exit_bilibili_async();
        return true;
    }

    if (s.view == View::Player) return true;

    if (s.view == View::List) {
        for (int i = 0; i < BILI_ROW_COUNT; ++i) {
            const int y0 = LIST_START_Y + i * (LIST_ROW_H + LIST_ROW_GAP);
            if (hit(x, y, LIST_ROW_X, y0, LIST_ROW_W, LIST_ROW_H) && i < s.count) {
                const int idx = i;
                const uint32_t gen = s.generation;
                Application::GetInstance().Schedule([idx, gen]() {
                    if (!s.active || gen != s.generation) return;
                    emote_handle_t h = get_emote_handle(); if (!h) return;
                    emote_lock(h); if (s.inited) show_player_locked(h, idx); emote_unlock(h);
                    bili_player_start(s.videos[idx].bvid, player_frame_cb, player_status_cb, nullptr);
                });
                return true;
            }
        }
    }
    return true;
}

extern "C" void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count) {
    if (!videos) return;
    count = std::min<uint8_t>(count, BILI_RECORD_MAX);
    memcpy(s.videos, videos, sizeof(bili_video_t) * count);
    s.count = count;
    schedule_list();
}
extern "C" bool vocat_bilibili_ui_is_active(void) { return s.active; }
