#include "lv_demo_music.h"

#if LV_USE_DEMO_MUSIC

#include <string.h>

#include "lv_demo_music_list.h"
#include "lv_demo_music_main.h"

#define MUSIC_PAGE_HEIGHT 360
#define MUSIC_PAGE_COUNT 2
#define MUSIC_SCROLL_ANIM_MS 260

static bili_video_t tracks[BILI_RECORD_MAX];
static uint8_t track_count;
static vocat_music_state_cb_t state_cb;
static lv_obj_t *music_root;

static void music_root_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        music_root = NULL;
    }
}

static void page_gesture_cb(lv_event_t *e)
{
    lv_obj_t *root = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_active();
    if (!root || !indev) return;

    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    const int32_t current_y = lv_obj_get_scroll_y(root);
    lv_obj_t *list = lv_obj_get_child(root, 1);

    if (dir == LV_DIR_TOP) {
        if (current_y >= MUSIC_PAGE_HEIGHT) return;
    } else if (dir == LV_DIR_BOTTOM) {
        if (current_y <= 0) return;
        if (list && lv_obj_get_scroll_y(list) > 0) return;
    } else {
        return;
    }

    lv_obj_set_style_anim_duration(root, MUSIC_SCROLL_ANIM_MS, 0);
    lv_obj_scroll_to_y(root,
                       dir == LV_DIR_TOP ? MUSIC_PAGE_HEIGHT : 0,
                       LV_ANIM_ON);
}

void vocat_lv_demo_music(void)
{
    vocat_lv_demo_args_t args = { .parent = NULL };
    vocat_lv_demo_music_with_args(&args);
}

void vocat_lv_demo_music_with_args(const vocat_lv_demo_args_t *args)
{
    LV_ASSERT_NULL(args);
    lv_obj_t *parent = args->parent ? args->parent : lv_screen_active();

    if (music_root != NULL) return;

    music_root = lv_obj_create(parent);
    lv_obj_remove_style_all(music_root);
    lv_obj_set_size(music_root, lv_pct(100), MUSIC_PAGE_HEIGHT * MUSIC_PAGE_COUNT);
    lv_obj_set_pos(music_root, 0, 0);
    lv_obj_set_style_bg_opa(music_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(music_root, lv_color_hex(0x343247), 0);
    lv_obj_set_scroll_dir(music_root, LV_DIR_VER);
    lv_obj_remove_flag(music_root, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(music_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(music_root, music_root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(music_root, page_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(music_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    (void)vocat_lv_demo_music_main_create(music_root);
    (void)vocat_lv_demo_music_list_create(music_root);
    lv_obj_scroll_to_y(music_root, 0, LV_ANIM_OFF);
}

void vocat_lv_demo_music_set_tracks(const bili_video_t *input, uint8_t count)
{
    if (count > BILI_RECORD_MAX) count = BILI_RECORD_MAX;
    memset(tracks, 0, sizeof(tracks));
    if (input != NULL && count > 0) memcpy(tracks, input, sizeof(bili_video_t) * count);
    track_count = count;
}

uint8_t vocat_lv_demo_music_get_track_count(void) { return track_count; }

const bili_video_t *vocat_lv_demo_music_get_track(uint32_t track_id)
{
    if (track_id >= track_count) return NULL;
    return &tracks[track_id];
}

const char *vocat_lv_demo_music_get_title(uint32_t track_id)
{
    const bili_video_t *track = vocat_lv_demo_music_get_track(track_id);
    return (track && track->title[0] != '\0') ? track->title : "Bilibili";
}

const char *vocat_lv_demo_music_get_artist(uint32_t track_id)
{
    return vocat_lv_demo_music_get_track(track_id) ? "Bilibili" : "Waiting for search...";
}

const char *vocat_lv_demo_music_get_genre(uint32_t track_id)
{
    return vocat_lv_demo_music_get_track(track_id) ? "Bilibili" : "Waiting for search...";
}

uint32_t vocat_lv_demo_music_get_track_length(uint32_t track_id)
{
    (void)track_id;
    return 0;
}

void vocat_lv_demo_music_set_state_callback(vocat_music_state_cb_t cb) { state_cb = cb; }

void vocat_lv_demo_music_emit_state(bool playing, uint32_t track_id)
{
    if (state_cb) state_cb(playing, track_id);
}

#endif
