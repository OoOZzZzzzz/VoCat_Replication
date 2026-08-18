#include "vocat_bilibili.h"
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <stdio.h>
#include <esp_heap_caps.h>

#define TAG "VOCAT_BILIBILI"
// ============改成你的电脑Flask代理局域网地址============
#define BILIBILI_API "http://192.168.31.106:8000/bili"
#define HTTP_BUF_SIZE 4096

const char* vocat_bilibili_url(void)
{
    return BILIBILI_API;
}

bool vocat_bilibili_check_wifi(void)
{
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) return false;
    return ip_info.ip.addr != 0;
}

// HTTP接收上下文，解决static静态变量残留bug
typedef struct {
    char *buf;
    size_t buf_max;
    size_t write_len;
} http_recv_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_recv_ctx_t *ctx = (http_recv_ctx_t*)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;

    switch(evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
            if ((ctx->write_len + evt->data_len) < (ctx->buf_max - 1))
            {
                memcpy(ctx->buf + ctx->write_len, evt->data, evt->data_len);
                ctx->write_len += evt->data_len;
                ctx->buf[ctx->write_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

uint8_t vocat_bilibili_get_recommend(bili_video_t* out_videos, uint8_t max_cnt)
{
    memset(out_videos, 0, sizeof(bili_video_t) * max_cnt);

    char* recv_buf = (char*)heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_8BIT);
    if(recv_buf == NULL)
    {
        ESP_LOGE(TAG, "malloc recv buf failed");
        return 0;
    }
    memset(recv_buf, 0, HTTP_BUF_SIZE);

    http_recv_ctx_t ctx = {
        .buf = recv_buf,
        .buf_max = HTTP_BUF_SIZE,
        .write_len = 0
    };

    esp_http_client_config_t cfg = {
        .url = BILIBILI_API,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 8000,
        .buffer_size_tx = 1024,
        // HTTP不需要crt_bundle_attach，删除证书配置
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        heap_caps_free(recv_buf);
        return 0;
    }

    // 访问本地代理，不再需要B站UA、Referer头，全部注释
    // esp_http_client_set_header(client, "User?Agent", "xxx");
    // esp_http_client_set_header(client, "Referer", "xxx");

    esp_err_t ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if(ret != ESP_OK || status_code != 200)
    {
        ESP_LOGE(TAG, "API request fail, ret:%d code:%d", ret, status_code);
        heap_caps_free(recv_buf);
        return 0;
    }

    // 打印代理返回原始JSON，调试用
    ESP_LOGI(TAG, "==== Proxy raw response ====");
    ESP_LOGI(TAG, "%s", recv_buf);
    ESP_LOGI(TAG, "=============================");

    cJSON *root = cJSON_Parse(recv_buf);
    heap_caps_free(recv_buf);

    if(!root)
    {
        ESP_LOGE(TAG, "JSON parse error");
        return 0;
    }

    // ?重点：代理直接输出list数组，没有外层data对象
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    if(!cJSON_IsArray(list))
    {
        ESP_LOGE(TAG, "list is not array");
        cJSON_Delete(root);
        return 0;
    }

    uint8_t valid_cnt = 0;
    int arr_size = cJSON_GetArraySize(list);
    for(int i = 0; i < arr_size && valid_cnt < max_cnt; i++)
    {
        cJSON *item = cJSON_GetArrayItem(list, i);
        if(!item) continue;
        bili_video_t *vid = &out_videos[valid_cnt];

        const char* title = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
        strncpy(vid->title, title ? title : "无标题", BILI_TITLE_MAX_LEN);
        vid->title[BILI_TITLE_MAX_LEN] = '\0';

        const char* cover = cJSON_GetStringValue(cJSON_GetObjectItem(item, "pic"));
        strncpy(vid->cover_url, cover ? cover : "", BILI_COVER_URL_MAX_LEN);
        vid->cover_url[BILI_COVER_URL_MAX_LEN] = '\0';

        const char* bvid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "bvid"));
        strncpy(vid->bvid, bvid ? bvid : "", BILI_BVID_MAX_LEN);
        vid->bvid[BILI_BVID_MAX_LEN] = '\0';

        vid->play_count = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "play"));
        valid_cnt++;
    }

    cJSON_Delete(root);
    return valid_cnt;
}