#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bilibili_audio_eof_cb_t)(void *user_data);

bool bilibili_audio_start(
    const char *bvid,
    bilibili_audio_eof_cb_t eof_cb,
    void *user_data
);

void bilibili_audio_stop(void);
void bilibili_audio_set_paused(bool paused);
bool bilibili_audio_is_running(void);
bool bilibili_audio_is_paused(void);

#ifdef __cplusplus
}
#endif
