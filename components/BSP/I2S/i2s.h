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

// MAX98357 扬声器 (I2S_NUM_0 TX)
#define I2S_SPK_BCK_PIN      15
#define I2S_SPK_WS_PIN       16
#define I2S_SPK_DATAOUT_PIN  7

// 双 INMP441 麦克风 (I2S_NUM_1 RX)
// 两个麦共享 SCK/WS/SD 三根线，靠 L/R 引脚分左右声道：
//   Mic1 L/R -> GND（左声道）  Mic2 L/R -> VDD（右声道）
//   间距建议 5~8cm；SCK/WS 从 ESP32 先到 Mic1 再菊花链到 Mic2，尽量等长。
#define I2S_MIC_BCK_PIN      5
#define I2S_MIC_WS_PIN       4
#define I2S_MIC_DATAIN_PIN   6                         

/* 双麦自适应降噪（差分噪声参考 + 归一化 LMS）
 * 注意：当前置 0 = 单麦模式（只读左声道=Mic1，与加第二个麦之前一致）。
 * 之前双麦音频持续产噪导致 VAD 永远不结束语句、无回复，疑似第二麦接线问题。
 * 排查好第二麦接线后再置 1 恢复双麦；置 1 且 I2S_NS_MU_Q16=0 为纯平均 (L+R)/2。 */
#define I2S_DUAL_MIC_NS_ENABLE   0
#define I2S_NS_TAP_NUM           16     /* 自适应 FIR 抽头数（16kHz 下 1ms） */
#define I2S_NS_MU_Q16            0      /* 自适应步长（Q16）。当前设为 0 = 纯平均 (L+R)/2，最稳。要恢复自适应改为 131(~0.002) */
#define I2S_NS_WEIGHT_MAX        (1 << 18)   /* 权重上限（Q16，~4 倍于典型收敛值，防发散） */

void i2s_dual_mic_reset(void); 
void i2s_dual_mic_set_adapt(bool on);

#define BTN_BOOT_PIN         0

/* 扬声器音量：运行时可变，范围 0.0 ~ 1.0 */
void  i2s_spk_set_volume(float vol);
float i2s_spk_get_volume(void);

// 每帧采样点数；16bit 单声道 → 每帧字节数 = AUDIO_FRAME_SAMPLES * 2
#define AUDIO_FRAME_SAMPLES  128
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * sizeof(int16_t))

// PCM 队列深度：128 帧缓存 (~1 秒) 
#define PCM_QUEUE_LEN        128

//0为数据有效，1为结束无效
typedef enum {
    AUDIO_FRAME_DATA = 0,   
    AUDIO_FRAME_END  = 1,   
} audio_frame_type_t;

typedef struct {
    audio_frame_type_t type;
    size_t bytes;                          
    int16_t data[AUDIO_FRAME_SAMPLES];     
} audio_frame_t;

void init_i2s(void);
void boot_init(void);

esp_err_t receive_audio(int16_t *buf, size_t len, size_t *bytes_read, TickType_t ticks_to_wait);
esp_err_t send_audio(const int16_t *buf, size_t len, size_t *bytes_written, TickType_t ticks_to_wait);

/* 异步播放：buf 所有权转移给播放器，主线程立刻返回，播放器完成后自动 free */
/* Returns true when ownership was transferred to the playback queue. */
bool i2s_play_async(int16_t *buf, size_t len);
/* 等待异步播放完成 */
void i2s_play_wait_done(TickType_t timeout);
/* 检查是否有正在进行的播放 */
bool i2s_play_is_busy(void);

// 创建 PCM 队列（AFE 写入；不再启动 mic 按键任务）
esp_err_t i2s_audio_start(void);

// 业务任务从该队列取 mic 帧（只读句柄）
QueueHandle_t i2s_get_pcm_queue(void);

#endif
