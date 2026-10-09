// main/remote_adpcm.c
// 见 remote_adpcm.h。标准 IMA/DVI ADPCM 步进表。
#include "remote_adpcm.h"

static const int16_t s_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

static const int8_t s_index_deltas[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

size_t rp_adpcm_frame_bytes(size_t samples)
{
    return RP_ADPCM_STATE_LEN + (samples + 1) / 2;
}

void rp_adpcm_encode(const int16_t *pcm, size_t samples,
                     int16_t *prev, int16_t *index, uint8_t *out)
{
    int16_t p = *prev;
    int16_t idx = *index;
    size_t outpos = RP_ADPCM_STATE_LEN;

    // 状态头:低字节在前
    out[0] = (uint8_t)(p & 0xFF);
    out[1] = (uint8_t)((p >> 8) & 0xFF);
    out[2] = (uint8_t)(idx & 0xFF);
    out[3] = (uint8_t)((idx >> 8) & 0xFF);

    for (size_t i = 0; i < samples; i++) {
        int sample = pcm[i];
        int diff = sample - p;
        int sign = 0;
        if (diff < 0) {
            sign = 8;
            diff = -diff;
        }
        int step = s_steps[idx];
        int delta = 0;
        int vdiff = step >> 3;
        if (diff >= step) {
            delta |= 4;
            diff -= step;
            vdiff += step;
        }
        if (diff >= (step >> 1)) {
            delta |= 2;
            diff -= (step >> 1);
            vdiff += (step >> 1);
        }
        if (diff >= (step >> 2)) {
            delta |= 1;
            vdiff += (step >> 2);
        }
        if (sign) {
            p = (int16_t)(p - vdiff);
        } else {
            p = (int16_t)(p + vdiff);
        }
        int ni = idx + s_index_deltas[sign | delta];
        if (ni < 0) ni = 0;
        if (ni > 88) ni = 88;
        idx = (int16_t)ni;

        uint8_t nibble = (uint8_t)(sign | delta);
        if ((i & 1) == 0) {
            out[outpos] = nibble; // 低半字节先
        } else {
            out[outpos] |= (uint8_t)(nibble << 4);
            outpos++;
        }
    }
    if (samples & 1) {
        // 奇数采样:尾字节高半字节补 0
        outpos++;
    }
    (void)outpos;

    *prev = p;
    *index = idx;
}

void rp_adpcm_decode(const uint8_t *in, size_t samples,
                     int16_t *prev, int16_t *index, int16_t *pcm_out)
{
    int16_t p = (int16_t)(in[0] | ((int16_t)in[1] << 8));
    int16_t idx = (int16_t)(in[2] | ((int16_t)in[3] << 8));
    size_t inpos = RP_ADPCM_STATE_LEN;

    for (size_t i = 0; i < samples; i++) {
        uint8_t nibble;
        if ((i & 1) == 0) {
            nibble = in[inpos] & 0x0F;
        } else {
            nibble = (in[inpos] >> 4) & 0x0F;
            inpos++;
        }
        int step = s_steps[idx];
        int vdiff = step >> 3;
        if (nibble & 4) vdiff += step;
        if (nibble & 2) vdiff += (step >> 1);
        if (nibble & 1) vdiff += (step >> 2);
        if (nibble & 8) {
            p = (int16_t)(p - vdiff);
        } else {
            p = (int16_t)(p + vdiff);
        }
        int ni = idx + s_index_deltas[nibble & 0x0F];
        if (ni < 0) ni = 0;
        if (ni > 88) ni = 88;
        idx = (int16_t)ni;
        pcm_out[i] = p;
    }

    *prev = p;
    *index = idx;
}
