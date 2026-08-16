#include "ai_client.h"
#include "esp_mac.h"
#include "mbedtls/base64.h"
#include "i2s.h"
#include "esp_timer.h"

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
        .timeout_ms = 15000,
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
static int ai_client_upload_asr_url(const char *url,
                                    const uint8_t *wav_data, int wav_len,
                                    char **resp_buf, int resp_cap)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .timeout_ms = 30000,
        .buffer_size = 8192,
        .buffer_size_tx = 8192,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 5,
        .keep_alive_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) 
    {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_key[18];
    snprintf(device_key, sizeof(device_key), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_http_client_set_header(client, "X-Device-Key", device_key);

    esp_err_t err = esp_http_client_open(client, wav_len);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "连接服务器失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    // 直接发送 WAV 二进制
    int wlen = esp_http_client_write(client, (const char *)wav_data, wav_len);
    if (wlen < 0) 
    {
        ESP_LOGE(TAG, "发送音频失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;  // 可能是 -1（chunked），不影响后续读取 

    int status = esp_http_client_get_status_code(client);
    if (status != 200) 
    {
        char error_body[256] = {0};
        int error_len = esp_http_client_read_response(client, error_body,
                                                       sizeof(error_body) - 1);
        if (error_len > 0) {
            error_body[error_len] = '\0';
            ESP_LOGW(TAG, "ASR HTTP %d body: %s", status, error_body);
        } else {
            ESP_LOGW(TAG, "ASR HTTP %d (empty response)", status);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return status == 404 ? -2 : -1;
    }

    /* 分块读取响应，自动扩容 */
    int total = 0;
    int cap = resp_cap;

    while (1) 
    {
        int n = esp_http_client_read_response(client,*resp_buf + total,cap - total - 1);
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

// 设备心跳接口
int ai_client_upload_asr(const uint8_t *wav_data, int wav_len,
                         char **resp_buf, int resp_cap)
{
    int result = ai_client_upload_asr_url(ASR_SERVER_URL, wav_data, wav_len,
                                          resp_buf, resp_cap);
    if (result == -2) {
        ESP_LOGW(TAG, "ASR /api/asr is 404, retrying /asr");
        result = ai_client_upload_asr_url(ASR_SERVER_URL_FALLBACK,
                                          wav_data, wav_len, resp_buf, resp_cap);
    }
    return result;
}

int ai_client_heartbeat(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"device_key\":\"%s\",\"name\":\"" DEVICE_NAME "\",\"firmware_version\":\"v1.0\"}",
             mac_str);

    esp_http_client_config_t config = {
        .url = HEARTBEAT_URL,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "心跳: HTTP初始化失败");
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    int post_len = strlen(payload);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "心跳: 连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_write(client, payload, post_len);
    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status == 200) {
        ESP_LOGI(TAG, "心跳成功: MAC=%s", mac_str);
        return 0;
    } else {
        ESP_LOGW(TAG, "心跳失败: HTTP %d", status);
        return -1;
    }
}

/* 本地命令 TTS 回复：POST {"cmd": id}，返回含 reply + base64 WAV 的 JSON */
int ai_client_cmd_tts(int cmd_id, char **resp_buf, int resp_cap)
{
    if (resp_buf == NULL || *resp_buf == NULL || resp_cap <= 0) {
        return -1;
    }

    esp_http_client_config_t config = {
        .url = CMD_TTS_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 30000,
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 5,
        .keep_alive_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "cmd_tts: HTTP初始化失败");
        return -1;
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"cmd\":%d}", cmd_id);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    int post_len = strlen(payload);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cmd_tts: 连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int wlen = esp_http_client_write(client, payload, post_len);
    if (wlen < 0) {
        ESP_LOGE(TAG, "cmd_tts: 发送失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "cmd_tts: HTTP %d", status);
        esp_http_client_cleanup(client);
        return -1;
    }

    int total = 0;
    int cap = resp_cap;
    while (1) {
        int n = esp_http_client_read_response(client, *resp_buf + total, cap - total - 1);
        if (n <= 0) break;
        total += n;
        if (cap - total < 2048) {
            int new_cap = cap * 2;
            char *tmp = (char *)realloc(*resp_buf, new_cap);
            if (!tmp) {
                ESP_LOGW(TAG, "cmd_tts: 缓冲区扩容失败，截断");
                break;
            }
            *resp_buf = tmp;
            cap = new_cap;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total > 0) {
        (*resp_buf)[total] = '\0';
        ESP_LOGI(TAG, "cmd_tts(%d): %d字节", cmd_id, total);
        return total;
    }

    ESP_LOGE(TAG, "cmd_tts: 读取响应失败");
    return -1;
}

// ── WebSocket 流式语音客户端 ──

#if 0 /* Replaced by raw_ws_client.c to avoid esp_websocket_client transport bugs. */
static esp_websocket_client_handle_t ws_handle = NULL;
static volatile bool ws_active = false;
static volatile bool ws_connected = false;
static SemaphoreHandle_t ws_connect_sem = NULL;
static SemaphoreHandle_t ws_rx_sem = NULL;
static SemaphoreHandle_t ws_done_sem = NULL;

static char ws_reply[512] = {0};
static volatile bool ws_reply_ready = false;
static volatile bool ws_stream_active = false;
static volatile bool ws_stream_failed = false;
static volatile bool ws_stream_has_tts = false;
static char ws_result_buf[8192];
static int  ws_result_len = 0;

#define WS_TEXT_MESSAGE_MAX  (12 * 1024)
static char ws_text_message[WS_TEXT_MESSAGE_MAX];
static char ws_b64[WS_TEXT_MESSAGE_MAX];

/* Returns 1 for a complete text message, 0 while collecting, and -1 on error. */
static int ws_collect_text_message(const esp_websocket_event_data_t *evt)
{
    if (evt->payload_len <= 0 || evt->payload_len >= WS_TEXT_MESSAGE_MAX ||
        evt->payload_offset < 0 || evt->data_len <= 0 ||
        evt->payload_offset + evt->data_len > evt->payload_len) {
        ESP_LOGW(TAG, "invalid WS text payload: offset=%d data=%d total=%d",
                 evt->payload_offset, evt->data_len, evt->payload_len);
        return -1;
    }

    memcpy(ws_text_message + evt->payload_offset, evt->data_ptr, evt->data_len);
    if (evt->payload_offset + evt->data_len != evt->payload_len) {
        return 0;
    }
    ws_text_message[evt->payload_len] = '\0';
    return 1;
}

static int ws_json_str(const char *json, const char *key, char *out, int out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
    return i;
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    if (!ws_active) return;
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WS connected");
            ws_connected = true;
            if (ws_connect_sem) xSemaphoreGive(ws_connect_sem);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WS disconnected");
            ws_connected = false;
            ws_stream_failed = true;
            ws_stream_active = false;
            if (ws_done_sem) xSemaphoreGive(ws_done_sem);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (evt->op_code == 0x01 && evt->data_len > 0) {
                int collect = ws_collect_text_message(evt);
                if (collect < 0) {
                    ws_stream_failed = true;
                    ws_stream_active = false;
                    if (ws_done_sem) xSemaphoreGive(ws_done_sem);
                    break;
                }
                if (collect == 0) break;
                const char *data = ws_text_message;

                if (!ws_stream_active) {
                    int cp = (ws_result_len + evt->payload_len < (int)sizeof(ws_result_buf) - 1)
                           ? evt->payload_len
                           : (int)sizeof(ws_result_buf) - 1 - ws_result_len;
                    memcpy(ws_result_buf + ws_result_len, data, cp);
                    ws_result_len += cp;
                    ws_result_buf[ws_result_len] = '\0';
                    if (ws_rx_sem) xSemaphoreGive(ws_rx_sem);
                    break;
                }

                if (strstr(data, "\"type\":\"reply\"")) {
                    char text[512];
                    if (ws_json_str(data, "text", text, sizeof(text)) > 0) {
                        strncpy(ws_reply, text, sizeof(ws_reply) - 1);
                        ws_reply[sizeof(ws_reply) - 1] = '\0';
                        ws_reply_ready = true;
                        ESP_LOGI(TAG, "WS reply: %s", ws_reply);
                    }
                } else if (strstr(data, "\"type\":\"tts\"")) {
                    int b64_len = ws_json_str(data, "data", ws_b64, sizeof(ws_b64));
                    if (b64_len > 0) {
                        size_t out_size = (b64_len * 3) / 4 + 16;
                        uint8_t *pcm = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
                        if (pcm) {
                            size_t decoded = 0;
                            if (mbedtls_base64_decode(pcm, out_size, &decoded,
                                    (const uint8_t *)ws_b64, (size_t)b64_len) == 0 && decoded > 0) {
                            if (i2s_play_async((int16_t *)pcm, decoded)) {
                                ws_stream_has_tts = true;
                            }
                            } else {
                                free(pcm);
                            }
                        }
                    }
                } else if (strstr(data, "\"type\":\"done\"")) {
                    ws_stream_active = false;
                    if (ws_done_sem) xSemaphoreGive(ws_done_sem);
                }
            } else if (evt->op_code == 0x02 && evt->data_len > 0 && ws_stream_active) {
                int16_t *pcm = heap_caps_malloc(evt->data_len, MALLOC_CAP_SPIRAM);
                if (pcm) {
                    memcpy(pcm, evt->data_ptr, evt->data_len);
                    if (i2s_play_async(pcm, evt->data_len)) {
                        ws_stream_has_tts = true;
                    }
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WS transport error");
            ws_stream_failed = true;
            ws_stream_active = false;
            if (ws_done_sem) xSemaphoreGive(ws_done_sem);
            break;

        default:
            break;
    }
}

int ws_client_init(void)
{
    if (ws_handle) return 0;

    if (!ws_connect_sem) ws_connect_sem = xSemaphoreCreateBinary();
    if (!ws_rx_sem)      ws_rx_sem      = xSemaphoreCreateBinary();
    if (!ws_done_sem)    ws_done_sem    = xSemaphoreCreateBinary();

    esp_websocket_client_config_t config = {
        .uri = WS_SERVER_URL,
        .reconnect_timeout_ms = 60000,
        .network_timeout_ms   = 5000,
        .ping_interval_sec    = 10,
        .pingpong_timeout_sec = 5,
    };
    ws_handle = esp_websocket_client_init(&config);
    if (!ws_handle) return -1;

    ws_active = true;

    esp_websocket_register_events(ws_handle, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(ws_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS start failed");
        esp_websocket_client_destroy(ws_handle);
        ws_handle = NULL;
    return -1;
}

    if (xSemaphoreTake(ws_connect_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGI(TAG, "WS not ready (server offline), will use HTTP");
        ws_client_deinit();
        return -1;
    }

    /* 等服务器 ready 消息 */
    static const char hello[] = "{\"type\":\"hello\",\"device\":\"esp32\"}";
    if (esp_websocket_client_send_text(ws_handle, hello, sizeof(hello) - 1,
                                       pdMS_TO_TICKS(1000)) != (int)(sizeof(hello) - 1)) {
        ESP_LOGW(TAG, "WS hello send failed");
        ws_client_deinit();
        return -1;
    }

    ws_result_len = 0;
    ws_stream_active = false;
    if (xSemaphoreTake(ws_rx_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGI(TAG, "WS ready timeout");
        ws_client_deinit();
        return -1;
    }

    ESP_LOGI(TAG, "WS ready");
    return 0;
}

void ws_client_deinit(void)
{
    ws_active = false;
    ws_connected = false;
    if (ws_handle) {
        esp_websocket_client_stop(ws_handle);
        esp_websocket_client_destroy(ws_handle);
        ws_handle = NULL;
    }
}

int ws_client_send_pcm(const uint8_t *pcm_data, int pcm_len)
{
    if (!ws_handle || !ws_connected) return -1;
    int sent = esp_websocket_client_send_bin(ws_handle, (const char *)pcm_data,
                                              pcm_len, 0);
    return sent == pcm_len ? 0 : -1;
}

bool ws_client_is_connected(void)
{
    return ws_handle != NULL && ws_active && ws_connected;
}

bool ws_client_has_tts(void)
{
    return ws_stream_has_tts;
}

int ws_client_send_end(void)
{
    if (!ws_client_is_connected()) return -1;

    ws_reply[0] = '\0';
    ws_reply_ready = false;
    ws_stream_failed = false;
    ws_stream_has_tts = false;
    ws_stream_active = true;

    while (xSemaphoreTake(ws_done_sem, 0) == pdTRUE) {}

    int sent = esp_websocket_client_send_text(ws_handle, "{\"type\":\"end\"}",
                                               14, pdMS_TO_TICKS(1000));
    if (sent != 14) {
        ws_stream_failed = true;
        ws_stream_active = false;
        return -1;
    }
    return 0;
}

int ws_client_wait_done(TickType_t timeout)
{
    if (!ws_handle) return -1;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout * portTICK_PERIOD_MS * 1000;

    while (ws_stream_active) {
        if (xSemaphoreTake(ws_done_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            return ws_stream_failed ? -1 : 0;
        }
        if (timeout != portMAX_DELAY) {
            if ((esp_timer_get_time() - start) >= timeout_us) {
                ESP_LOGW(TAG, "WS done timeout");
                ws_stream_active = false;
                return -1;
            }
        }
    }
    return ws_stream_failed ? -1 : 0;
}

const char *ws_client_get_reply(void)
{
    return ws_reply_ready ? ws_reply : NULL;
}

int ws_client_end_utterance(const char **result, int *result_len)
{
    if (!ws_handle) return -1;

    if (esp_websocket_client_send_text(ws_handle, "{\"type\":\"end\"}",
                                       14, pdMS_TO_TICKS(1000)) != 14) {
        return -1;
    }

    ws_result_len = 0;
    if (xSemaphoreTake(ws_rx_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGW(TAG, "WS reply timeout");
        return -1;
    }

    *result = ws_result_buf;
    *result_len = ws_result_len;
    return 0;
}
#endif
