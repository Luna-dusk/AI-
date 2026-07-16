#ifndef AI_CLIENT_H
#define AI_CLIENT_H

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"

#define SERVER_URL      "http://81.71.24.233/api/chat"
#define ASR_SERVER_URL  "http://81.71.24.233/api/asr"

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
int ai_client_upload_asr(const uint8_t *wav_data, int wav_len,
                         char **resp_buf, int resp_cap);

#endif /* AI_CLIENT_H */
