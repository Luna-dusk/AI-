#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "wifi.h"
#include "ai_client.h"
#include "i2s.h"
#include "wavecode.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

static const char *TAG = "MAIN";

#define PCM_BUF_SIZE      (320 * 1024)     //PSRAM: 最多缓存 ~10s 音频 
#define RESP_BUF_INIT     4096           // 响应初始容量 
#define RESP_BUF_MAX      (2 * 1024 * 1024) // 响应上限 2MB 
#define TTS_BUF_SIZE      (512 * 1024)   // TTS PCM 上限 

// 在 JSON 字符串中找 "key":"value" 并拷贝 value 到 out（处理转义）
static int json_extract_string(const char *json, const char *key,char *out, int out_size)
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
    while (*p && *p != '"' && i < out_size - 1) 
    {
        if (*p == '\\' && *(p + 1)) 
        {
            p++;
            switch (*p) 
            {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case '\\': out[i++] = '\\'; break;
                case '"':  out[i++] = '"';  break;
                default:   out[i++] = *p;   break;
            }
        } 
        else 
        {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_find_b64(const char *json, const char *key,const char **out_ptr, size_t *out_len)
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

static void audio_process_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    audio_frame_t frame;

    // PCM 累加缓冲（PSRAM）
    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) 
    {
        ESP_LOGE(TAG, "PCM缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }
    size_t pcm_pos = 0;

    while (1) {
        if (xQueueReceive(q, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 采集阶段：累积 PCM 帧
        if (frame.type == AUDIO_FRAME_DATA) 
        {
            if (pcm_pos + frame.bytes <= PCM_BUF_SIZE) 
            {
                memcpy(pcm_buf + pcm_pos, frame.data, frame.bytes);
                pcm_pos += frame.bytes;
            } 
            else 
            {
                ESP_LOGW(TAG, "PCM缓冲满，丢帧");
            }
            continue;
        }

        // 一句话结束
        if (frame.type != AUDIO_FRAME_END) 
        {
            continue;
        }

        if (pcm_pos == 0) 
        {
            ESP_LOGW(TAG, "空录音，跳过");
            continue;
        }

        ESP_LOGI(TAG, "录音完成: %u 字节", (unsigned)pcm_pos);

        // 1. 构建 WAV
        uint8_t *wav_buf = (uint8_t *)heap_caps_malloc(
            pcm_pos + WAV_HEADER_SIZE, MALLOC_CAP_SPIRAM);
        if (!wav_buf) 
        {
            ESP_LOGE(TAG, "WAV缓冲分配失败");
            pcm_pos = 0;
            continue;
        }

        size_t wav_size = 0;
        if (wav_build(wav_buf, (const int16_t *)pcm_buf, pcm_pos,16000, 1, &wav_size) != 0) 
        {
            ESP_LOGE(TAG, "WAV构建失败");
            free(wav_buf);
            pcm_pos = 0;
            continue;
        }
        ESP_LOGI(TAG, "WAV: %u 字节", (unsigned)wav_size);

        // 上传到服务器
        char *resp_buf = (char *)malloc(RESP_BUF_INIT);
        if (!resp_buf) 
        {
            ESP_LOGE(TAG, "响应缓冲分配失败");
            free(wav_buf);
            pcm_pos = 0;
            continue;
        }

        int resp_len = ai_client_upload_asr(wav_buf, (int)wav_size,&resp_buf, RESP_BUF_INIT);
        free(wav_buf);
        wav_buf = NULL;

        if (resp_len <= 0) 
        {
            ESP_LOGE(TAG, "ASR上传失败");
            free(resp_buf);
            pcm_pos = 0;
            continue;
        }
        ESP_LOGI(TAG, "收到响应(%d字节)", resp_len);

        //提取 TTS base64（在 resp_buf 内零拷贝定位）
        const char *tts_b64 = NULL;
        size_t tts_b64_len = 0;
        json_find_b64(resp_buf, "tts", &tts_b64, &tts_b64_len);

        uint8_t *tts_pcm = NULL;
        size_t tts_pcm_size = 0;

        if (tts_b64 && tts_b64_len > 0) 
        {
            tts_pcm = (uint8_t *)heap_caps_malloc(TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (tts_pcm) 
            {
                size_t decoded = 0;
                int ret = mbedtls_base64_decode(tts_pcm, TTS_BUF_SIZE,
                                                &decoded,
                                                (const uint8_t *)tts_b64,
                                                tts_b64_len);
                if (ret == 0) 
                {
                    tts_pcm_size = decoded;
                    ESP_LOGI(TAG, "TTS解码: %u 字节", (unsigned)tts_pcm_size);
                } 
                else 
                {
                    ESP_LOGE(TAG, "base64解码失败: %d", ret);
                    free(tts_pcm);
                    tts_pcm = NULL;
                }
            }
        }

        //提取回复文本
        char reply[512] = {0};
        json_extract_string(resp_buf, "reply", reply, sizeof(reply));
        if (reply[0]) {
            ESP_LOGI(TAG, "AI回复: %s", reply);
        }

        free(resp_buf);
        resp_buf = NULL;

        //播放 TTS
        if (tts_pcm && tts_pcm_size > WAV_HEADER_SIZE) 
        {
            int16_t *play_data = (int16_t *)tts_pcm;
            size_t play_bytes  = tts_pcm_size;

            // 检测 RIFF 头，跳过 WAV 文件头
            if (memcmp(tts_pcm, "RIFF", 4) == 0) 
            {
                play_data  = (int16_t *)((uint8_t *)tts_pcm + WAV_HEADER_SIZE);
                play_bytes = tts_pcm_size - WAV_HEADER_SIZE;
            }

            ESP_LOGI(TAG, "播放TTS: %u 字节", (unsigned)play_bytes);
            size_t written = 0;
            send_audio(play_data, play_bytes, &written, pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "播放完成: %u 字节", (unsigned)written);
        } 
        else 
        {
            ESP_LOGW(TAG, "无TTS音频可播放");
        }

        free(tts_pcm);
        tts_pcm = NULL;

        //排空队列中处理期间积压的帧
        pcm_pos = 0;
        while (xQueueReceive(q, &frame, 0) == pdTRUE) 
        {
            /* 丢弃旧帧，避免下次录音混入 */
        }

        ESP_LOGI(TAG, "等待下一次录音...");
    }
}

void app_main(void)
{
    if (esp_create_wifi_task() != ESP_OK) 
    {
        ESP_LOGE(TAG, "wifi task create failed");
        return;
    }

    EventGroupHandle_t wifi_evt = NULL;
    while ((wifi_evt = wifi_get_event_group()) == NULL) 
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "等待WiFi连接...");
    xEventGroupWaitBits(wifi_evt,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi已就绪");

    init_i2s();
    if (i2s_audio_start() != ESP_OK) 
    {
        ESP_LOGE(TAG, "i2s audio start failed");
        return;
    }

    xTaskCreate(audio_process_task, "audio_proc", 8192,i2s_get_pcm_queue(), 4, NULL);

    // 启动测试
    char *response = malloc(2048);
    if (!response) 
    {
        ESP_LOGE(TAG, "内存不足");
        return;
    }
    ai_client_send_request("{\"text\":\"你好\"}", response, 2048);
    free(response);
}
