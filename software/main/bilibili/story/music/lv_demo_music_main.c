#include "lv_demo_music_main.h"

#if LV_USE_DEMO_MUSIC

#include <stdatomic.h>

#include "lv_demo_music_list.h"

#define PLAYER_PAGE_HEIGHT 360
#define ALBUM_BOX_W 220
#define ALBUM_BOX_H 205
#define ALBUM_SCALE_MIN (LV_SCALE_NONE * 97 / 100)
#define TRACK_CHANGE_ANIM_MS 220
#define PULSE_TIMER_MS 66

static lv_obj_t *player_page;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *genre_label;
static lv_obj_t *time_obj;
static lv_obj_t *album_image_obj;
static lv_obj_t *slider_obj;
static lv_obj_t *play_obj;
static lv_timer_t *sec_counter_timer;
static lv_timer_t *pulse_timer;
static const lv_font_t *font_small;
static const lv_font_t *font_large;
static uint32_t track_id;
static uint32_t time_act;
static atomic_bool playing;
static atomic_bool touch_active;
static uint8_t pulse_phase;

static void set_album_source(lv_obj_t *img, uint32_t id)
{
    LV_IMAGE_DECLARE(vocat_music_cover_1);
    LV_IMAGE_DECLARE(vocat_music_cover_2);
    LV_IMAGE_DECLARE(vocat_music_cover_3);

    switch (id % 3U) {
        case 1: lv_image_set_src(img, &vocat_music_cover_2); break;
        case 2: lv_image_set_src(img, &vocat_music_cover_3); break;
        default: lv_image_set_src(img, &vocat_music_cover_1); break;
    }
}

static void refresh_text(void)
{
    lv_label_set_text(title_label, vocat_lv_demo_music_get_title(track_id));
    lv_label_set_text(artist_label, vocat_lv_demo_music_get_artist(track_id));
    lv_label_set_text(genre_label, vocat_lv_demo_music_get_genre(track_id));
}

static void track_load(uint32_t id)
{
    const uint8_t count = vocat_lv_demo_music_get_track_count();
    if (count == 0) return;
    if (id >= count) id = 0;

    vocat_lv_demo_music_list_button_check(track_id, false);
    track_id = id;
    time_act = 0;
    lv_slider_set_value(slider_obj, 0, LV_ANIM_OFF);
    lv_label_set_text(time_obj, "0:00");
    refresh_text();

    lv_anim_delete(album_image_obj, NULL);
    set_album_source(album_image_obj, track_id);
    lv_obj_set_style_image_opa(album_image_obj, LV_OPA_COVER, 0);
    lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    vocat_lv_demo_music_list_button_check(track_id, atomic_load_explicit(&playing, memory_order_acquire));
}

static void pulse_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (album_image_obj == NULL ||
        !atomic_load_explicit(&playing, memory_order_acquire) ||
        atomic_load_explicit(&touch_active, memory_order_acquire)) {
        return;
    }
    pulse_phase = (uint8_t)((pulse_phase + 1U) & 0x1FU);
    uint8_t p = pulse_phase;
    if (p > 16U) p = (uint8_t)(32U - p);
    const uint16_t span = LV_SCALE_NONE - ALBUM_SCALE_MIN;
    lv_image_set_scale(
        album_image_obj,
        (uint16_t)(ALBUM_SCALE_MIN + ((uint32_t)span * p) / 16U));
}

static void start_pulse(void)
{
    if (pulse_timer == NULL) {
        pulse_phase = 0;
        pulse_timer = lv_timer_create(pulse_timer_cb, PULSE_TIMER_MS, NULL);
    } else {
        lv_timer_resume(pulse_timer);
    }
}

static void stop_pulse(void)
{
    if (pulse_timer) lv_timer_pause(pulse_timer);
    if (album_image_obj) lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!atomic_load_explicit(&playing, memory_order_acquire)) return;
    ++time_act;
    lv_label_set_text_fmt(time_obj, "%"LV_PRIu32":%02"LV_PRIu32,
                          time_act / 60U, time_act % 60U);
    const uint32_t duration = vocat_lv_demo_music_get_track_length(track_id);
    if (duration > 0) {
        lv_slider_set_range(slider_obj, 0, (int32_t)duration);
        lv_slider_set_value(slider_obj, (int32_t)time_act, LV_ANIM_OFF);
    }
}

static void album_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) vocat_lv_demo_music_album_next(true);
    else if (dir == LV_DIR_RIGHT) vocat_lv_demo_music_album_next(false);
}

static void play_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    if (lv_obj_has_state(obj, LV_STATE_CHECKED)) vocat_lv_demo_music_resume();
    else vocat_lv_demo_music_pause();
}

static void prev_event_cb(lv_event_t *e)
{
    (void)e;
    vocat_lv_demo_music_album_next(false);
}

static void next_event_cb(lv_event_t *e)
{
    (void)e;
    vocat_lv_demo_music_album_next(true);
}

static lv_obj_t *create_ctrl(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 270, 70);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    LV_IMAGE_DECLARE(vocat_music_btn_next);
    LV_IMAGE_DECLARE(vocat_music_btn_prev);
    LV_IMAGE_DECLARE(vocat_music_btn_play);
    LV_IMAGE_DECLARE(vocat_music_btn_pause);
    LV_IMAGE_DECLARE(vocat_music_slider_knob);

    lv_obj_t *icon = lv_image_create(cont);
    lv_image_set_src(icon, &vocat_music_btn_prev);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 28, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, prev_event_cb, LV_EVENT_CLICKED, NULL);

    play_obj = lv_imagebutton_create(cont);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &vocat_music_btn_play, NULL);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, NULL, &vocat_music_btn_pause, NULL);
    lv_obj_add_flag(play_obj, LV_OBJ_FLAG_CHECKABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(play_obj);
    lv_obj_add_event_cb(play_obj, play_event_cb, LV_EVENT_CLICKED, NULL);

    icon = lv_image_create(cont);
    lv_image_set_src(icon, &vocat_music_btn_next);
    lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, -28, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, next_event_cb, LV_EVENT_CLICKED, NULL);

    slider_obj = lv_slider_create(cont);
    lv_obj_set_width(slider_obj, 215);
    lv_obj_set_height(slider_obj, 3);
    lv_obj_align(slider_obj, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_anim_duration(slider_obj, 0, 0);
    lv_obj_remove_flag(slider_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_image_src(slider_obj, &vocat_music_slider_knob, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_obj, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_obj, 16, LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(slider_obj, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_obj, lv_color_hex(0x569af8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider_obj, lv_color_hex(0xa666f1), LV_PART_INDICATOR);

    time_obj = lv_label_create(cont);
    lv_obj_set_style_text_font(time_obj, font_small, 0);
    lv_obj_set_style_text_color(time_obj, lv_color_hex(0x8a86b8), 0);
    lv_label_set_text(time_obj, "0:00");
    lv_obj_align(time_obj, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    return cont;
}

static lv_obj_t *create_player(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), PLAYER_PAGE_HEIGHT);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xffffff), 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    LV_IMAGE_DECLARE(vocat_music_wave_top);
    LV_IMAGE_DECLARE(vocat_music_wave_bottom);

    lv_obj_t *wave = lv_image_create(page);
    lv_image_set_src(wave, &vocat_music_wave_top);
    lv_image_set_inner_align(wave, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave, lv_pct(100));
    lv_obj_align(wave, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(wave, LV_OBJ_FLAG_IGNORE_LAYOUT);

    wave = lv_image_create(page);
    lv_image_set_src(wave, &vocat_music_wave_bottom);
    lv_image_set_inner_align(wave, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave, lv_pct(100));
    lv_obj_align(wave, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(wave, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t *box = lv_obj_create(page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ALBUM_BOX_W, ALBUM_BOX_H);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    album_image_obj = lv_image_create(box);
    set_album_source(album_image_obj, 0);
    lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    lv_obj_center(album_image_obj);
    lv_obj_add_flag(album_image_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(album_image_obj, album_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title_box = lv_obj_create(page);
    lv_obj_remove_style_all(title_box);
    lv_obj_set_size(title_box, 300, 44);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title_label = lv_label_create(title_box);
    lv_obj_set_style_text_font(title_label, font_large, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x504d6d), 0);
    artist_label = lv_label_create(title_box);
    lv_obj_set_style_text_font(artist_label, font_small, 0);
    lv_obj_set_style_text_color(artist_label, lv_color_hex(0x504d6d), 0);
    genre_label = lv_label_create(title_box);
    lv_obj_set_style_text_font(genre_label, font_small, 0);
    lv_obj_set_style_text_color(genre_label, lv_color_hex(0x8a86b8), 0);

    lv_obj_t *ctrl = create_ctrl(page);
    lv_obj_t *handle = lv_obj_create(page);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 150, 28);
    lv_obj_set_flex_flow(handle, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(handle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(handle, 3, 0);
    lv_obj_t *label = lv_label_create(handle);
    lv_label_set_text(label, "ALL TRACKS");
    lv_obj_set_style_text_font(label, font_small, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8a86b8), 0);
    lv_obj_t *line = lv_obj_create(handle);
    lv_obj_set_size(line, 24, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x8a86b8), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_align(title_box, LV_ALIGN_TOP_MID, 0, 207);
    lv_obj_align(ctrl, LV_ALIGN_TOP_MID, 0, 251);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 327);

    refresh_text();
    sec_counter_timer = lv_timer_create(timer_cb, 1000, NULL);
    lv_timer_pause(sec_counter_timer);
    return page;
}

lv_obj_t *vocat_lv_demo_music_main_create(lv_obj_t *parent)
{
    font_small = LV_FONT_DEFAULT;
    font_large = LV_FONT_DEFAULT;
#if LV_DEMO_MUSIC_LARGE
# if LV_FONT_MONTSERRAT_22
    font_small = &lv_font_montserrat_22;
# endif
# if LV_FONT_MONTSERRAT_32
    font_large = &lv_font_montserrat_32;
# endif
#else
# if LV_FONT_MONTSERRAT_12
    font_small = &lv_font_montserrat_12;
# endif
# if LV_FONT_MONTSERRAT_16
    font_large = &lv_font_montserrat_16;
# endif
#endif
    track_id = 0;
    time_act = 0;
    atomic_init(&playing, false);
    atomic_init(&touch_active, false);
    player_page = create_player(parent);
    return player_page;
}

void vocat_lv_demo_music_refresh_tracks(void)
{
    const uint8_t count = vocat_lv_demo_music_get_track_count();
    if (count == 0) {
        track_id = 0;
        atomic_store_explicit(&playing, false, memory_order_release);
        if (play_obj) lv_obj_remove_state(play_obj, LV_STATE_CHECKED);
        if (sec_counter_timer) lv_timer_pause(sec_counter_timer);
        stop_pulse();
        vocat_lv_demo_music_list_rebuild();
        refresh_text();
        return;
    }
    if (track_id >= count) track_id = 0;
    track_load(track_id);
    vocat_lv_demo_music_list_rebuild();
}

void vocat_lv_demo_music_play(uint32_t id)
{
    if (vocat_lv_demo_music_get_track_count() == 0) return;
    track_load(id);
    vocat_lv_demo_music_resume();
}

void vocat_lv_demo_music_resume(void)
{
    if (vocat_lv_demo_music_get_track_count() == 0) return;
    (void)atomic_exchange_explicit(&playing, true, memory_order_acq_rel);
    lv_obj_add_state(play_obj, LV_STATE_CHECKED);
    if (sec_counter_timer) lv_timer_resume(sec_counter_timer);
    start_pulse();
    vocat_lv_demo_music_list_button_check(track_id, true);
    /* Resume is also used after a track change.  Always notify the owner so
     * a currently playing HTTP stream is replaced by the newly selected BVID. */
    vocat_lv_demo_music_emit_state(true, track_id);
}

void vocat_lv_demo_music_pause(void)
{
    const bool was_playing = atomic_exchange_explicit(&playing, false, memory_order_acq_rel);
    if (!was_playing) return;
    if (sec_counter_timer) lv_timer_pause(sec_counter_timer);
    stop_pulse();
    lv_obj_remove_state(play_obj, LV_STATE_CHECKED);
    vocat_lv_demo_music_list_button_check(track_id, false);
    vocat_lv_demo_music_emit_state(false, track_id);
}

void vocat_lv_demo_music_album_next(bool next)
{
    const uint8_t count = vocat_lv_demo_music_get_track_count();
    if (count == 0) return;
    const uint32_t id = next ? ((track_id + 1U) % count) : ((track_id == 0U) ? (count - 1U) : (track_id - 1U));
    const bool is_playing = atomic_load_explicit(&playing, memory_order_acquire);
    track_load(id);
    if (is_playing) vocat_lv_demo_music_resume();
}

uint32_t vocat_lv_demo_music_get_current_id(void)
{
    return track_id;
}

bool vocat_lv_demo_music_is_playing(void)
{
    return atomic_load_explicit(&playing, memory_order_acquire);
}

void vocat_lv_demo_music_set_touch_active(bool active)
{
    atomic_store_explicit(&touch_active, active, memory_order_release);
}

#endif
