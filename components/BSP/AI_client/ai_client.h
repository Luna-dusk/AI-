#ifndef AI_CLIENT_H
#define AI_CLIENT_H

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_mac.h"

#define SERVER_URL      "http://81.71.24.233/api/chat"
#define ASR_SERVER_URL  "http://81.71.24.233/api/asr"
#define ASR_SERVER_URL_FALLBACK "http://81.71.24.233/asr"
#define CMD_TTS_URL     "http://81.71.24.233/api/cmd_tts"
#define WS_SERVER_URL   "ws://81.71.24.233/ws"
#define HEARTBEAT_URL   "http://81.71.24.233/manage-api/devices/heartbeat"
#define DEVICE_NAME     "小阁-01"
/* 设备标识：必须与网页端绑定记录里的设备标识逐字符一致。
 * 当前与网页端一致，固定为 ESP32-S3-001。网页端换绑时只改这一处。 */
#define DEVICE_KEY      "ESP32-S3-001"

void device_key_str(char *buf, size_t sz);

/* 文本对话（原有接口） */
int ai_client_send_request(const char *post_data, char *response_buf, int buf_size);

/*
 * ASR 音频上传：发送 WAV 二进制数据，接收 JSON 响应。
 *
 * wav_data     WAV 文件二进制（含 44 字节头）
 * wav_len      WAV 数据字节数
 * resp_buf     [out] 响应 JSON 字符串（调用者分配，内部会 realloc 扩容）
 * resp_cap     resp_buf 初始容量
 *
 * 返回 >0 : 响应字节数
 * 返回 <0 : 失败
 *
 * 服务器响应格式（JSON）：
 * {
 *   "reply": "LLM回复文本",
 *   "tts":   "base64编码的WAV音频"
 * }
 */
int ai_client_upload_asr(const uint8_t *wav_data, int wav_len,char **resp_buf, int resp_cap);

/*
 * 本地命令 TTS 回复：POST {"cmd": id} 到 /api/cmd_tts。
 * 返回 JSON（含 reply 文本 + base64 WAV），不经过 ASR/LLM。
 * resp_buf 由调用者分配，内部 realloc 扩容；返回 >0 响应字节数，<0 失败。
 */
int ai_client_cmd_tts(int cmd_id, char **resp_buf, int resp_cap);

/* 设备心跳：向服务器报告设备在线，首次调用自动注册 */
int ai_client_heartbeat(void);

/* ── WebSocket 流式语音接口 ── */

/*
 * 初始化 WebSocket 连接，发送 handshake。
 * 返回 0 成功，<0 失败。
 */
int ws_client_init(void);

/* 关闭 WebSocket 连接 */
void ws_client_deinit(void);

/* 发送 PCM 音频块 (binary frame) */
int ws_client_send_pcm(const uint8_t *pcm_data, int pcm_len);

/* True only while the underlying client still has a usable connection. */
bool ws_client_is_connected(void);

/* Whether the current completed stream delivered at least one TTS chunk. */
bool ws_client_has_tts(void);

/*
 * 结束一句话，发送 "END" 并等待服务器返回结果（非流式，兼容旧模式）。
 * result     [out] 指向静态缓冲区的指针，内容为 JSON 字符串
 * result_len [out] JSON 长度
 * 返回 0 成功，<0 失败/超时
 */
int ws_client_end_utterance(const char **result, int *result_len);

/*
 * 流式模式：发送结束信号，不阻塞等待。
 * 服务器会通过 WS 持续推送 {"type":"reply"/"tts"/"done"} 消息，
 * ws_event_handler 自动处理并播放 TTS。
 * 返回 0 成功，<0 失败。
 */
int ws_client_send_end(void);

/*
 * 流式模式：阻塞等待服务器发送 {"type":"done"} 或超时。
 * timeout 为 FreeRTOS tick 数，portMAX_DELAY 表示无限等待。
 * 返回 0 成功（收到 done），<0 超时。
 */
int ws_client_wait_done(TickType_t timeout);

/*
 * 流式模式：获取服务器流式返回的文本回复。
 * 返回文本指针（静态缓冲区），如果还没收到返回 NULL。
 */
const char *ws_client_get_reply(void);

/*
 * 服务器推送回调：所有 WS 文本消息（解析出 type 后）都会调用。
 * 内部类型 ready/reply/tts/done 已由 raw_ws 处理；main 侧用它接收
 * settings/chat 等网页端下发的消息（之前固件完全没有这个入口）。
 */
typedef void (*ws_text_cb_t)(const char *type, const char *json);
void ws_client_set_text_cb(ws_text_cb_t cb);

#endif /* AI_CLIENT_H */
