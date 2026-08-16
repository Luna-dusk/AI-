#include "i2s.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

static const char *TAG = "I2S";

static QueueHandle_t pcm_queue = NULL;
static StaticQueue_t pcm_queue_static;
static uint8_t *pcm_queue_storage = NULL;
static TaskHandle_t mic_task_handle = NULL;
static bool mic_warmed = false;

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

static float spk_volume = 0.5f;

void i2s_spk_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    spk_volume = vol;
    ESP_LOGI(TAG, "spk volume = %.2f", (double)spk_volume);
}

float i2s_spk_get_volume(void)
{
    return spk_volume;
}

esp_err_t send_audio(const int16_t *buf, size_t len, size_t *bytes_written, TickType_t ticks_to_wait)
{
    if (buf == NULL || len == 0) 
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int16_t *out = buf;
    int16_t *vol_buf = NULL;

    if (spk_volume < 0.99f) 
    {
        size_t samples = len / sizeof(int16_t);
        vol_buf = (int16_t *)malloc(len);
        if (vol_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < samples; i++) 
        {
            vol_buf[i] = (int16_t)(buf[i] * spk_volume);
        }
        out = vol_buf;
    }

    size_t total = 0;
    esp_err_t ret = ESP_OK;

    // 分块写入数据，每次写入 AUDIO_FRAME_BYTES ，避免一次阻塞太久，给 WiFi 让出 CPU
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
    return pcm_queue;
}

/* ── 异步播放 ── */
typedef struct {
    int16_t *buf;
    size_t  len;
    SemaphoreHandle_t done_sem;   /* 调用方等待播放完成 */
} play_job_t;

static QueueHandle_t play_queue = NULL;
static TaskHandle_t player_task = NULL;
static volatile bool spk_busy = false;   /* 播放中标志：判定扬声器是否在工作 */

static void player_task_func(void *arg)
{
    (void)arg;
    play_job_t job;

    while (1) {
        if (xQueueReceive(play_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        spk_busy = true;
        send_audio(job.buf, job.len, NULL, portMAX_DELAY);
        free(job.buf);
        spk_busy = false;
        if (job.done_sem) {
            xSemaphoreGive(job.done_sem);
        }
    }
}

bool i2s_play_async(int16_t *buf, size_t len)
{
    if (!play_queue || !buf || len == 0) {
        if (buf) free(buf);
        return false;
    }
    play_job_t job = { .buf = buf, .len = len, .done_sem = NULL };
    /* 播放是实时的：队列满时等玩家任务腾出槽位（最多 2s），
     * 避免 TTS 音频因队列满被直接丢弃导致"有回复没声音"。 */
    if (xQueueSend(play_queue, &job, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "play queue full, drop %u bytes", (unsigned)len);
        free(buf);
        return false;
    }
    return true;
}

void i2s_play_wait_done(TickType_t timeout)
{
    if (!player_task) return;
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return;

    play_job_t job = { .buf = NULL, .len = 0, .done_sem = sem };
    xQueueSend(play_queue, &job, portMAX_DELAY);
    xSemaphoreTake(sem, timeout);
    vSemaphoreDelete(sem);
}

bool i2s_play_is_busy(void)
{
    if (!play_queue) return false;
    return spk_busy || uxQueueMessagesWaiting(play_queue) > 0;
}

static bool pcm_queue_send(const audio_frame_t *frame)
{
    if (pcm_queue == NULL) {
        return false;
    }
    // 超时 0：队列满则丢帧，避免 mic 堵死拖垮 WiFi 
    if (xQueueSend(pcm_queue, frame, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "pcm queue full, drop frame type=%d", (int)frame->type);
        return false;
    }
    return true;
}

// 按键采集任务：按下读麦入 PCM 队列，松开发 END 
static void mic_task(void *arg)
{
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
            mic_warmed = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!press) 
        {
            ESP_LOGI(TAG, "record start");
            press = true;
            mic_warmed = false;
        }

        if (!mic_warmed) 
        {
            int16_t dump[AUDIO_FRAME_SAMPLES];
            for (int i = 0; i < 4; i++) 
            {
                receive_audio(dump, AUDIO_FRAME_BYTES, &bytes_read, pdMS_TO_TICKS(100));
            }
            mic_warmed = true;
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
    if (pcm_queue != NULL)
    {
        return ESP_OK;
    }

    size_t pcm_queue_bytes = PCM_QUEUE_LEN * sizeof(audio_frame_t);
    pcm_queue_storage = heap_caps_malloc(pcm_queue_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm_queue_storage) {
        pcm_queue = xQueueCreateStatic(PCM_QUEUE_LEN, sizeof(audio_frame_t),
                                       pcm_queue_storage, &pcm_queue_static);
    }
    if (pcm_queue == NULL)
    {
        free(pcm_queue_storage);
        pcm_queue_storage = NULL;
        ESP_LOGE(TAG, "create PCM queue failed: %u bytes, internal=%u, spiram=%u",
                 (unsigned)pcm_queue_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    /* mic_task_handle 已由 AFE feed/fetch 替代，仅保留队列 */
    mic_task_handle = NULL;

    /* 异步播放队列 + 任务 */
    if (play_queue == NULL) {
        /* 队列够大：一次性 TTS 可能包含几十个小分片，避免 i2s_play_async 因
         * 队列满丢音频（配合 i2s_play_async 内部 2s 阻塞等待更保险）。 */
        play_queue = xQueueCreate(256, sizeof(play_job_t));
        if (play_queue && player_task == NULL) {
            xTaskCreate(player_task_func, "i2s_play", 4096, NULL, 18, &player_task);
        }
    }

    ESP_LOGI(TAG, "pcm queue ready: %d frames (AFE mode)", PCM_QUEUE_LEN);
    return ESP_OK;
}
