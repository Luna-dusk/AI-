#include "i2s.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "I2S";

static QueueHandle_t s_pcm_queue = NULL;
static TaskHandle_t s_mic_task = NULL;
static bool s_mic_warmed = false;

void init_i2s(void)
{
    i2s_config_t i2s_spk_config = {
        .mode = I2S_MODE_TX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t spk_pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_SPK_BCK_PIN,
        .ws_io_num = I2S_SPK_WS_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
        .data_out_num = I2S_SPK_DATAOUT_PIN
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_spk_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &spk_pin_config));

    i2s_config_t i2s_mic_config = {
        .mode = I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t mic_pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_MIC_BCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_in_num = I2S_MIC_DATAIN_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE
    };

    /* 事件队列传 0/NULL：mic 数据走 i2s_read 直接读 DMA，不需要事件通知 */
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_1, &i2s_mic_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_1, &mic_pin_config));
}

void boot_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_BOOT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);
}

esp_err_t receive_audio(int16_t *buf, size_t len, size_t *bytes_read, TickType_t ticks_to_wait)
{
    if (buf == NULL || len == 0) 
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t real_byte = 0;
    esp_err_t ret = i2s_read(I2S_NUM_1, buf, len, &real_byte, ticks_to_wait);
    if (bytes_read)
    {
        *bytes_read = real_byte;
    }
    return ret;
}

esp_err_t send_audio(const int16_t *buf, size_t len, size_t *bytes_written, TickType_t ticks_to_wait)
{
    if (buf == NULL || len == 0) 
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int16_t *out = buf;
    int16_t *vol_buf = NULL;

    if (SPK_VOLUME < 1.0f) 
    {
        size_t samples = len / sizeof(int16_t);
        vol_buf = (int16_t *)malloc(len);
        if (vol_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < samples; i++) 
        {
            vol_buf[i] = (int16_t)(buf[i] * SPK_VOLUME);
        }
        out = vol_buf;
    }

    size_t total = 0;
    esp_err_t ret = ESP_OK;

    /* 分块写，避免一次阻塞太久，给 WiFi 让出 CPU */
    while (total < len) 
    {
        size_t chunk = len - total;
        if (chunk > AUDIO_FRAME_BYTES) 
        {
            chunk = AUDIO_FRAME_BYTES;
        }
        size_t w = 0;
        ret = i2s_write(I2S_NUM_0, (const uint8_t *)out + total, chunk, &w, ticks_to_wait);
        total += w;
        if (ret != ESP_OK) 
        {
            break;
        }
        if (w == 0) 
        {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }

    if (bytes_written) 
    {
        *bytes_written = total;
    }
    free(vol_buf);
    return ret;
}

QueueHandle_t i2s_get_pcm_queue(void)
{
    return s_pcm_queue;
}

static bool pcm_queue_send(const audio_frame_t *frame)
{
    if (s_pcm_queue == NULL) {
        return false;
    }
    /* 超时 0：队列满则丢帧，避免 mic 堵死拖垮 WiFi */
    if (xQueueSend(s_pcm_queue, frame, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "pcm queue full, drop frame type=%d", (int)frame->type);
        return false;
    }
    return true;
}

/* 按键采集任务：按下读麦入 PCM 队列，松开发 END */
static void mic_task(void *arg)
{
    (void)arg;
    audio_frame_t frame;
    size_t bytes_read = 0;
    bool press = false;

    boot_init();

    while (1) {
        bool pressed = (gpio_get_level(BTN_BOOT_PIN) == 0);

        if (!pressed) 
        {
            if (press) 
            {
                memset(&frame, 0, sizeof(frame));
                frame.type = AUDIO_FRAME_END;
                frame.bytes = 0;
                pcm_queue_send(&frame);
                ESP_LOGI(TAG, "record end");
            }
            press = false;
            s_mic_warmed = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!press) 
        {
            ESP_LOGI(TAG, "record start");
            press = true;
            s_mic_warmed = false;
        }

        if (!s_mic_warmed) 
        {
            int16_t dump[AUDIO_FRAME_SAMPLES];
            for (int i = 0; i < 4; i++) 
            {
                receive_audio(dump, AUDIO_FRAME_BYTES, &bytes_read, pdMS_TO_TICKS(100));
            }
            s_mic_warmed = true;
        }

        memset(&frame, 0, sizeof(frame));
        frame.type = AUDIO_FRAME_DATA;
        if (receive_audio(frame.data, AUDIO_FRAME_BYTES, &bytes_read, pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }
        if (bytes_read == 0) {
            continue;
        }
        frame.bytes = bytes_read;
        pcm_queue_send(&frame);
    }
}

esp_err_t i2s_audio_start(void)
{
    if (s_pcm_queue != NULL)
    {
        return ESP_OK;
    }

    s_pcm_queue = xQueueCreate(PCM_QUEUE_LEN, sizeof(audio_frame_t));
    if (s_pcm_queue == NULL)
    {
        ESP_LOGE(TAG, "create pcm queue failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(mic_task, "mic", 4096, NULL, 5, &s_mic_task) != pdPASS)
    {
        ESP_LOGE(TAG, "create mic task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio started: pcm_queue=%d frames", PCM_QUEUE_LEN);
    return ESP_OK;
}
