#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "vocat_bilibili.h"
#ifdef __cplusplus
extern "C" {
#endif
bool bilibili_story_open(void);
void bilibili_story_close(void);
bool bilibili_story_is_active(void);
void bilibili_story_search(const char *up_name);
void bilibili_story_show_list(const bili_video_t *videos, uint8_t count);
void bilibili_story_show_player(uint8_t index);
void bilibili_story_set_playing(bool playing);
void bilibili_story_set_track(uint8_t index);
void bilibili_story_previous(void);
void bilibili_story_next(void);
void bilibili_story_back(void);
bool bilibili_story_handle_touch(int x, int y);
bool bilibili_story_handle_swipe(int start_x, int start_y, int end_x, int end_y);
void vocat_bilibili_render_screen_async(void);
bool vocat_bilibili_render_screen(void);
void vocat_bilibili_ui_clear(void);
void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count);
bool vocat_bilibili_ui_handle_touch(int x, int y);
/* LVGL touch bridge: 1=PRESS, 2=HOLD, 0=RELEASE. */
bool vocat_bilibili_ui_handle_touch_event(int x, int y, int event);
bool vocat_bilibili_ui_is_active(void);
#ifdef __cplusplus
}
#endif
