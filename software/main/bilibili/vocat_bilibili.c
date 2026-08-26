#include "vocat_bilibili.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_netif.h>

#include <stdio.h>
#include <string.h>

#include "bilibili_debug.h"

#define TAG "BILI_API"

#ifndef BILIBILI_SERVER_URL
#define BILIBILI_SERVER_URL BILI_SERVER_DEFAULT
#endif

#define HTTP_TIMEOUT_MS 10000
#define JSON_BUFFER_SIZE (16 * 1024)

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} http_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_buffer_t *buffer = (http_buffer_t *)event->user_data;
    if (!buffer || !buffer->data) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        const size_t remaining = buffer->capacity - buffer->length - 1;
        const size_t copy_size = event->data_len < remaining ? event->data_len : remaining;

        if (copy_size > 0) {
            memcpy(buffer->data + buffer->length, event->data, copy_size);
            buffer->length += copy_size;
            buffer->data[buffer->length] = '\0';
        }

        if (copy_size != (size_t)event->data_len) {
            BILI_LOGW(TAG, "JSON response truncated");
        }
    }

    return ESP_OK;
}

static bool url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    if (!src || !dst || dst_size == 0) {
        return false;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)src[i];
        const bool safe =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            if (out + 1 >= dst_size) return false;
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_size) return false;
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[c & 0x0F];
        }
    }

    dst[out] = '\0';
    return true;
}

static bool http_get_json(const char *url, char **json_out)
{
    if (!url || !json_out) {
        return false;
    }

    *json_out = NULL;

    char *data = (char *)heap_caps_malloc(JSON_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!data) {
        BILI_LOGE(TAG, "JSON buffer allocation failed");
        return false;
    }

    memset(data, 0, JSON_BUFFER_SIZE);

    http_buffer_t buffer = {
        .data = data,
        .capacity = JSON_BUFFER_SIZE,
        .length = 0,
    };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(data);
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "VoCat-Bilibili/2.0");

    const esp_err_t ret = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);

    BILI_LOGI(TAG, "GET %s ret=%s status=%d bytes=%u",
              url, esp_err_to_name(ret), status, (unsigned)buffer.length);

    esp_http_client_cleanup(client);

    if (ret != ESP_OK || status < 200 || status >= 300) {
        heap_caps_free(data);
        return false;
    }

    *json_out = data;
    return true;
}

static uint8_t parse_video_list(const char *json, bili_video_t *out, uint8_t max_count)
{
    if (!json || !out || max_count == 0) {
        return 0;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        BILI_LOGE(TAG, "JSON parse failed");
        return 0;
    }

    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        BILI_LOGE(TAG, "response does not contain list[]");
        return 0;
    }

    uint8_t count = 0;
    const int total = cJSON_GetArraySize(list);

    for (int i = 0; i < total && count < max_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsObject(item)) continue;

        const char *bvid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "bvid"));
        if (!bvid || bvid[0] == '\0') continue;

        bili_video_t *video = &out[count];
        memset(video, 0, sizeof(*video));

        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
        const char *cover = cJSON_GetStringValue(cJSON_GetObjectItem(item, "pic"));
        if (!cover) {
            cover = cJSON_GetStringValue(cJSON_GetObjectItem(item, "cover"));
        }

        const double play = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "play"));
        const double view = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "view"));

        strncpy(video->title, title ? title : "无标题", BILI_TITLE_MAX_LEN);
        strncpy(video->cover_url, cover ? cover : "", BILI_COVER_URL_MAX_LEN);
        strncpy(video->bvid, bvid, BILI_BVID_MAX_LEN);
        video->play_count = play > 0 ? (uint32_t)play : (uint32_t)view;

        video->title[BILI_TITLE_MAX_LEN] = '\0';
        video->cover_url[BILI_COVER_URL_MAX_LEN] = '\0';
        video->bvid[BILI_BVID_MAX_LEN] = '\0';
        ++count;
    }

    cJSON_Delete(root);
    return count;
}

const char *vocat_bilibili_url(void)
{
    return BILIBILI_SERVER_URL "/bili";
}

bool vocat_bilibili_check_wifi(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return false;

    return info.ip.addr != 0;
}

uint8_t vocat_bilibili_get_recommend(bili_video_t *out_videos, uint8_t max_cnt)
{
    char *json = NULL;
    if (!http_get_json(BILIBILI_SERVER_URL "/bili", &json)) return 0;

    const uint8_t count = parse_video_list(json, out_videos, max_cnt);
    heap_caps_free(json);
    return count;
}

uint8_t vocat_bilibili_search_up(const char *up_name, bili_video_t *out_videos, uint8_t max_cnt)
{
    if (!up_name || up_name[0] == '\0') return 0;

    char encoded[256];
    if (!url_encode(up_name, encoded, sizeof(encoded))) return 0;

    char url[512];
    const int written = snprintf(url, sizeof(url), BILIBILI_SERVER_URL "/bili?up_name=%s", encoded);
    if (written <= 0 || (size_t)written >= sizeof(url)) return 0;

    BILI_LOGI(TAG, "search up=%s", up_name);

    char *json = NULL;
    if (!http_get_json(url, &json)) return 0;

#if BILIBILI_DEBUG
    BILI_LOGD(TAG, "response=%s", json);
#endif

    const uint8_t count = parse_video_list(json, out_videos, max_cnt);
    heap_caps_free(json);

    BILI_LOGI(TAG, "search result count=%u", (unsigned)count);
    return count;
}

bool vocat_bilibili_build_audio_url(const char *bvid, char *out_url, size_t out_size)
{
    if (!bvid || !bvid[0] || !out_url || out_size == 0) return false;

    char encoded[64];
    if (!url_encode(bvid, encoded, sizeof(encoded))) return false;

    const int written = snprintf(
        out_url,
        out_size,
        BILIBILI_SERVER_URL "/bili/audio_stream?bvid=%s",
        encoded
    );

    return written > 0 && (size_t)written < out_size;
}
