#include <cstdlib>
#include "bilibili_ui.h"
#include "bilibili_audio.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_display.h"
#include "esp_log.h"
#include <lvgl.h>

#define TAG "BILI_STORY_LVGL"

namespace {

constexpr int SCREEN_W = 360;
constexpr int SCREEN_H = 360;
constexpr int SAFE_W = 300;

constexpr uint32_t COLOR_BG = 0x0B0F14;
constexpr uint32_t COLOR_PANEL = 0x121923;
constexpr uint32_t COLOR_PANEL_2 = 0x1A2330;
constexpr uint32_t COLOR_TEXT = 0xF3F5F7;
constexpr uint32_t COLOR_MUTED = 0x9EA8B5;
constexpr uint32_t COLOR_ACCENT = 0xFB7299;
constexpr uint32_t COLOR_ACCENT_DARK = 0x452331;
constexpr uint32_t COLOR_SUCCESS = 0x58D68D;

constexpr int ROW_HEIGHT = 54;
constexpr int ROW_GAP = 6;

enum class Page : uint8_t {
    Closed = 0,
    List,
    Player,
};

struct State {
    bool active = false;
    bool playing = false;
    Page page = Page::Closed;

    bili_video_t videos[BILI_RECORD_MAX] = {};
    uint8_t count = 0;
    uint8_t selected = 0;

    lv_obj_t *root = nullptr;
    lv_obj_t *list_page = nullptr;
    lv_obj_t *player_page = nullptr;
    lv_obj_t *list_container = nullptr;

    lv_obj_t *player_title = nullptr;
    lv_obj_t *player_status = nullptr;

    lv_obj_t *record = nullptr;
    lv_obj_t *record_hole = nullptr;
    lv_obj_t *record_center = nullptr;

    lv_obj_t *prev_button = nullptr;
    lv_obj_t *play_button = nullptr;
    lv_obj_t *next_button = nullptr;

    lv_anim_t record_anim{};
    bool record_anim_active = false;
};

State s;

static lv_color_t rgb(uint32_t value)
{
    return lv_color_hex(value);
}

static LvglDisplay *display()
{
    return dynamic_cast<LvglDisplay *>(
        Board::GetInstance().GetDisplay()
    );
}

static std::string clip_utf8(
    const char *text,
    size_t max_chars
)
{
    if (!text) {
        return {};
    }

    std::string result(text);
    size_t pos = 0;
    size_t count = 0;

    while (pos < result.size()) {
        const uint8_t c =
            static_cast<uint8_t>(
                result[pos]
            );

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
            result.resize(pos);
            result += "...";
            break;
        }

        pos += std::min(
            step,
            result.size() - pos
        );

        ++count;
    }

    return result;
}

static void style_page(
    lv_obj_t *page
)
{
    lv_obj_remove_style_all(
        page
    );

    lv_obj_set_size(
        page,
        SCREEN_W,
        SCREEN_H
    );

    lv_obj_center(
        page
    );

    lv_obj_set_style_bg_color(
        page,
        rgb(COLOR_BG),
        0
    );

    lv_obj_set_style_bg_opa(
        page,
        LV_OPA_COVER,
        0
    );

    lv_obj_clear_flag(
        page,
        LV_OBJ_FLAG_SCROLLABLE
    );
}

static lv_obj_t *make_button(
    lv_obj_t *parent,
    const char *text,
    int width,
    int height,
    uint32_t background
)
{
    lv_obj_t *button =
        lv_button_create(
            parent
        );

    lv_obj_set_size(
        button,
        width,
        height
    );

    lv_obj_set_style_radius(
        button,
        LV_RADIUS_CIRCLE,
        0
    );

    lv_obj_set_style_bg_color(
        button,
        rgb(background),
        0
    );

    lv_obj_set_style_bg_color(
        button,
        rgb(COLOR_ACCENT_DARK),
        LV_STATE_PRESSED
    );

    lv_obj_set_style_border_width(
        button,
        0,
        0
    );

    lv_obj_set_style_shadow_width(
        button,
        0,
        0
    );

    lv_obj_t *label =
        lv_label_create(
            button
        );

    lv_label_set_text(
        label,
        text
    );

    lv_obj_set_style_text_color(
        label,
        rgb(COLOR_TEXT),
        0
    );

    lv_obj_center(
        label
    );

    return button;
}

static void delete_page(
    lv_obj_t *page
)
{
    if (page) {
        lv_obj_delete(
            page
        );
    }
}

static void stop_record_animation()
{
    if (!s.record_anim_active ||
        !s.record) {
        return;
    }

    lv_anim_delete(
        s.record,
        nullptr
    );

    s.record_anim_active = false;
}

static void record_anim_cb(
    void *obj,
    int32_t angle
)
{
    lv_obj_t *record =
        static_cast<lv_obj_t *>(obj);

    lv_obj_set_style_transform_angle(
        record,
        angle,
        0
    );
}

static void start_record_animation()
{
    if (!s.record) {
        return;
    }

    stop_record_animation();

    lv_anim_init(
        &s.record_anim
    );

    lv_anim_set_var(
        &s.record_anim,
        s.record
    );

    lv_anim_set_values(
        &s.record_anim,
        0,
        3600
    );

    lv_anim_set_duration(
        &s.record_anim,
        900
    );

    lv_anim_set_exec_cb(
        &s.record_anim,
        record_anim_cb
    );

    lv_anim_set_repeat_count(
        &s.record_anim,
        LV_ANIM_REPEAT_INFINITE
    );

    lv_anim_start(
        &s.record_anim
    );

    s.record_anim_active = true;
}

static void update_play_button()
{
    if (!s.play_button) {
        return;
    }

    lv_obj_t *label =
        lv_obj_get_child(
            s.play_button,
            0
        );

    if (!label) {
        return;
    }

    lv_label_set_text(
        label,
        s.playing ? "||" : "▶"
    );
}

static void close_event(
    lv_event_t *event
)
{
    LV_UNUSED(event);
    bilibili_story_close();
}

static void back_event(
    lv_event_t *event
)
{
    LV_UNUSED(event);
    bilibili_story_back();
}

static void previous_event(
    lv_event_t *event
)
{
    LV_UNUSED(event);
    bilibili_story_previous();
}

static void play_event(
    lv_event_t *event
)
{
    LV_UNUSED(event);

    bilibili_story_set_playing(
        !s.playing
    );
}

static void next_event(
    lv_event_t *event
)
{
    LV_UNUSED(event);
    bilibili_story_next();
}

static void item_event(
    lv_event_t *event
)
{
    const int index =
        static_cast<int>(
            reinterpret_cast<intptr_t>(
                lv_event_get_user_data(
                    event
                )
            )
        );

    if (
        index >= 0 &&
        index < s.count
    ) {
        bilibili_story_show_player(
            static_cast<uint8_t>(
                index
            )
        );
    }
}

static void create_header(
    lv_obj_t *page,
    const char *title,
    const char *subtitle,
    bool close_button
)
{
    lv_obj_t *header =
        lv_obj_create(page);

    lv_obj_remove_style_all(
        header
    );

    lv_obj_set_size(
        header,
        300,
        52
    );

    lv_obj_align(
        header,
        LV_ALIGN_TOP_MID,
        0,
        7
    );

    lv_obj_set_style_bg_color(
        header,
        rgb(COLOR_PANEL),
        0
    );

    lv_obj_set_style_bg_opa(
        header,
        LV_OPA_COVER,
        0
    );

    lv_obj_t *left =
        make_button(
            header,
            close_button ? "×" : "‹",
            36,
            36,
            COLOR_PANEL_2
        );

    lv_obj_align(
        left,
        LV_ALIGN_LEFT_MID,
        4,
        0
    );

    lv_obj_add_event_cb(
        left,
        close_button
            ? close_event
            : back_event,
        LV_EVENT_CLICKED,
        nullptr
    );

    lv_obj_t *label =
        lv_label_create(header);

    lv_label_set_text(
        label,
        title
    );

    lv_obj_set_style_text_color(
        label,
        rgb(COLOR_TEXT),
        0
    );

    lv_obj_set_width(
        label,
        210
    );

    lv_label_set_long_mode(
        label,
        LV_LABEL_LONG_MODE_CLIP
    );

    lv_obj_align(
        label,
        LV_ALIGN_CENTER,
        0,
        subtitle ? -8 : 0
    );

    if (subtitle) {
        lv_obj_t *sub =
            lv_label_create(header);

        lv_label_set_text(
            sub,
            subtitle
        );

        lv_obj_set_style_text_color(
            sub,
            rgb(COLOR_MUTED),
            0
        );

        lv_obj_set_width(
            sub,
            250
        );

        lv_label_set_long_mode(
            sub,
            LV_LABEL_LONG_MODE_CLIP
        );

        lv_obj_align(
            sub,
            LV_ALIGN_CENTER,
            0,
            12
        );
    }
}

static void create_list_page()
{
    s.list_page =
        lv_obj_create(
            s.root
        );

    style_page(
        s.list_page
    );

    create_header(
        s.list_page,
        "B站视频",
        "点击播放 · 上下滑动",
        true
    );

    s.list_container =
        lv_obj_create(
            s.list_page
        );

    lv_obj_remove_style_all(
        s.list_container
    );

    lv_obj_set_size(
        s.list_container,
        292,
        250
    );

    lv_obj_align(
        s.list_container,
        LV_ALIGN_TOP_MID,
        0,
        62
    );

    lv_obj_set_flex_flow(
        s.list_container,
        LV_FLEX_FLOW_COLUMN
    );

    lv_obj_set_style_pad_all(
        s.list_container,
        0,
        0
    );

    lv_obj_set_style_pad_bottom(
        s.list_container,
        12,
        0
    );

    lv_obj_set_style_pad_row(
        s.list_container,
        ROW_GAP,
        0
    );

    lv_obj_set_style_bg_opa(
        s.list_container,
        LV_OPA_TRANSP,
        0
    );

    lv_obj_set_scroll_dir(
        s.list_container,
        LV_DIR_VER
    );

    lv_obj_set_scrollbar_mode(
        s.list_container,
        LV_SCROLLBAR_MODE_AUTO
    );

    for (
        uint8_t i = 0;
        i < s.count;
        ++i
    ) {
        lv_obj_t *item =
            lv_button_create(
                s.list_container
            );

        lv_obj_set_width(
            item,
            LV_PCT(100)
        );

        lv_obj_set_height(
            item,
            ROW_HEIGHT
        );

        lv_obj_set_style_radius(
            item,
            14,
            0
        );

        lv_obj_set_style_bg_color(
            item,
            rgb(COLOR_PANEL),
            0
        );

        lv_obj_set_style_bg_color(
            item,
            rgb(COLOR_ACCENT_DARK),
            LV_STATE_PRESSED
        );

        lv_obj_set_style_border_width(
            item,
            0,
            0
        );

        lv_obj_set_style_shadow_width(
            item,
            0,
            0
        );

        lv_obj_add_event_cb(
            item,
            item_event,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(
                static_cast<intptr_t>(
                    i
                )
            )
        );

        lv_obj_t *number =
            lv_label_create(item);

        char number_text[8];
        snprintf(
            number_text,
            sizeof(number_text),
            "%02u",
            static_cast<unsigned>(
                i + 1
            )
        );

        lv_label_set_text(
            number,
            number_text
        );

        lv_obj_set_style_text_color(
            number,
            rgb(COLOR_ACCENT),
            0
        );

        lv_obj_align(
            number,
            LV_ALIGN_LEFT_MID,
            12,
            0
        );

        lv_obj_t *title =
            lv_label_create(item);

        std::string title_text =
            clip_utf8(
                s.videos[i].title,
                25
            );

        lv_label_set_text(
            title,
            title_text.c_str()
        );

        lv_obj_set_width(
            title,
            210
        );

        lv_label_set_long_mode(
            title,
            LV_LABEL_LONG_MODE_CLIP
        );

        lv_obj_set_style_text_color(
            title,
            rgb(COLOR_TEXT),
            0
        );

        lv_obj_align(
            title,
            LV_ALIGN_LEFT_MID,
            48,
            -7
        );

        char play_count[32];
        snprintf(
            play_count,
            sizeof(play_count),
            "%lu",
            static_cast<unsigned long>(
                s.videos[i].play_count
            )
        );

        lv_obj_t *meta =
            lv_label_create(item);

        lv_label_set_text(
            meta,
            play_count
        );

        lv_obj_set_style_text_color(
            meta,
            rgb(COLOR_MUTED),
            0
        );

        lv_obj_align(
            meta,
            LV_ALIGN_LEFT_MID,
            48,
            14
        );
    }
}

static void create_player_page()
{
    s.player_page =
        lv_obj_create(
            s.root
        );

    style_page(
        s.player_page
    );

    create_header(
        s.player_page,
        "B站播放器",
        nullptr,
        false
    );

    s.record =
        lv_obj_create(
            s.player_page
        );

    lv_obj_remove_style_all(
        s.record
    );

    lv_obj_set_size(
        s.record,
        190,
        190
    );

    lv_obj_set_style_radius(
        s.record,
        LV_RADIUS_CIRCLE,
        0
    );

    lv_obj_set_style_bg_color(
        s.record,
        rgb(0x151A21),
        0
    );

    lv_obj_set_style_border_width(
        s.record,
        10,
        0
    );

    lv_obj_set_style_border_color(
        s.record,
        rgb(0x252C36),
        0
    );

    lv_obj_set_style_shadow_width(
        s.record,
        18,
        0
    );

    lv_obj_set_style_shadow_opa(
        s.record,
        LV_OPA_30,
        0
    );

    lv_obj_align(
        s.record,
        LV_ALIGN_TOP_MID,
        0,
        62
    );

    const uint32_t ring_colors[] = {
        0x0D1117,
        0x1B2028,
        0x2D333D,
    };

    const int ring_sizes[] = {
        164,
        120,
        74,
    };

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *ring =
            lv_obj_create(
                s.record
            );

        lv_obj_remove_style_all(
            ring
        );

        lv_obj_set_size(
            ring,
            ring_sizes[i],
            ring_sizes[i]
        );

        lv_obj_set_style_radius(
            ring,
            LV_RADIUS_CIRCLE,
            0
        );

        lv_obj_set_style_bg_color(
            ring,
            rgb(ring_colors[i]),
            0
        );

        lv_obj_center(
            ring
        );
    }

    s.record_hole =
        lv_obj_create(
            s.record
        );

    lv_obj_remove_style_all(
        s.record_hole
    );

    lv_obj_set_size(
        s.record_hole,
        34,
        34
    );

    lv_obj_set_style_radius(
        s.record_hole,
        LV_RADIUS_CIRCLE,
        0
    );

    lv_obj_set_style_bg_color(
        s.record_hole,
        rgb(COLOR_ACCENT_DARK),
        0
    );

    lv_obj_center(
        s.record_hole
    );

    s.record_center =
        lv_label_create(
            s.record
        );

    lv_label_set_text(
        s.record_center,
        "♪"
    );

    lv_obj_set_style_text_color(
        s.record_center,
        rgb(COLOR_ACCENT),
        0
    );

    lv_obj_center(
        s.record_center
    );

    const char *title =
        (
            s.count > 0 &&
            s.selected < s.count
        )
        ? s.videos[s.selected].title
        : "Bilibili";

    s.player_title =
        lv_label_create(
            s.player_page
        );

    std::string clipped_title =
        clip_utf8(
            title,
            28
        );

    lv_label_set_text(
        s.player_title,
        clipped_title.c_str()
    );

    lv_obj_set_width(
        s.player_title,
        320
    );

    lv_label_set_long_mode(
        s.player_title,
        LV_LABEL_LONG_MODE_CLIP
    );

    lv_obj_set_style_text_color(
        s.player_title,
        rgb(COLOR_TEXT),
        0
    );

    lv_obj_set_style_text_align(
        s.player_title,
        LV_TEXT_ALIGN_CENTER,
        0
    );

    lv_obj_align(
        s.player_title,
        LV_ALIGN_TOP_MID,
        0,
        266
    );

    s.player_status =
        lv_label_create(
            s.player_page
        );

    lv_label_set_text(
        s.player_status,
        s.playing
            ? "正在播放"
            : "已暂停"
    );

    lv_obj_set_style_text_color(
        s.player_status,
        s.playing
            ? rgb(COLOR_SUCCESS)
            : rgb(COLOR_MUTED),
        0
    );

    lv_obj_align(
        s.player_status,
        LV_ALIGN_TOP_MID,
        0,
        290
    );

    s.prev_button =
        make_button(
            s.player_page,
            "‹",
            44,
            44,
            COLOR_PANEL_2
        );

    lv_obj_align(
        s.prev_button,
        LV_ALIGN_BOTTOM_LEFT,
        58,
        -5
    );

    lv_obj_add_event_cb(
        s.prev_button,
        previous_event,
        LV_EVENT_CLICKED,
        nullptr
    );

    s.play_button =
        make_button(
            s.player_page,
            s.playing ? "||" : "▶",
            54,
            54,
            COLOR_ACCENT
        );

    lv_obj_align(
        s.play_button,
        LV_ALIGN_BOTTOM_MID,
        0,
        -18
    );

    lv_obj_add_event_cb(
        s.play_button,
        play_event,
        LV_EVENT_CLICKED,
        nullptr
    );

    s.next_button =
        make_button(
            s.player_page,
            "›",
            44,
            44,
            COLOR_PANEL_2
        );

    lv_obj_align(
        s.next_button,
        LV_ALIGN_BOTTOM_RIGHT,
        -58,
        -5
    );

    lv_obj_add_event_cb(
        s.next_button,
        next_event,
        LV_EVENT_CLICKED,
        nullptr
    );

    if (s.playing) {
        start_record_animation();
    }
}

static void rebuild_page()
{
    stop_record_animation();

    if (s.list_page) {
        delete_page(
            s.list_page
        );
        s.list_page = nullptr;
    }

    if (s.player_page) {
        delete_page(
            s.player_page
        );
        s.player_page = nullptr;
    }

    if (s.page == Page::Player) {
        create_player_page();
    } else {
        create_list_page();
    }
}

static bool ensure_root_locked()
{
    if (s.root) {
        return true;
    }

    s.root =
        lv_obj_create(
            lv_layer_top()
        );

    lv_obj_remove_style_all(
        s.root
    );

    lv_obj_set_size(
        s.root,
        SCREEN_W,
        SCREEN_H
    );

    lv_obj_align(
        s.root,
        LV_ALIGN_CENTER,
        0,
        0
    );

    lv_obj_set_style_bg_opa(
        s.root,
        LV_OPA_TRANSP,
        0
    );

    /*
     * Physical screen is circular inside a 360x360 framebuffer.
     * All content stays inside the 300px safe diameter.
     */


    lv_obj_clear_flag(
        s.root,
        LV_OBJ_FLAG_SCROLLABLE
    );

    return true;
}

static void refresh_locked()
{
    if (!ensure_root_locked()) {
        return;
    }

    rebuild_page();
}

static void destroy_locked()
{
    stop_record_animation();

    if (s.root) {
        lv_obj_delete(
            s.root
        );
        s.root = nullptr;
    }

    s.list_page = nullptr;
    s.player_page = nullptr;
    s.list_container = nullptr;
    s.player_title = nullptr;
    s.player_status = nullptr;
    s.record = nullptr;
    s.record_hole = nullptr;
    s.record_center = nullptr;
    s.prev_button = nullptr;
    s.play_button = nullptr;
    s.next_button = nullptr;
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

extern "C" bool bilibili_story_open(void)
{
    s.active = true;

    if (s.page == Page::Closed) {
        s.page = Page::List;
    }

    LvglDisplay *display_ptr =
        display();

    if (!display_ptr) {
        ESP_LOGE(
            TAG,
            "[OPEN] LvglDisplay unavailable"
        );
        return false;
    }

    {
        DisplayLockGuard lock(
            display_ptr
        );

        refresh_locked();
    }

    return s.root != nullptr;
}

extern "C" void bilibili_story_close(void)
{
    bilibili_audio_stop();
    s.active = false;
    s.page = Page::Closed;
    s.playing = false;

    LvglDisplay *display_ptr =
        display();

    if (!display_ptr) {
        return;
    }

    DisplayLockGuard lock(
        display_ptr
    );

    destroy_locked();
}

extern "C" bool bilibili_story_is_active(void)
{
    return s.active;
}

extern "C" void bilibili_story_search(const char *up_name) {
    if (!up_name || up_name[0] == '\0') return;
    if (!s.active && !bilibili_story_open()) return;
    bilibili_audio_stop();
    bili_video_t results[BILI_RECORD_MAX] = {};
    const uint8_t count = vocat_bilibili_search_up(up_name, results, BILI_RECORD_MAX);
    bilibili_story_show_list(results, count);
}

extern "C" void bilibili_story_show_list(
    const bili_video_t *videos,
    uint8_t count
)
{
    if (!s.active) {
        return;
    }

    s.count =
        std::min<uint8_t>(
            count,
            BILI_RECORD_MAX
        );

    memset(
        s.videos,
        0,
        sizeof(s.videos)
    );

    if (
        videos &&
        s.count > 0
    ) {
        memcpy(
            s.videos,
            videos,
            sizeof(bili_video_t) *
                s.count
        );
    }

    if (
        s.selected >= s.count
    ) {
        s.selected = 0;
    }

    bilibili_audio_stop();
    s.page = Page::List;
    s.playing = false;

    LvglDisplay *display_ptr =
        display();

    if (!display_ptr) {
        return;
    }

    DisplayLockGuard lock(
        display_ptr
    );

    refresh_locked();
}

extern "C" void bilibili_story_show_player(
    uint8_t index
)
{
    if (
        !s.active ||
        index >= s.count
    ) {
        return;
    }

    bilibili_audio_stop();
    s.selected = index;
    s.page = Page::Player;
    s.playing = start_audio_for_selected();

    LvglDisplay *display_ptr =
        display();

    if (!display_ptr) {
        return;
    }

    DisplayLockGuard lock(
        display_ptr
    );

    refresh_locked();
}

extern "C" void bilibili_story_set_playing(
    bool playing
)
{
    if (
        !s.active ||
        s.page != Page::Player
    ) {
        return;
    }

    s.playing = playing;
    bilibili_audio_set_paused(!playing);

    LvglDisplay *display_ptr =
        display();

    if (!display_ptr) {
        return;
    }

    DisplayLockGuard lock(
        display_ptr
    );

    update_play_button();

    if (s.player_status) {
        lv_label_set_text(
            s.player_status,
            s.playing
                ? "正在播放"
                : "已暂停"
        );

        lv_obj_set_style_text_color(
            s.player_status,
            s.playing
                ? rgb(COLOR_SUCCESS)
                : rgb(COLOR_MUTED),
            0
        );
    }

    if (s.playing) {
        start_record_animation();
    } else {
        stop_record_animation();
    }
}

extern "C" void bilibili_story_set_track(uint8_t index) {
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

extern "C" void bilibili_story_back(void)
{
    if (!s.active) {
        return;
    }

    if (s.page == Page::Player) {
        bilibili_audio_stop();
        s.page = Page::List;
        s.playing = false;

        LvglDisplay *display_ptr =
            display();

        if (!display_ptr) {
            return;
        }

        DisplayLockGuard lock(
            display_ptr
        );

        refresh_locked();
        return;
    }

    bilibili_story_close();
}

extern "C" bool bilibili_story_handle_touch(
    int x,
    int y
)
{
    if (!s.active) {
        return false;
    }

    if (s.page == Page::Player) {
        if (x < 82 && y < 62) {
            bilibili_story_back();
            return true;
        }

        if (x > 278 && y < 62) {
            bilibili_story_close();
            return true;
        }

        if (y > 305) {
            if (x < 120) {
                bilibili_story_previous();
                return true;
            }

            if (x < 240) {
                bilibili_story_set_playing(!s.playing);
                return true;
            }

            bilibili_story_next();
            return true;
        }

        return true;
    }

    if (s.page == Page::List) {
        if (x > 278 && y < 62) {
            bilibili_story_close();
            return true;
        }

        /*
         * Native LVGL scrolling is used when the LVGL touch indev is active.
         * For the existing board-level release-only touch path, touching a
         * list row directly selects the corresponding item.
         */
        if (
            y >= 66 &&
            y < 310 &&
            s.count > 0
        ) {
            const int relative =
                y - 66;
            const int slot =
                relative / (ROW_HEIGHT + ROW_GAP);

            if (
                slot >= 0 &&
                slot < s.count
            ) {
                bilibili_story_show_player(
                    static_cast<uint8_t>(slot)
                );
                return true;
            }
        }

        return true;
    }

    return true;
}

extern "C" bool bilibili_story_handle_swipe(
    int start_x,
    int start_y,
    int end_x,
    int end_y
)
{
    if (
        !s.active ||
        s.page != Page::List ||
        !s.list_container
    ) {
        return false;
    }

    const int dy = end_y - start_y;

    if (std::abs(dy) < 20) {
        return false;
    }

    lv_obj_scroll_by(
        s.list_container,
        0,
        -dy,
        LV_ANIM_ON
    );

    return true;
}

extern "C" void vocat_bilibili_render_screen_async(void)
{
    (void)bilibili_story_open();
}

extern "C" bool vocat_bilibili_render_screen(void)
{
    return bilibili_story_open();
}

extern "C" void vocat_bilibili_ui_clear(void)
{
    bilibili_story_close();
}

extern "C" void vocat_bilibili_ui_draw(
    const bili_video_t *videos,
    uint8_t count
)
{
    if (!s.active) {
        (void)bilibili_story_open();
    }

    bilibili_story_show_list(
        videos,
        count
    );
}

extern "C" bool vocat_bilibili_ui_handle_touch(
    int x,
    int y
)
{
    return bilibili_story_handle_touch(
        x,
        y
    );
}

extern "C" bool vocat_bilibili_ui_is_active(void)
{
    return bilibili_story_is_active();
}
