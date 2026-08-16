#ifndef __MULTINET_CMD_H
#define __MULTINET_CMD_H

#include "esp_err.h"
#include <stdint.h>

/* MultiNet 本地命令 ID */
enum {
    MNET_CMD_LIGHT_ON    = 1,
    MNET_CMD_LIGHT_OFF   = 2,
    MNET_CMD_VOL_UP      = 3,
    MNET_CMD_VOL_DOWN    = 4,
    MNET_CMD_SMILE       = 5,
    MNET_CMD_CRY         = 6,
    MNET_CMD_ANGRY       = 7,
    MNET_CMD_MAX         = 8,
};
/* 初始化模型 + 注册拼音命令表；需 menuconfig 已选 mn6_cn 并全量 flash */
esp_err_t multinet_cmd_init(void);
void      multinet_cmd_deinit(void);

/* 每帧喂 AFE fetch 的 data；返回：0=检测中, >0=命中的 cmd_id, -1=超时无命令 */
int       multinet_cmd_feed(const int16_t *pcm);

/* 进入 LISTENING 时调用，清空检测状态 */
void      multinet_cmd_reset(void);

/* 本地执行一条命令 */
void      multinet_cmd_exec(int cmd_id);

/* 命令命中后的语音回复回调（由 main 注册，从服务器拉取对应 TTS 播放） */
typedef void (*multinet_reply_cb_t)(int cmd_id);
void      multinet_cmd_set_reply_cb(multinet_reply_cb_t cb);

/* 返回 dbtect 需要的采样点数（应与 AFE fetch 帧长一致） */
int       multinet_cmd_chunksize(void);

#endif
