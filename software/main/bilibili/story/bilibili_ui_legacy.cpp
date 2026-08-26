#include "bilibili_ui.h"
#include "bilibili_audio.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "application.h"
#include "board.h"
#include "display/emote_display.h"
#include "esp_log.h"
#include "gfx.h"

#define TAG "BILI_STORY"

namespace {

constexpr int UI_W = 360;
constexpr int UI_H = 360;

constexpr int HEADER_X = 22;
constexpr int HEADER_Y = 6;
constexpr int HEADER_W = 316;
constexpr int HEADER_H = 44;

constexpr int LIST_X = 18;
constexpr int LIST_Y = 60;
constexpr int LIST_W = 324;
constexpr int ROW_H = 63;
constexpr int ROW_GAP = 6;
constexpr int LIST_VISIBLE = 4;

constexpr int PLAYER_BACK_X = 24;
constexpr int PLAYER_BACK_Y = 10;
constexpr int PLAYER_BACK_W = 42;
constexpr int PLAYER_BACK_H = 32;

constexpr int RECORD_X = 55;
constexpr int RECORD_Y = 55;
constexpr int RECORD_S = 250;

constexpr int TITLE_Y = 244;
constexpr int SUBTITLE_Y = 273;

constexpr int CTRL_Y = 306;
constexpr int PREV_X = 67;
constexpr int PLAY_X = 159;
constexpr int NEXT_X = 251;
constexpr int CTRL_S = 42;

enum class Page : uint8_t {
    Closed = 0,
    List,
    Player,
};

struct State {
    emote_handle_t emote = nullptr;

    bool initialized = false;
    bool active = false;
    bool playing = false;

    Page page = Page::Closed;

    bili_video_t videos[BILI_RECORD_MAX] = {};
    uint8_t count = 0;
    uint8_t selected = 0;
    uint8_t scroll = 0;

    char search_name[64] = {};

    gfx_obj_t *root = nullptr;
    gfx_obj_t *header = nullptr;
    gfx_obj_t *title = nullptr;
    gfx_obj_t *back = nullptr;
    gfx_obj_t *close = nullptr;

    gfx_obj_t *rows[BILI_RECORD_MAX] = {};

    gfx_obj_t *record_outer = nullptr;
    gfx_obj_t *record_mid = nullptr;
    gfx_obj_t *record_inner = nullptr;
    gfx_obj_t *record_text = nullptr;

    gfx_obj_t *player_title = nullptr;
    gfx_obj_t *player_status = nullptr;

    gfx_obj_t *prev = nullptr;
    gfx_obj_t *play = nullptr;
    gfx_obj_t *next = nullptr;
};

State s;

static gfx_obj_t *create_label(emote_handle_t h, const char *name) {
    return emote_create_obj_by_type(h, EMOTE_OBJ_TYPE_LABEL, name);
}

static void set_visible(gfx_obj_t *obj, bool visible) {
    if (obj) {
        gfx_obj_set_visible(obj, visible);
    }
}

static void set_label(gfx_obj_t *obj,
                      const char *value,
                      uint32_t fg,
                      int x,
                      int y,
                      int w,
                      int h,
                      uint32_t bg = 0,
                      bool bg_enable = false) {
    if (!obj) {
        return;
    }

    gfx_label_set_text(obj, value ? value : "");
    gfx_label_set_color(obj, GFX_COLOR_HEX(fg));
    gfx_label_set_bg_color(obj, GFX_COLOR_HEX(bg));
    gfx_label_set_bg_enable(obj, bg_enable);

    gfx_obj_set_pos(obj, x, y);
    gfx_obj_set_size(obj, w, h);
    gfx_obj_set_visible(obj, true);
}

static bool inside(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width &&
           y >= top && y < top + height;
}

static std::string clip_utf8(const char *src, size_t max_chars) {
    if (!src) {
        return {};
    }

    std::string value(src);
    size_t pos = 0;
    size_t count = 0;

    while (pos < value.size()) {
        const uint8_t c = static_cast<uint8_t>(value[pos]);
        size_t step = 1;

        if (c < 0x80) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else {
            step = 4;
        }

        if (count >= max_chars) {
            value.resize(pos);
            value += "...";
            break;
        }

        pos += std::min(step, value.size() - pos);
        ++count;
    }

    return value;
}

static emote_handle_t get_emote_handle() {
    if (s.emote) {
        return s.emote;
    }

    Display *display = Board::GetInstance().GetDisplay();
    if (!display) {
        return nullptr;
    }

    auto *emote_display = dynamic_cast<emote::EmoteDisplay *>(display);
    if (!emote_display) {
        return nullptr;
    }

    s.emote = emote_display->GetEmoteHandle();
    return s.emote;
}

static void hide_page_objects() {
    set_visible(s.header, false);
    set_visible(s.title, false);
    set_visible(s.back, false);
    set_visible(s.close, false);

    for (auto *row : s.rows) {
        set_visible(row, false);
    }

    set_visible(s.record_outer, false);
    set_visible(s.record_mid, false);
    set_visible(s.record_inner, false);
    set_visible(s.record_text, false);
    set_visible(s.player_title, false);
    set_visible(s.player_status, false);

    set_visible(s.prev, false);
    set_visible(s.play, false);
    set_visible(s.next, false);
}

static void create_objects(emote_handle_t h) {
    if (s.initialized) {
        return;
    }

    s.root = create_label(h, "story_bili_root");
    s.header = create_label(h, "story_bili_header");
    s.title = create_label(h, "story_bili_title");
    s.back = create_label(h, "story_bili_back");
    s.close = create_label(h, "story_bili_close");

    for (int i = 0; i < BILI_RECORD_MAX; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "story_bili_row_%d", i);
        s.rows[i] = create_label(h, name);
    }

    s.record_outer = create_label(h, "story_bili_record_outer");
    s.record_mid = create_label(h, "story_bili_record_mid");
    s.record_inner = create_label(h, "story_bili_record_inner");
    s.record_text = create_label(h, "story_bili_record_text");

    s.player_title = create_label(h, "story_bili_player_title");
    s.player_status = create_label(h, "story_bili_player_status");

    s.prev = create_label(h, "story_bili_prev");
    s.play = create_label(h, "story_bili_play");
    s.next = create_label(h, "story_bili_next");

    s.initialized =
        s.root &&
        s.header &&
        s.title &&
        s.back &&
        s.close &&
        s.record_outer &&
        s.record_mid &&
        s.record_inner &&
        s.record_text &&
        s.player_title &&
        s.player_status &&
        s.prev &&
        s.play &&
        s.next;

    for (auto *row : s.rows) {
        s.initialized = s.initialized && (row != nullptr);
    }
}

static void draw_header(const char *heading) {
    set_label(s.header, "", 0x2C3138,
              HEADER_X, HEADER_Y, HEADER_W, HEADER_H,
              0xF7F8FA, true);

    set_label(s.title, heading, 0x22272D,
              76, 12, 190, 25);

    set_label(s.back, "<", 0xFB7299,
              36, 13, 30, 28);

    set_label(s.close, "x", 0xFB7299,
              306, 13, 28, 28);
}

static void draw_list() {
    hide_page_objects();
    set_label(s.root, "", 0, 0, 0, UI_W, UI_H, 0xF1F3F6, true);
    char heading[96];
    if (s.search_name[0] != '\0') snprintf(heading, sizeof(heading), "Bilibili · %s", s.search_name);
    else snprintf(heading, sizeof(heading), "Bilibili");
    draw_header(heading);
    if (s.count == 0) {
        set_label(s.rows[0], "暂无视频", 0x69717D, 72, 150, 216, 44, 0xFFFFFF, true);
        set_visible(s.rows[0], true);
    } else {
        for (uint8_t slot = 0; slot < LIST_VISIBLE; ++slot) {
            const uint8_t index = static_cast<uint8_t>(s.scroll + slot);
            if (index >= s.count) break;
            const int y = LIST_Y + slot * (ROW_H + ROW_GAP);
            const std::string title = clip_utf8(s.videos[index].title, 24);
            char line[180];
            snprintf(line, sizeof(line), "%s\n%lu 次播放", title.c_str(), static_cast<unsigned long>(s.videos[index].play_count));
            const uint32_t bg = (index == s.selected) ? 0xFFF0F5 : 0xFFFFFF;
            set_label(s.rows[slot], line, 0x23282F, LIST_X, y, LIST_W, ROW_H, bg, true);
            gfx_label_set_long_mode(s.rows[slot], GFX_LABEL_LONG_WRAP);
        }
    }
    set_visible(s.root, true);
}

static void draw_record() {
    /*
     * Stage 1 uses pure GFX labels for the record.
     * This avoids introducing an asset partition dependency.
     * The same objects can later be replaced by animated images.
     */
    set_label(s.record_outer, "●",
              0x30353B,
              RECORD_X, RECORD_Y, RECORD_S, RECORD_S,
              0x111419, true);

    set_label(s.record_mid, "●",
              0x1C2025,
              RECORD_X + 27, RECORD_Y + 27,
              RECORD_S - 54, RECORD_S - 54,
              0x080A0D, true);

    set_label(s.record_inner, "●",
              0x454A51,
              RECORD_X + 76, RECORD_Y + 76,
              RECORD_S - 152, RECORD_S - 152,
              0x24282D, true);

    set_label(s.record_text, "VINYL",
              0xFB7299,
              RECORD_X + 92, RECORD_Y + 105,
              66, 26,
              0x342027, true);
}

static void draw_player() {
    hide_page_objects();

    set_label(s.root, "", 0,
              0, 0, UI_W, UI_H,
              0x0B0D10, true);

    set_label(s.back, "<", 0xFB7299,
              PLAYER_BACK_X, PLAYER_BACK_Y,
              PLAYER_BACK_W, PLAYER_BACK_H);

    set_label(s.close, "x", 0xFB7299,
              306, 13, 28, 28);

    draw_record();

    std::string title = "Bilibili";
    if (s.count > 0 && s.selected < s.count) {
        title = clip_utf8(s.videos[s.selected].title, 28);
    }

    set_label(s.player_title, title.c_str(),
              0xF3F5F7,
              24, TITLE_Y, 312, 26,
              0x171A1F, true);

    set_label(s.player_status,
              s.playing ? "PLAYING" : "PAUSED",
              s.playing ? 0x66E99A : 0xC7CDD3,
              135, SUBTITLE_Y, 90, 20);

    set_label(s.prev, "|<",
              0xF4F6F8,
              PREV_X, CTRL_Y, CTRL_S, CTRL_S,
              0x252A31, true);

    set_label(s.play,
              s.playing ? "||" : ">",
              0xFB7299,
              PLAY_X, CTRL_Y, CTRL_S, CTRL_S,
              0x332128, true);

    set_label(s.next, ">|",
              0xF4F6F8,
              NEXT_X, CTRL_Y, CTRL_S, CTRL_S,
              0x252A31, true);

    set_visible(s.root, true);
}

static void refresh() {
    emote_handle_t h = get_emote_handle();
    if (!h || !s.active) {
        return;
    }

    emote_lock(h);

    if (s.page == Page::List) {
        draw_list();
    } else if (s.page == Page::Player) {
        draw_player();
    }

    emote_notify_all_refresh(h);
    emote_unlock(h);
}

static void audio_eof_cb(void *) {
    Application::GetInstance().Schedule([]() {
        if (s.active && s.page == Page::Player) bilibili_story_next();
    });
}

static bool start_audio_for_selected() {
    return s.selected < s.count && bilibili_audio_start(s.videos[s.selected].bvid, audio_eof_cb, nullptr);
}

}  // namespace

extern "C" bool bilibili_story_open(void) {
    if (s.active) {
        return true;
    }

    s.active = true;
    s.page = Page::List;
    s.playing = false;
    s.selected = 0;
    s.scroll = 0;
    s.count = 0;
    bilibili_audio_stop();

    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) {
            return;
        }

        emote_lock(h);

        create_objects(h);
        if (!s.initialized) {
            ESP_LOGE(TAG, "create Bilibili Story UI objects failed");
            emote_unlock(h);
            return;
        }

        draw_list();

        emote_notify_all_refresh(h);
        emote_unlock(h);
    });

    return true;
}

extern "C" void bilibili_story_close(void) {
    bilibili_audio_stop();
    s.active = false;
    s.page = Page::Closed;
    s.playing = false;

    Application::GetInstance().Schedule([]() {
        emote_handle_t h = get_emote_handle();
        if (!h) {
            return;
        }

        emote_lock(h);

        hide_page_objects();
        set_visible(s.root, false);

        emote_notify_all_refresh(h);
        emote_unlock(h);
    });
}

extern "C" bool bilibili_story_is_active(void) {
    return s.active;
}

extern "C" void bilibili_story_search(const char *up_name) {
    if (!up_name || up_name[0] == '\0') return;
    if (!s.active && !bilibili_story_open()) return;
    strncpy(s.search_name, up_name, sizeof(s.search_name)-1);
    s.search_name[sizeof(s.search_name)-1] = '\0';
    bilibili_audio_stop();
    bili_video_t results[BILI_RECORD_MAX] = {};
    const uint8_t count = vocat_bilibili_search_up(up_name, results, BILI_RECORD_MAX);
    bilibili_story_show_list(results, count);
}

extern "C" void bilibili_story_show_list(const bili_video_t *videos,
                                          uint8_t count) {
    if (!s.active) {
        return;
    }

    count = std::min<uint8_t>(count, BILI_RECORD_MAX);

    if (videos && count > 0) {
        memcpy(s.videos, videos, sizeof(bili_video_t) * count);
        s.count = count;
    }

    if (s.selected >= s.count && s.count > 0) {
        s.selected = 0;
    }

    bilibili_audio_stop();
    s.page = Page::List;
    s.playing = false;
    s.scroll = 0;

    refresh();
}

extern "C" void bilibili_story_show_player(uint8_t index) {
    if (!s.active || index >= s.count) {
        return;
    }

    bilibili_audio_stop();
    s.selected = index;
    s.page = Page::Player;
    s.playing = start_audio_for_selected();

    refresh();
}

extern "C" void bilibili_story_set_playing(bool playing) {
    if (!s.active || s.page != Page::Player) {
        return;
    }

    s.playing = playing;
    bilibili_audio_set_paused(!playing);
    refresh();
}

extern "C" void bilibili_story_set_track(uint8_t index) {
    if (!s.active || index >= s.count) {
        return;
    }

    bilibili_story_show_player(index);
}

extern "C" void bilibili_story_previous(void) {
    if (!s.active || s.count == 0) return;
    bilibili_story_show_player(s.selected == 0 ? static_cast<uint8_t>(s.count-1) : static_cast<uint8_t>(s.selected-1));
}

extern "C" void bilibili_story_next(void) {
    if (!s.active || s.count == 0) return;
    bilibili_story_show_player(static_cast<uint8_t>((s.selected + 1) % s.count));
}

extern "C" void bilibili_story_back(void) {
    if (!s.active) {
        return;
    }

    if (s.page == Page::Player) {
        bilibili_audio_stop();
        s.page = Page::List;
        s.playing = false;
        refresh();
        return;
    }

    bilibili_story_close();
}

extern "C" bool bilibili_story_handle_touch(int x, int y) {
    if (!s.active) {
        return false;
    }

    if (s.page == Page::List) {
        if (inside(x, y,
                   292, 3, 60, 52)) {
            bilibili_story_close();
            return true;
        }

        if (inside(x, y,
                   16, 3, 62, 52)) {
            bilibili_story_close();
            return true;
        }

        for (uint8_t slot = 0; slot < LIST_VISIBLE; ++slot) {
            const uint8_t i = static_cast<uint8_t>(s.scroll + slot);
            if (i >= s.count) break;
            const int row_y = LIST_Y + slot * (ROW_H + ROW_GAP);

            if (inside(x, y,
                       LIST_X, row_y,
                       LIST_W, ROW_H)) {
                bilibili_story_show_player(i);
                return true;
            }
        }

        return true;
    }

    if (s.page == Page::Player) {
        if (inside(x, y,
                   PLAYER_BACK_X - 8,
                   PLAYER_BACK_Y - 8,
                   PLAYER_BACK_W + 16,
                   PLAYER_BACK_H + 16)) {
            s.page = Page::List;
            s.playing = false;
            refresh();
            return true;
        }

        if (inside(x, y, 292, 3, 60, 52)) {
            bilibili_story_close();
            return true;
        }

        if (inside(x, y,
                   PREV_X - 7,
                   CTRL_Y - 7,
                   CTRL_S + 14,
                   CTRL_S + 14)) {
            bilibili_story_previous();
            return true;
        }

        if (inside(x, y,
                   PLAY_X - 7,
                   CTRL_Y - 7,
                   CTRL_S + 14,
                   CTRL_S + 14)) {
            bilibili_story_set_playing(!s.playing);
            return true;
        }

        if (inside(x, y,
                   NEXT_X - 7,
                   CTRL_Y - 7,
                   CTRL_S + 14,
                   CTRL_S + 14)) {
            bilibili_story_next();
            return true;
        }

        return true;
    }

    return true;
}

extern "C" bool bilibili_story_handle_swipe(int, int start_y, int, int end_y) {
    if (!s.active || s.page != Page::List || s.count <= LIST_VISIBLE) return false;
    const int dy = end_y - start_y;
    if (std::abs(dy) < 20) return false;
    if (dy < 0 && s.scroll + LIST_VISIBLE < s.count) ++s.scroll;
    else if (dy > 0 && s.scroll > 0) --s.scroll;
    refresh();
    return true;
}

extern "C" void vocat_bilibili_render_screen_async(void) {
    bilibili_story_open();
}

extern "C" bool vocat_bilibili_render_screen(void) {
    return bilibili_story_open();
}

extern "C" void vocat_bilibili_ui_clear(void) {
    bilibili_story_close();
}

extern "C" void vocat_bilibili_ui_draw(const bili_video_t *videos,
                                        uint8_t count) {
    if (!s.active) {
        bilibili_story_open();
    }

    bilibili_story_show_list(videos, count);
}

extern "C" bool vocat_bilibili_ui_handle_touch(int x, int y) {
    return bilibili_story_handle_touch(x, y);
}

extern "C" bool vocat_bilibili_ui_is_active(void) {
    return bilibili_story_is_active();
}
