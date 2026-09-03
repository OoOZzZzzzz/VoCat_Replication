#ifndef LV_DEMO_MUSIC_LIST_H
#define LV_DEMO_MUSIC_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_demo_music.h"

#if LV_USE_DEMO_MUSIC
lv_obj_t *vocat_lv_demo_music_list_create(lv_obj_t *parent);
void vocat_lv_demo_music_list_rebuild(void);
void vocat_lv_demo_music_list_button_check(uint32_t track_id, bool state);
#endif

#ifdef __cplusplus
}
#endif

#endif
