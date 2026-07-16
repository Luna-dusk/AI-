#ifndef __WAVECODE_H
#define __WAVECODE_H

#include <stdint.h>
#include <stddef.h>

#define WAV_HEADER_SIZE  44

/*
 * 在 out_buf 中构建完整 WAV 文件（44 字节头 + PCM 数据）。
 * out_buf  调用者分配，至少 pcm_bytes + WAV_HEADER_SIZE 字节
 * pcm      原始 PCM 采样（int16, 16-bit）
 * pcm_bytes PCM 数据字节数
 * sr       采样率（Hz）
 * channels 声道数
 * out_size [out] 生成的 WAV 总字节数
 * 返回 0 成功，-1 参数错误
 */
int wav_build(uint8_t *out_buf,
              const int16_t *pcm,
              size_t pcm_bytes,
              uint32_t sr,
              uint16_t channels,
              size_t *out_size);

#endif /* __WAVECODE_H */
