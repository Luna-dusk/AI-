#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "wifi.h"
#include "ai_client.h"
#include "i2s.h"
#include "afe_sr.h"
#include "lcd.h"
#include "lcd_ui.h"
#include "wavecode.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "multinet_cmd.h"

static const char *TAG = "MAIN";
static bool ws_online = false;

#define PCM_BUF_SIZE      (320 * 1024)
#define RESP_BUF_INIT     4096
#define RESP_BUF_MAX      (2 * 1024 * 1024)
#define TTS_BUF_SIZE      (512 * 1024)

static QueueHandle_t cmd_reply_queue = NULL;

static int json_find_b64(const char *json, const char *key,
                         const char **out_ptr, size_t *out_len);

/* 等待扬声器播放结束，并把表情复位为正常脸。
 * 判定依据：i2s_play_is_busy()（播放队列非空=仍在工作）+ wait_done。
 * 目的：情绪脸只在"回复播放期间"显示，播完立即回正常脸，
 * 而不是等下一句情绪到来才切换。 */
static void spk_wait_done_reset_face(TickType_t timeout)
{
    i2s_play_wait_done(timeout);
    while (i2s_play_is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lcd_ui_set_emotion(LCD_EMO_NORMAL);
}

static void cmd_reply_task(void *arg)
{
    (void)arg;
    int cmd_id;
    uint8_t *tts_pcm = (uint8_t *)heap_caps_malloc(TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!tts_pcm) {
        ESP_LOGE(TAG, "cmd_reply: TTS buffer alloc fail");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(cmd_reply_queue, &cmd_id, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "CMD reply: fetch TTS for cmd=%d", cmd_id);
        lcd_ui_set_status(LCD_STATUS_REPLYING);

        char *rsp = (char *)malloc(RESP_BUF_INIT);
        if (!rsp) {
            lcd_ui_set_emotion(LCD_EMO_NORMAL);
            afe_sr_set_capture_enabled(true);
            lcd_ui_set_status(LCD_STATUS_LISTENING);
            continue;
        }

        int rlen = ai_client_cmd_tts(cmd_id, &rsp, RESP_BUF_INIT);
        if (rlen > 0) {
            const char *tts_b64 = NULL; size_t tts_b64_len = 0;
            json_find_b64(rsp, "tts", &tts_b64, &tts_b64_len);

            if (tts_b64 && tts_b64_len > 0) {
                size_t tts_pcm_size = 0;
                int rc = mbedtls_base64_decode(tts_pcm, TTS_BUF_SIZE, &tts_pcm_size,
                                               (const uint8_t *)tts_b64, tts_b64_len);

                if (rc == 0 && tts_pcm_size > WAV_HEADER_SIZE) {
                    int16_t *pd = (int16_t *)tts_pcm;
                    size_t pb = tts_pcm_size;
                    if (memcmp(tts_pcm, "RIFF", 4) == 0) {
                        pd = (int16_t *)((uint8_t *)tts_pcm + WAV_HEADER_SIZE);
                        pb = tts_pcm_size - WAV_HEADER_SIZE;
                    }
                    int16_t *play_buf = heap_caps_malloc(pb, MALLOC_CAP_SPIRAM);
                    if (play_buf) {
                        memcpy(play_buf, pd, pb);
                        i2s_play_async(play_buf, pb);
                    }
                    spk_wait_done_reset_face(pdMS_TO_TICKS(30000));
                } else {
                    ESP_LOGW(TAG, "cmd_reply: bad tts payload len=%u rc=%d",
                             (unsigned)tts_b64_len, rc);
                }
            } else {
                ESP_LOGW(TAG, "cmd_reply: no tts in response");
            }
        } else {
            ESP_LOGW(TAG, "cmd_reply: fetch failed for cmd=%d", cmd_id);
        }

        free(rsp);

        /* 播放已完成或拉取失败：表情一律回正常脸（命令 5/6 不再停留情绪脸） */
        lcd_ui_set_emotion(LCD_EMO_NORMAL);
        afe_sr_set_capture_enabled(true);
        lcd_ui_set_status(LCD_STATUS_LISTENING);
    }
}

static void cmd_reply_cb(int cmd_id)
{
    if (cmd_reply_queue) {
        xQueueSend(cmd_reply_queue, &cmd_id, 0);
    }
}

static int json_extract_string(const char *json, const char *key, char *out, int out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *k = strstr(json, search);
    if (!k) return -1;
    const char *p = k + strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) { p++;
            switch (*p) {
                case 'n': out[i++]='\n'; break;
                case 't': out[i++]='\t'; break;
                case '\\':out[i++]='\\'; break;
                case '"': out[i++]='"'; break;
                default: out[i++]=*p; break;
            }
        } else out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_find_b64(const char *json, const char *key, const char **out_ptr, size_t *out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *k = strstr(json, search);
    if (!k) return -1;
    const char *p = k + strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    *out_ptr = p;
    const char *end = p;
    while (*end && *end != '"') end++;
    *out_len = end - p;
    return 0;
}

static void process_tts_response(const char *json, int json_len)
{
    QueueHandle_t q = i2s_get_pcm_queue();
    audio_frame_t dummy;
    (void)json_len;

    const char *tts_b64 = NULL; size_t tts_b64_len = 0;
    json_find_b64(json, "tts", &tts_b64, &tts_b64_len);

    uint8_t *tts_pcm = NULL; size_t tts_pcm_size = 0;
    if (tts_b64 && tts_b64_len > 0) {
        tts_pcm = (uint8_t *)heap_caps_malloc(TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (tts_pcm && mbedtls_base64_decode(tts_pcm, TTS_BUF_SIZE,
                 &tts_pcm_size, (const uint8_t *)tts_b64, tts_b64_len) != 0) {
            free(tts_pcm); tts_pcm = NULL;
        }
    }

    char reply[512] = {0};
    json_extract_string(json, "reply", reply, sizeof(reply));
    if (reply[0]) ESP_LOGI(TAG, "AI: %s", reply);

    { int s = 0; int a = 0;
        if (strstr(reply,"哈哈")||strstr(reply,"开心")||strstr(reply,"笑")||strstr(reply,"太好了")||strstr(reply,"不错")||strstr(reply,"棒")||strstr(reply,"可以")||strstr(reply,"嗯")||strstr(reply,"好")) s++;
        if (strstr(reply,"抱歉")||strstr(reply,"对不起")||strstr(reply,"遗憾")||strstr(reply,"不行")||strstr(reply,"没办法")||strstr(reply,"不能")||strstr(reply,"难过")||strstr(reply,"哭")) s--;
        if (strstr(reply,"烦")||strstr(reply,"坏")||strstr(reply,"生气")||strstr(reply,"吵")||strstr(reply,"愤怒")) a++;
        if (a>0) lcd_ui_set_emotion(LCD_EMO_ANGRY);
        else if (s>0) lcd_ui_set_emotion(LCD_EMO_SMILE);
        else if (s<0) lcd_ui_set_emotion(LCD_EMO_CRY);
        else lcd_ui_set_emotion(LCD_EMO_NORMAL);
    }

    if (tts_pcm && tts_pcm_size > WAV_HEADER_SIZE) {
        int16_t *pd = (int16_t *)tts_pcm; size_t pb = tts_pcm_size;
        if (memcmp(tts_pcm, "RIFF", 4) == 0) { pd = (int16_t *)((uint8_t*)tts_pcm+WAV_HEADER_SIZE); pb = tts_pcm_size - WAV_HEADER_SIZE; }
        int16_t *play_buf = heap_caps_malloc(pb, MALLOC_CAP_SPIRAM);
        if (play_buf) {
            memcpy(play_buf, pd, pb);
            i2s_play_async(play_buf, pb);
        }
        free(tts_pcm);
        spk_wait_done_reset_face(portMAX_DELAY);
    }

    while (xQueueReceive(q, &dummy, 0) == pdTRUE) {}
    afe_sr_set_capture_enabled(true);
    lcd_ui_set_status(LCD_STATUS_LISTENING);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    ai_client_heartbeat();
    while (1) { vTaskDelay(pdMS_TO_TICKS(60000)); ai_client_heartbeat(); }
}

/* 每 10 秒检查一次 WS 状态：连接断了就重连，恢复流式模式。
 * 注意：LCD 的 待机/聆听/回复 状态完全由 AFE 状态机（afe_sr.c）决定，
 * 本任务只负责 WS 重连，不在重连后强行把 LCD 置成"聆听中"，
 * 否则会和 AFE 的 enter_idle()（待机中）打架，导致"待机中"消失。 */
static void ws_reconnect_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!ws_client_is_connected()) {
            if (ws_online) {
                ws_online = false;
                ESP_LOGW(TAG, "WS connection lost, will reconnect");
            }
            if (ws_client_init() == 0) {
                ws_online = true;
                ESP_LOGI(TAG, "WS reconnected (streaming mode)");
            }
        } else if (!ws_online) {
            ws_online = true;
            ESP_LOGI(TAG, "WS stream mode restored");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void audio_process_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    audio_frame_t frame;

    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) { ESP_LOGE(TAG, "PCM malloc fail"); vTaskDelete(NULL); return; }
    size_t pcm_pos = 0;

    while (1) {
        if (xQueueReceive(q, &frame, portMAX_DELAY) != pdTRUE) continue;

        if (frame.type == AUDIO_FRAME_DATA) {
            if (pcm_pos + frame.bytes <= PCM_BUF_SIZE) {
                memcpy(pcm_buf + pcm_pos, frame.data, frame.bytes);
                pcm_pos += frame.bytes;
            }
            if (ws_online && ws_client_send_pcm((const uint8_t *)frame.data, (int)frame.bytes) != 0) {
                ESP_LOGW(TAG, "WS PCM send failed, switch to HTTP");
                ws_online = false;
            }
            continue;
        }
        if (frame.type != AUDIO_FRAME_END) continue;

        ESP_LOGI(TAG, "audio end: pcm=%u, ws=%d", (unsigned)pcm_pos, ws_online);

        if (pcm_pos == 0) {
            afe_sr_set_capture_enabled(true);
            lcd_ui_set_status(LCD_STATUS_LISTENING);
            continue;
        }

        /* WS streaming: progressive ASR->LLM->TTS */
        if (ws_online) {
            lcd_ui_set_status(LCD_STATUS_REPLYING);
            ESP_LOGI(TAG, "WS: send end, waiting reply");
            if (ws_client_send_end() != 0) {
                ESP_LOGW(TAG, "WS: send_end failed");
                ws_online = false;
                lcd_ui_set_status(LCD_STATUS_LISTENING);
            } else {
                ESP_LOGI(TAG, "WS: waiting done");
                if (ws_client_wait_done(pdMS_TO_TICKS(30000)) != 0) {
                    ESP_LOGW(TAG, "WS: wait_done failed");
                    ws_online = false;
                    lcd_ui_set_status(LCD_STATUS_LISTENING);
                } else if (!ws_client_has_tts()) {
                    ESP_LOGW(TAG, "WS: no tts, fallback HTTP");
                    ws_online = false;
                    lcd_ui_set_status(LCD_STATUS_LISTENING);
                } else {
                    ESP_LOGI(TAG, "WS: stream complete");
                    const char *reply = ws_client_get_reply();
                    if (reply && reply[0]) {
                        ESP_LOGI(TAG, "AI: %s", reply);
                        { int s = 0; int a = 0;
                            if (strstr(reply,"哈哈")||strstr(reply,"开心")||strstr(reply,"笑")||strstr(reply,"太好了")||strstr(reply,"不错")||strstr(reply,"棒")||strstr(reply,"可以")||strstr(reply,"嗯")||strstr(reply,"好")) s++;
                            if (strstr(reply,"抱歉")||strstr(reply,"对不起")||strstr(reply,"遗憾")||strstr(reply,"不行")||strstr(reply,"没办法")||strstr(reply,"不能")||strstr(reply,"难过")||strstr(reply,"哭")) s--;
                            if (strstr(reply,"烦")||strstr(reply,"坏")||strstr(reply,"生气")||strstr(reply,"吵")||strstr(reply,"愤怒")) a++;
                            if (a>0) lcd_ui_set_emotion(LCD_EMO_ANGRY);
                            else if (s>0) lcd_ui_set_emotion(LCD_EMO_SMILE);
                            else if (s<0) lcd_ui_set_emotion(LCD_EMO_CRY);
                            else lcd_ui_set_emotion(LCD_EMO_NORMAL);
                        }
                    }
                    spk_wait_done_reset_face(pdMS_TO_TICKS(30000));
                    { audio_frame_t dummy; while (xQueueReceive(q, &dummy, 0) == pdTRUE) {} }
                    afe_sr_set_capture_enabled(true);
                    lcd_ui_set_status(LCD_STATUS_LISTENING);
                    pcm_pos = 0;
                    continue;
                }
            }
        }

        ESP_LOGI(TAG, "录音完成: %u 字节", (unsigned)pcm_pos);

        uint8_t *wav = (uint8_t *)heap_caps_malloc(pcm_pos + WAV_HEADER_SIZE, MALLOC_CAP_SPIRAM);
        if (!wav) { pcm_pos=0; afe_sr_set_capture_enabled(true); continue; }
        size_t wav_sz = 0;
        if (wav_build(wav, (const int16_t *)pcm_buf, pcm_pos, 16000, 1, &wav_sz) != 0) {
            free(wav); pcm_pos=0; afe_sr_set_capture_enabled(true); continue;
        }

        char *rsp = (char *)malloc(RESP_BUF_INIT);
        if (!rsp) { free(wav); pcm_pos=0; afe_sr_set_capture_enabled(true); continue; }

        int rlen = ai_client_upload_asr(wav, (int)wav_sz, &rsp, RESP_BUF_INIT);
        free(wav);

        if (rlen <= 0) {
            ESP_LOGE(TAG, "ASR fail");
            free(rsp); pcm_pos=0;
            afe_sr_set_capture_enabled(true);
            lcd_ui_set_status(LCD_STATUS_LISTENING);
            continue;
        }
        process_tts_response(rsp, rlen);
        free(rsp);
        pcm_pos = 0;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== XiaoGe Starting, reset_reason=%d ===", esp_reset_reason());

    lcd_init();
    lcd_ui_set_status(LCD_STATUS_STANDBY);
    xTaskCreatePinnedToCore(lcd_ui_task, "lcd_ui", 4*1024, NULL, 6, NULL, 1);

    if (esp_create_wifi_task() != ESP_OK) { ESP_LOGE(TAG, "wifi fail"); return; }
    lcd_ui_set_status(LCD_STATUS_CONNECTING);

    EventGroupHandle_t evt = NULL;
    while ((evt = wifi_get_event_group()) == NULL) vTaskDelay(pdMS_TO_TICKS(10));
    xEventGroupWaitBits(evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi ready");

    xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 2, NULL);
    xTaskCreate(ws_reconnect_task, "ws_reconn", 3072, NULL, 2, NULL);

    /* try WS */
    if (ws_client_init() == 0) {
        ws_online = true;
        ESP_LOGI(TAG, "WS online (streaming mode)");
        /* 显示交给 AFE 状态机：afe_sr_start 后 fetch 任务会 enter_idle() 置 STANDBY */
    }

    init_i2s();
    if (i2s_audio_start() != ESP_OK) return;

    cmd_reply_queue = xQueueCreate(4, sizeof(int));
    if (cmd_reply_queue) {
        xTaskCreate(cmd_reply_task, "cmd_reply", 8192, NULL, 4, NULL);
        multinet_cmd_set_reply_cb(cmd_reply_cb);
    }

    if (afe_init() != ESP_OK) return;
    if (afe_sr_start(i2s_get_pcm_queue()) != ESP_OK) return;

    xTaskCreate(audio_process_task, "audio_proc", 8192, i2s_get_pcm_queue(), 4, NULL);
}
