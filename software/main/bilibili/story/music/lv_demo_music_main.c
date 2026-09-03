/**
 * VoCat local LVGL Music Player UI.
 *
 * The original LVGL Music Demo was designed as a showcase and contains a
 * relatively expensive spectrum renderer plus several long-running intro and
 * transition animations.  This copy keeps the same visual language and the
 * player/list interaction, but uses a lightweight 360x360 first page and a
 * second page for the track list.
 */
#include "lv_demo_music_main.h"
#include <stdatomic.h>
#if LV_USE_DEMO_MUSIC

#include "lv_demo_music_list.h"

#define ACTIVE_TRACK_CNT       3
#define PLAYER_PAGE_HEIGHT     360
#define ALBUM_BOX_W            220
#define ALBUM_BOX_H            205
#define ALBUM_SCALE_MIN        (LV_SCALE_NONE * 97 / 100)
#define ALBUM_SCALE_MAX        (LV_SCALE_NONE * 100 / 100)
#define TRACK_CHANGE_ANIM_MS   280
#define PULSE_TIMER_MS         66

static lv_obj_t * player_page;
static lv_obj_t * title_label;
static lv_obj_t * artist_label;
static lv_obj_t * genre_label;
static lv_obj_t * time_obj;
static lv_obj_t * album_image_obj;
static lv_obj_t * slider_obj;
static lv_obj_t * play_obj;
static lv_timer_t * sec_counter_timer;
static const lv_font_t * font_small;
static const lv_font_t * font_large;
static uint32_t track_id;
static uint32_t time_act;
static atomic_bool playing;
static atomic_bool touch_active;
static lv_anim_t album_anim;
static lv_timer_t * pulse_timer;
static uint8_t pulse_phase;

static lv_obj_t * create_player_page(lv_obj_t * parent);
static lv_obj_t * create_title_box(lv_obj_t * parent);
static lv_obj_t * create_album_box(lv_obj_t * parent);
static lv_obj_t * create_ctrl_box(lv_obj_t * parent);
static lv_obj_t * create_handle(lv_obj_t * parent);
static void album_gesture_event_cb(lv_event_t * e);
static void play_event_click_cb(lv_event_t * e);
static void prev_click_event_cb(lv_event_t * e);
static void next_click_event_cb(lv_event_t * e);
static void timer_cb(lv_timer_t * t);
static void del_counter_timer_cb(lv_event_t * e);
static void pulse_timer_cb(lv_timer_t * t);
static void track_anim_opa_cb(void * obj, int32_t opa);
static void track_anim_scale_cb(void * obj, int32_t scale);
static void track_load(uint32_t id);
static void start_album_pulse(void);
static void stop_album_pulse(void);

static void set_album_source(lv_obj_t * img, uint32_t id)
{
    LV_IMAGE_DECLARE(vocat_music_cover_1);
    LV_IMAGE_DECLARE(vocat_music_cover_2);
    LV_IMAGE_DECLARE(vocat_music_cover_3);

    switch(id) {
        case 2:
            lv_image_set_src(img, &vocat_music_cover_3);
            break;
        case 1:
            lv_image_set_src(img, &vocat_music_cover_2);
            break;
        default:
            lv_image_set_src(img, &vocat_music_cover_1);
            break;
    }
}

lv_obj_t * vocat_lv_demo_music_main_create(lv_obj_t * parent)
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

    player_page = create_player_page(parent);
    return player_page;
}

void vocat_lv_demo_music_album_next(bool next)
{
    uint32_t id = track_id;
    if(next) {
        id = (id + 1U) % ACTIVE_TRACK_CNT;
    }
    else {
        id = (id == 0U) ? (ACTIVE_TRACK_CNT - 1U) : (id - 1U);
    }

    if(atomic_load_explicit(&playing, memory_order_acquire)) {
        vocat_lv_demo_music_play(id);
    }
    else {
        track_load(id);
    }
}

void vocat_lv_demo_music_play(uint32_t id)
{
    if(id >= ACTIVE_TRACK_CNT) {
        id = 0;
    }
    track_load(id);
    vocat_lv_demo_music_resume();
}

void vocat_lv_demo_music_resume(void)
{
    atomic_store_explicit(&playing, true, memory_order_release);
    lv_obj_add_state(play_obj, LV_STATE_CHECKED);

    if(sec_counter_timer) {
        lv_timer_resume(sec_counter_timer);
    }

    start_album_pulse();
    vocat_lv_demo_music_list_button_check(track_id, true);
}

bool vocat_lv_demo_music_is_playing(void)
{
    return atomic_load_explicit(&playing, memory_order_acquire);
}

void vocat_lv_demo_music_set_touch_active(bool active)
{
    atomic_store_explicit(&touch_active, active, memory_order_release);
    if(active) {
        if(pulse_timer != NULL) {
            lv_timer_pause(pulse_timer);
        }
        if(album_image_obj != NULL) {
            lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
        }
    }
    else if(atomic_load_explicit(&playing, memory_order_acquire)) {
        start_album_pulse();
    }
}

void vocat_lv_demo_music_pause(void)
{
    atomic_store_explicit(&playing, false, memory_order_release);

    if(sec_counter_timer) {
        lv_timer_pause(sec_counter_timer);
    }

    stop_album_pulse();
    lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    lv_obj_remove_state(play_obj, LV_STATE_CHECKED);
    vocat_lv_demo_music_list_button_check(track_id, false);
}

static lv_obj_t * create_player_page(lv_obj_t * parent)
{
    player_page = lv_obj_create(parent);
    lv_obj_remove_style_all(player_page);
    lv_obj_set_size(player_page, lv_pct(100), PLAYER_PAGE_HEIGHT);
    lv_obj_set_pos(player_page, 0, 0);
    lv_obj_set_style_bg_opa(player_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(player_page, lv_color_hex(0xffffff), 0);
    lv_obj_clear_flag(player_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(player_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(player_page, del_counter_timer_cb, LV_EVENT_DELETE, NULL);

    /* Keep the original wave artwork, but do not animate it. */
    LV_IMAGE_DECLARE(vocat_music_wave_top);
    LV_IMAGE_DECLARE(vocat_music_wave_bottom);

    lv_obj_t * wave_top = lv_image_create(player_page);
    lv_image_set_src(wave_top, &vocat_music_wave_top);
    lv_image_set_inner_align(wave_top, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave_top, lv_pct(100));
    lv_obj_align(wave_top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(wave_top, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t * wave_bottom = lv_image_create(player_page);
    lv_image_set_src(wave_bottom, &vocat_music_wave_bottom);
    lv_image_set_inner_align(wave_bottom, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave_bottom, lv_pct(100));
    lv_obj_align(wave_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(wave_bottom, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t * album_box = create_album_box(player_page);
    lv_obj_t * title_box = create_title_box(player_page);
    lv_obj_t * ctrl_box = create_ctrl_box(player_page);
    lv_obj_t * handle_box = create_handle(player_page);

    lv_obj_align(album_box, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_align(title_box, LV_ALIGN_TOP_MID, 0, 207);
    lv_obj_align(ctrl_box, LV_ALIGN_TOP_MID, 0, 251);
    lv_obj_align(handle_box, LV_ALIGN_TOP_MID, 0, 327);

    sec_counter_timer = lv_timer_create(timer_cb, 1000, NULL);
    lv_timer_pause(sec_counter_timer);

    return player_page;
}

static lv_obj_t * create_album_box(lv_obj_t * parent)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ALBUM_BOX_W, ALBUM_BOX_H);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    album_image_obj = lv_image_create(box);
    set_album_source(album_image_obj, track_id);
    lv_image_set_antialias(album_image_obj, false);
    lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    lv_obj_center(album_image_obj);
    lv_obj_add_flag(album_image_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(album_image_obj, album_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(album_image_obj, LV_OBJ_FLAG_GESTURE_BUBBLE);

    return box;
}

static lv_obj_t * create_title_box(lv_obj_t * parent)
{
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 300, 44);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title_label = lv_label_create(cont);
    lv_obj_set_style_text_font(title_label, font_large, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x504d6d), 0);
    lv_label_set_text(title_label, vocat_lv_demo_music_get_title(track_id));

    artist_label = lv_label_create(cont);
    lv_obj_set_style_text_font(artist_label, font_small, 0);
    lv_obj_set_style_text_color(artist_label, lv_color_hex(0x504d6d), 0);
    lv_label_set_text(artist_label, vocat_lv_demo_music_get_artist(track_id));

    genre_label = lv_label_create(cont);
    lv_obj_set_style_text_font(genre_label, font_small, 0);
    lv_obj_set_style_text_color(genre_label, lv_color_hex(0x8a86b8), 0);
    lv_label_set_text(genre_label, vocat_lv_demo_music_get_genre(track_id));

    return cont;
}

static lv_obj_t * create_ctrl_box(lv_obj_t * parent)
{
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 270, 70);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    LV_IMAGE_DECLARE(vocat_music_btn_next);
    LV_IMAGE_DECLARE(vocat_music_btn_prev);
    LV_IMAGE_DECLARE(vocat_music_btn_play);
    LV_IMAGE_DECLARE(vocat_music_btn_pause);
    LV_IMAGE_DECLARE(vocat_music_slider_knob);

    lv_obj_t * icon = lv_image_create(cont);
    lv_image_set_src(icon, &vocat_music_btn_prev);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 28, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, prev_click_event_cb, LV_EVENT_CLICKED, NULL);

    play_obj = lv_imagebutton_create(cont);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL,
                           &vocat_music_btn_play, NULL);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, NULL,
                           &vocat_music_btn_pause, NULL);
    lv_obj_add_flag(play_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CHECKABLE);
    lv_obj_center(play_obj);
    lv_obj_add_event_cb(play_obj, play_event_click_cb, LV_EVENT_CLICKED, NULL);

    icon = lv_image_create(cont);
    lv_image_set_src(icon, &vocat_music_btn_next);
    lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, -28, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, next_click_event_cb, LV_EVENT_CLICKED, NULL);

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
    lv_obj_add_event_cb(slider_obj, del_counter_timer_cb, LV_EVENT_DELETE, NULL);

    time_obj = lv_label_create(cont);
    lv_obj_set_style_text_font(time_obj, font_small, 0);
    lv_obj_set_style_text_color(time_obj, lv_color_hex(0x8a86b8), 0);
    lv_label_set_text(time_obj, "0:00");
    lv_obj_align(time_obj, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_add_event_cb(time_obj, del_counter_timer_cb, LV_EVENT_DELETE, NULL);

    return cont;
}

static lv_obj_t * create_handle(lv_obj_t * parent)
{
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 150, 28);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 3, 0);

    lv_obj_t * label = lv_label_create(cont);
    lv_label_set_text(label, "ALL TRACKS");
    lv_obj_set_style_text_font(label, font_small, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8a86b8), 0);

    lv_obj_t * rect = lv_obj_create(cont);
    lv_obj_set_size(rect, 24, 2);
    lv_obj_set_style_bg_color(rect, lv_color_hex(0x8a86b8), 0);
    lv_obj_set_style_border_width(rect, 0, 0);

    return cont;
}

static void track_load(uint32_t id)
{
    if(id >= ACTIVE_TRACK_CNT) id = 0;

    time_act = 0;
    lv_slider_set_value(slider_obj, 0, LV_ANIM_OFF);
    lv_label_set_text(time_obj, "0:00");

    if(id == track_id) return;

    vocat_lv_demo_music_list_button_check(track_id, false);
    track_id = id;
    vocat_lv_demo_music_list_button_check(track_id, atomic_load_explicit(&playing, memory_order_acquire));

    lv_label_set_text(title_label, vocat_lv_demo_music_get_title(track_id));
    lv_label_set_text(artist_label, vocat_lv_demo_music_get_artist(track_id));
    lv_label_set_text(genre_label, vocat_lv_demo_music_get_genre(track_id));

    lv_anim_delete(album_image_obj, NULL);
    set_album_source(album_image_obj, track_id);
    lv_obj_set_style_image_opa(album_image_obj, LV_OPA_TRANSP, 0);
    lv_image_set_scale(album_image_obj, ALBUM_SCALE_MIN);

    lv_anim_init(&album_anim);
    lv_anim_set_var(&album_anim, album_image_obj);
    lv_anim_set_values(&album_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&album_anim, TRACK_CHANGE_ANIM_MS);
    lv_anim_set_exec_cb(&album_anim, track_anim_opa_cb);
    lv_anim_start(&album_anim);

    lv_anim_init(&album_anim);
    lv_anim_set_var(&album_anim, album_image_obj);
    lv_anim_set_values(&album_anim, ALBUM_SCALE_MIN, LV_SCALE_NONE);
    lv_anim_set_duration(&album_anim, TRACK_CHANGE_ANIM_MS);
    lv_anim_set_exec_cb(&album_anim, track_anim_scale_cb);
    lv_anim_start(&album_anim);
}

static void start_album_pulse(void)
{
    if(album_image_obj == NULL) return;

    if(pulse_timer == NULL) {
        pulse_phase = 0;
        pulse_timer = lv_timer_create(pulse_timer_cb, PULSE_TIMER_MS, NULL);
    }
    else {
        lv_timer_resume(pulse_timer);
    }
}

static void stop_album_pulse(void)
{
    if(pulse_timer != NULL) {
        lv_timer_pause(pulse_timer);
    }
    if(album_image_obj != NULL) {
        lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    }
}

static void album_gesture_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT) {
        vocat_lv_demo_music_album_next(true);
    }
    else if(dir == LV_DIR_RIGHT) {
        vocat_lv_demo_music_album_next(false);
    }
}

static void play_event_click_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    if(lv_obj_has_state(obj, LV_STATE_CHECKED)) {
        vocat_lv_demo_music_resume();
    }
    else {
        vocat_lv_demo_music_pause();
    }
}

static void prev_click_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    vocat_lv_demo_music_album_next(false);
}

static void next_click_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    vocat_lv_demo_music_album_next(true);
}

static void timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(!atomic_load_explicit(&playing, memory_order_acquire)) return;

    ++time_act;
    lv_label_set_text_fmt(time_obj, "%"LV_PRIu32":%02"LV_PRIu32,
                          time_act / 60U, time_act % 60U);
    lv_slider_set_value(slider_obj, (int32_t)time_act, LV_ANIM_OFF);
}

static void del_counter_timer_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_DELETE) return;

    if(sec_counter_timer != NULL) {
        lv_timer_delete(sec_counter_timer);
        sec_counter_timer = NULL;
    }
    if(pulse_timer != NULL) {
        lv_timer_delete(pulse_timer);
        pulse_timer = NULL;
    }
}

static void pulse_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(album_image_obj == NULL ||
       !atomic_load_explicit(&playing, memory_order_acquire) ||
       atomic_load_explicit(&touch_active, memory_order_acquire)) {
        return;
    }

    /* 30 Hz, very small pulse: enough to keep the artwork alive without
     * forcing LVGL to rescale the image on every timer-handler iteration. */
    pulse_phase = (uint8_t)((pulse_phase + 1U) & 0x1FU);
    uint8_t p = pulse_phase;
    if(p > 16U) p = (uint8_t)(32U - p);

    const uint16_t span = ALBUM_SCALE_MAX - ALBUM_SCALE_MIN;
    const uint16_t scale = (uint16_t)(ALBUM_SCALE_MIN +
                                      ((uint32_t)span * p) / 16U);
    lv_image_set_scale(album_image_obj, scale);
}

static void track_anim_opa_cb(void * obj, int32_t opa)
{
    lv_obj_set_style_image_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static void track_anim_scale_cb(void * obj, int32_t scale)
{
    lv_image_set_scale((lv_obj_t *)obj, (uint16_t)scale);
}

#endif /* LV_USE_DEMO_MUSIC */
