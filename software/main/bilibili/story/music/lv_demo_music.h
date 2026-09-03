#ifndef LV_DEMO_MUSIC_H
#define LV_DEMO_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "lv_demo_music_config.h"
#include "vocat_bilibili.h"

#if LV_USE_DEMO_MUSIC

typedef struct {
    lv_obj_t *parent;
} vocat_lv_demo_args_t;

typedef void (*vocat_music_state_cb_t)(bool playing, uint32_t track_id);

void vocat_lv_demo_music(void);
void vocat_lv_demo_music_with_args(const vocat_lv_demo_args_t *args);

void vocat_lv_demo_music_set_tracks(const bili_video_t *tracks, uint8_t count);
uint8_t vocat_lv_demo_music_get_track_count(void);
const bili_video_t *vocat_lv_demo_music_get_track(uint32_t track_id);

const char *vocat_lv_demo_music_get_title(uint32_t track_id);
const char *vocat_lv_demo_music_get_artist(uint32_t track_id);
const char *vocat_lv_demo_music_get_genre(uint32_t track_id);
uint32_t vocat_lv_demo_music_get_track_length(uint32_t track_id);

void vocat_lv_demo_music_set_state_callback(vocat_music_state_cb_t cb);
void vocat_lv_demo_music_emit_state(bool playing, uint32_t track_id);

bool vocat_lv_demo_music_is_playing(void);
void vocat_lv_demo_music_set_touch_active(bool active);

#endif

#ifdef __cplusplus
}
#endif

#endif
