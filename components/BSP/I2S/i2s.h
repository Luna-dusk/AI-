#ifndef __I2S_H__
#define __I2S_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_types.h"

/* MAX98357 扬声器 (I2S_NUM_0 TX) */
#define I2S_SPK_BCK_PIN      15
#define I2S_SPK_WS_PIN       16
#define I2S_SPK_DATAOUT_PIN  7

/* INMP441 麦克风 (I2S_NUM_1 RX) */
#define I2S_MIC_BCK_PIN      5
#define I2S_MIC_WS_PIN       4
#define I2S_MIC_DATAIN_PIN   6

#define BTN_BOOT_PIN         0

#define SPK_VOLUME           0.5f

/* 每帧采样点数；16bit 单声道 → 每帧字节数 = AUDIO_FRAME_SAMPLES * 2 */
#define AUDIO_FRAME_SAMPLES  128
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * sizeof(int16_t))

/* PCM 队列深度：约 16 帧缓存，网络慢时可丢帧保 WiFi */
#define PCM_QUEUE_LEN        16

typedef enum {
    AUDIO_FRAME_DATA = 0,   /* 一帧麦克风 PCM */
    AUDIO_FRAME_END  = 1,   /* 按键松开，本句结束 */
} audio_frame_type_t;

typedef struct {
    audio_frame_type_t type;
    size_t bytes;                          /* type==DATA 时有效，字节数 */
    int16_t data[AUDIO_FRAME_SAMPLES];     /* PCM 样本 */
} audio_frame_t;

void init_i2s(void);
void boot_init(void);

esp_err_t receive_audio(int16_t *buf, size_t len, size_t *bytes_read, TickType_t ticks_to_wait);
esp_err_t send_audio(const int16_t *buf, size_t len, size_t *bytes_written, TickType_t ticks_to_wait);

/* 创建 PCM 队列 + mic 任务 */
esp_err_t i2s_audio_start(void);

/* 业务任务从该队列取 mic 帧（只读句柄，勿删除） */
QueueHandle_t i2s_get_pcm_queue(void);

#endif
