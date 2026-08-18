#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocat_bilibili.h"

#ifdef __cplusplus
extern "C" {
#endif

void vocat_bilibili_render_screen_async(void);
void vocat_bilibili_ui_clear(void);
bool vocat_bilibili_ui_handle_touch(int x, int y);
void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count);
bool vocat_bilibili_render_screen(void);
bool vocat_bilibili_ui_is_active(void);

#ifdef __cplusplus
}
#endif
