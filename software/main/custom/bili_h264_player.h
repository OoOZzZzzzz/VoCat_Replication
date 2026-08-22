#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *rgb565;
    uint16_t width;
    uint16_t height;
    uint64_t pts_us;
} bili_player_frame_t;

typedef void (*bili_player_frame_cb_t)(const bili_player_frame_t *frame, void *user_data);
typedef void (*bili_player_status_cb_t)(const char *status, void *user_data);

bool bili_player_start(const char *bvid,
                       bili_player_frame_cb_t frame_cb,
                       bili_player_status_cb_t status_cb,
                       void *user_data);

void bili_player_stop(void);
bool bili_player_is_running(void);

#ifdef __cplusplus
}
#endif
