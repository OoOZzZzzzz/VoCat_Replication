#pragma once

#ifndef BILIBILI_DEBUG
#define BILIBILI_DEBUG 1
#endif

#include "esp_log.h"

#if BILIBILI_DEBUG
#define BILI_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define BILI_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define BILI_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define BILI_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#else
#define BILI_LOGE(tag, fmt, ...) do { } while (0)
#define BILI_LOGW(tag, fmt, ...) do { } while (0)
#define BILI_LOGI(tag, fmt, ...) do { } while (0)
#define BILI_LOGD(tag, fmt, ...) do { } while (0)
#endif
