#ifndef LV_DEMO_MUSIC_MAIN_H
#define LV_DEMO_MUSIC_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_demo_music.h"

#if LV_USE_DEMO_MUSIC
lv_obj_t *vocat_lv_demo_music_main_create(lv_obj_t *parent);
void vocat_lv_demo_music_refresh_tracks(void);
void vocat_lv_demo_music_play(uint32_t id);
void vocat_lv_demo_music_resume(void);
void vocat_lv_demo_music_pause(void);
void vocat_lv_demo_music_album_next(bool next);
uint32_t vocat_lv_demo_music_get_current_id(void);
bool vocat_lv_demo_music_is_playing(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
