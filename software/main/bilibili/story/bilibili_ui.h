#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocat_bilibili.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * B站 Story UI
 * Stage 1:
 *   - 一级视频列表
 *   - 二级播放器
 *   - 返回/关闭
 *   - 上一首/播放暂停/下一首
 *
 * 网络搜索、封面下载、音频流和自动连播在后续阶段接入。
 */
bool bilibili_story_open(void);
void bilibili_story_close(void);
bool bilibili_story_is_active(void);

void bilibili_story_show_list(const bili_video_t *videos, uint8_t count);
void bilibili_story_show_player(uint8_t index);
void bilibili_story_set_playing(bool playing);
void bilibili_story_set_track(uint8_t index);
void bilibili_story_previous(void);
void bilibili_story_next(void);
void bilibili_story_back(void);

bool bilibili_story_handle_touch(int x, int y);

/*
 * 兼容当前 VoCat 触摸/MCP 入口。
 * custom 目录文件保持不改，由本 Story UI 提供新的实现。
 */
void vocat_bilibili_render_screen_async(void);
bool vocat_bilibili_render_screen(void);
void vocat_bilibili_ui_clear(void);
void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count);
bool vocat_bilibili_ui_handle_touch(int x, int y);
bool vocat_bilibili_ui_is_active(void);

#ifdef __cplusplus
}
#endif
