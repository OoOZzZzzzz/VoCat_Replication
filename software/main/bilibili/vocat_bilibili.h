#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BILI_TITLE_MAX_LEN       120
#define BILI_COVER_URL_MAX_LEN   250
#define BILI_BVID_MAX_LEN        30
#define BILI_RECORD_MAX          8

#define BILI_SERVER_DEFAULT      "http://192.168.31.106:8000"

typedef struct {
    char title[BILI_TITLE_MAX_LEN + 1];
    char cover_url[BILI_COVER_URL_MAX_LEN + 1];
    uint32_t play_count;
    char bvid[BILI_BVID_MAX_LEN + 1];
} bili_video_t;

bool vocat_bilibili_check_wifi(void);
const char *vocat_bilibili_url(void);

uint8_t vocat_bilibili_get_recommend(
    bili_video_t *out_videos,
    uint8_t max_cnt
);

uint8_t vocat_bilibili_search_up(
    const char *up_name,
    bili_video_t *out_videos,
    uint8_t max_cnt
);

bool vocat_bilibili_build_audio_url(
    const char *bvid,
    char *out_url,
    size_t out_size
);

#ifdef __cplusplus
}
#endif
