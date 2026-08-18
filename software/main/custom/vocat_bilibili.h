#ifndef VOCAT_BILIBILI_H
#define VOCAT_BILIBILI_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BILI_TITLE_MAX_LEN 120
#define BILI_COVER_URL_MAX_LEN 250
#define BILI_BVID_MAX_LEN 30
#define BILI_RECORD_MAX 4

typedef struct {
    char title[BILI_TITLE_MAX_LEN + 1];
    char cover_url[BILI_COVER_URL_MAX_LEN + 1];
    uint32_t play_count;
    char bvid[BILI_BVID_MAX_LEN + 1];
} bili_video_t;

uint8_t vocat_bilibili_get_recommend(bili_video_t* out_videos, uint8_t max_cnt);
bool vocat_bilibili_check_wifi(void);
const char* vocat_bilibili_url(void);

#ifdef __cplusplus
}
#endif
#endif