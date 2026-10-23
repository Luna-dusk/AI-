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

/* ── 双麦自适应降噪状态（差分噪声参考 + 归一化 LMS）── */
static int32_t ns_w[I2S_NS_TAP_NUM];         /* 自适应 FIR 权重（Q16） */
static int16_t ns_delay[I2S_NS_TAP_NUM];     /* 差分参考 d 的延迟线 */
static int32_t ns_dc_l = 0, ns_dc_r = 0;     /* 左右声道 DC 估计 */
static volatile bool ns_adapt = true;        /* 采集打开时允许自适应 */
static int16_t *ns_stereo = NULL;            /* 立体声读取缓冲 */
static size_t ns_stereo_cap = 0;
static int32_t ns_sat_count = 0;             /* 输出持续饱和计数：发散保护 */

void i2s_dual_mic_reset(void)
{
    memset(ns_delay, 0, sizeof(ns_delay));
    ns_dc_l = 0;
    ns_dc_r = 0;
    ns_sat_count = 0;
    ns_adapt = true;
    ESP_LOGI(TAG, "dual-mic NS: delay/DC reset, weights kept");
}

void i2s_dual_mic_set_adapt(bool on)
{
    if (ns_adapt != on) {
        ESP_LOGI(TAG, "dual-mic NS adapt %s", on ? "ON" : "OFF");
    }
    ns_adapt = on;
}

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
#if I2S_DUAL_MIC_NS_ENABLE
        /* 双麦：两个 INMP441 共享 SD 线，靠 L/R 分左右声道，数据 L/R 交织 */
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
#else
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
#endif
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
    ESP_LOGI(TAG, "mic I2S ready, dual-mic NS=%s (BCK=%d WS=%d SD=%d)",
             I2S_DUAL_MIC_NS_ENABLE ? "ON" : "OFF",
             (int)I2S_MIC_BCK_PIN, (int)I2S_MIC_WS_PIN, (int)I2S_MIC_DATAIN_PIN);
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
    if (buf == NULL || len == 0 || (len % sizeof(int16_t)) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

#if I2S_DUAL_MIC_NS_ENABLE
    /* len = 单声道输出字节数；立体声交织数据需要 2 倍字节 */
    size_t need = len * 2;
    if (ns_stereo == NULL || ns_stereo_cap < need) {
        heap_caps_free(ns_stereo);
        ns_stereo = (int16_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ns_stereo == NULL) {
            ns_stereo_cap = 0;
            ESP_LOGE(TAG, "dual-mic stereo buffer alloc failed (%u bytes)", (unsigned)need);
            return ESP_ERR_NO_MEM;
        }
        ns_stereo_cap = need;
    }

    size_t real_bytes = 0;
    esp_err_t ret = i2s_read(I2S_NUM_1, ns_stereo, need, &real_bytes, ticks_to_wait);
    if (bytes_read)
    {
        *bytes_read = 0;
    }
    if (ret != ESP_OK)
    {
        return ret;
    }

    size_t pairs = real_bytes / (2 * sizeof(int16_t));
    const int16_t *in = ns_stereo;
    int16_t *out = buf;

    for (size_t i = 0; i < pairs; i++) {
        int32_t l = in[i * 2];
        int32_t r = in[i * 2 + 1];

        /* 单极点 DC 阻塞：去掉 INMP441 直流偏置（~16ms 时间常数） */
        ns_dc_l += ((int32_t)l - ns_dc_l) / 256;
        ns_dc_r += ((int32_t)r - ns_dc_r) / 256;
        l -= ns_dc_l;
        r -= ns_dc_r;

        int32_t s = l + r;   /* 前方语音同时到达两麦：同相增强 */
        int32_t d = l - r;   /* 前方语音抵消、扩散噪声保留：噪声参考 */

        /* 自适应 FIR：用差分参考 d 估计 s 中与噪声相关的分量并减去 */
        int64_t noise_est = 0;
        for (int k = 0; k < I2S_NS_TAP_NUM; k++) {
            noise_est += (int64_t)ns_w[k] * ns_delay[k];
        }
        int32_t e = (s >> 1) - (int32_t)(noise_est >> 16);
        if (e > 32767) e = 32767;
        else if (e < -32768) e = -32768;
        out[i] = (int16_t)e;

        /* 发散保护：输出长时间饱和说明权重跑飞，清零重建 */
        if (e >= 16000 || e <= -16000) {
            if (++ns_sat_count > 8000) {   /* 持续饱和 ~500ms */
                memset(ns_w, 0, sizeof(ns_w));
                ns_sat_count = 0;
                ESP_LOGW(TAG, "dual-mic NS saturated, weights reset");
            }
        } else {
            ns_sat_count = 0;
        }

        /* 滚动差分参考延迟线 */
        for (int k = I2S_NS_TAP_NUM - 1; k > 0; k--) {
            ns_delay[k] = ns_delay[k - 1];
        }
        ns_delay[0] = (int16_t)d;

        /* 归一化 LMS：权重更新量 = mu*e*d/||d||^2，被归一化约束在很小值。
         * 之前步长/权重上限过大，滤波器 1ms 内发散到削波，导致 VAD 永远
         * 认为在说话（不出 END）、PCM 队列被噪声灌满。只在采集打开时更新，
         * 防止锁到扬声器回灌的 TTS 声。 */
        if (ns_adapt && I2S_NS_MU_Q16 > 0) {
            int64_t ref_pow = 0;
            for (int k = 0; k < I2S_NS_TAP_NUM; k++) {
                ref_pow += (int64_t)ns_delay[k] * ns_delay[k];
            }
            if (ref_pow > 0) {
                int64_t common = ((int64_t)I2S_NS_MU_Q16 * e) / (ref_pow + 1);
                for (int k = 0; k < I2S_NS_TAP_NUM; k++) {
                    int64_t upd = common * ns_delay[k];
                    int64_t w = (int64_t)ns_w[k] + upd;
                    if (w > I2S_NS_WEIGHT_MAX) w = I2S_NS_WEIGHT_MAX;
                    else if (w < -I2S_NS_WEIGHT_MAX) w = -I2S_NS_WEIGHT_MAX;
                    ns_w[k] = (int32_t)w;
                }
            }
        }
    }

    if (bytes_read)
    {
        *bytes_read = pairs * sizeof(int16_t);
    }
    return ESP_OK;
#else
    size_t real_byte = 0;
    esp_err_t ret = i2s_read(I2S_NUM_1, buf, len, &real_byte, ticks_to_wait);
    if (bytes_read)
    {
        *bytes_read = real_byte;
    }
    return ret;
#endif
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
