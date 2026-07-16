#include "ai_client.h"

static const char *TAG = "AI_CLIENT";

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "已连接到服务器");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "请求头已发送");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "收到响应头: %s = %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "收到数据, 长度=%d", evt->data_len);
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "连接已断开");
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "发生错误");
            break;
        default:
            break;
    }
    return ESP_OK;
}
// 文本对话接口
int ai_client_send_request(const char *post_data, char *response_buf, int buf_size)
{
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "初始化HTTP客户端失败");
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    int post_len = strlen(post_data);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接服务器失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int wlen = esp_http_client_write(client, post_data, post_len);
    if (wlen < 0) {
        ESP_LOGE(TAG, "发送数据失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        ESP_LOGE(TAG, "获取响应头失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "服务器返回: %d", status);
        esp_http_client_cleanup(client);
        return -1;
    }

    int read_len = esp_http_client_read_response(client, response_buf, buf_size - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len > 0) {
        response_buf[read_len] = '\0';
        ESP_LOGI(TAG, "收到响应(%d字节): %s", read_len, response_buf);
        return read_len;
    }

    ESP_LOGE(TAG, "读取响应失败");
    return -1;
}

//ASR 音频上传
int ai_client_upload_asr(const uint8_t *wav_data, int wav_len,
                         char **resp_buf, int resp_cap)
{
    esp_http_client_config_t config = {
        .url = ASR_SERVER_URL,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) 
    {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    esp_err_t err = esp_http_client_open(client, wav_len);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "连接服务器失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 直接发送 WAV 二进制，不使用 perform() */
    int wlen = esp_http_client_write(client, (const char *)wav_data, wav_len);
    if (wlen < 0) {
        ESP_LOGE(TAG, "发送音频失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;  /* 可能是 -1（chunked），不影响后续读取 */

    int status = esp_http_client_get_status_code(client);
    if (status != 200) 
    {
        ESP_LOGW(TAG, "服务器返回: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 分块读取响应，自动扩容 */
    int total = 0;
    int cap = resp_cap;

    while (1) 
    {
        int n = esp_http_client_read_response(client,
                                              *resp_buf + total,
                                              cap - total - 1);
        if (n <= 0) break;
        total += n;

        /* 剩余空间不足 2KB 时扩容 */
        if (cap - total < 2048) 
        {
            int new_cap = cap * 2;
            char *tmp = (char *)realloc(*resp_buf, new_cap);
            if (!tmp) 
            {
                ESP_LOGW(TAG, "响应缓冲区扩容失败，截断");
                break;
            }
            *resp_buf = tmp;
            cap = new_cap;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total > 0) 
    {
        (*resp_buf)[total] = '\0';
        ESP_LOGI(TAG, "ASR响应: %d字节", total);
        return total;
    }

    ESP_LOGE(TAG, "读取ASR响应失败");
    return -1;
}
