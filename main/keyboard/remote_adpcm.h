// main/remote_adpcm.h
// IMA/DVI ADPCM 编解码(纯 C,无依赖,主机可测)。
// 为抗 BLE 丢帧,每个音频帧独立携带预测值/步进索引状态头(4 字节),
// 帧与帧之间状态不延续;丢帧只影响该帧本身。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 16kHz/16bit 单声道,10ms 一帧
#define RP_ADPCM_SAMPLE_RATE 16000u
#define RP_ADPCM_SAMPLES_PER_FRAME 160u
#define RP_ADPCM_STATE_LEN 4u

// samples 个采样编码后的字节数(含 4 字节状态头;nibbles 按 2 个采样 1 字节向上取整)
size_t rp_adpcm_frame_bytes(size_t samples);

// 编码一帧。out 容量至少 rp_adpcm_frame_bytes(samples)。
// prev/index 为编解码器状态(调用方可在帧间保留,但每帧输出已自带状态头)。
void rp_adpcm_encode(const int16_t *pcm, size_t samples,
                     int16_t *prev, int16_t *index, uint8_t *out);

// 解码一帧(与编码对称;主机测试与 PC 端 Python 解码器共享同一算法定义)。
void rp_adpcm_decode(const uint8_t *in, size_t samples,
                     int16_t *prev, int16_t *index, int16_t *pcm_out);

#ifdef __cplusplus
}
#endif
