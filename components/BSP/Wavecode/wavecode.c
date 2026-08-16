#include "wavecode.h"
#include <string.h>

// 小端写入辅助
static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8)  & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int wav_build(uint8_t *out_buf,
              const int16_t *pcm,
              size_t pcm_bytes,
              uint32_t sr,
              uint16_t channels,
              size_t *out_size)
{
    if (!out_buf || !pcm || pcm_bytes == 0) {
        return -1;
    }

    uint16_t bits   = 16;
    uint16_t blk    = channels * (bits / 8);      /* 每采样块字节数 */
    uint32_t byterate = sr * blk;                  /* 字节率 */
    uint32_t total  = pcm_bytes + WAV_HEADER_SIZE; /* WAV 文件总长 */

    uint8_t *h = out_buf;

    memcpy(h, "RIFF", 4);             h += 4;
    write_le32(h, total - 8);         h += 4; 
    memcpy(h, "WAVE", 4);             h += 4;

    memcpy(h, "fmt ", 4);             h += 4;
    write_le32(h, 16);                h += 4;
    write_le16(h, 1);                 h += 2;
    write_le16(h, channels);          h += 2;
    write_le32(h, sr);                h += 4;
    write_le32(h, byterate);          h += 4;
    write_le16(h, blk);               h += 2;
    write_le16(h, bits);              h += 2;

    memcpy(h, "data", 4);             h += 4;
    write_le32(h, (uint32_t)pcm_bytes); h += 4;

   
    memcpy(h, pcm, pcm_bytes);

    if (out_size) {
        *out_size = total;
    }
    return 0;
}
