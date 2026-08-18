#pragma once
#include "vocat_bilibili.h"
#ifdef __cplusplus
extern "C" {
#endif
void vocat_bilibili_render_screen_async(void);
void vocat_bilibili_ui_clear(void);
bool vocat_bilibili_ui_handle_touch(int x, int y);
#ifdef __cplusplus
}
#endif
