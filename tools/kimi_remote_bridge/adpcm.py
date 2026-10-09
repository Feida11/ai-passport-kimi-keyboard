"""IMA/DVI ADPCM codec matching ``main/remote_adpcm.c`` in the firmware.

Audio payload of every AUDIO_TX frame: 16 kHz / 16-bit mono, 160 samples.
Each frame starts with a 4-byte state header (predictor int16 LE + step index
int16 LE); the two nibbles of every following byte each encode one sample,
low nibble first.  Frames are decoded independently (no cross-frame state).

The step table and index-delta table below are copied verbatim from the
firmware; the encode/decode arithmetic mirrors the C code, including the
int16 wrapping of the predictor.
"""

from __future__ import annotations

from typing import List, Sequence, Tuple

# 89 entries, verbatim from remote_adpcm.c.
STEPS = (
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
)

# 16 entries, verbatim from remote_adpcm.c.
INDEX_DELTAS = (-1, -1, -1, -1, 2, 4, 6, 8,
                -1, -1, -1, -1, 2, 4, 6, 8)

STEP_MAX_INDEX = len(STEPS) - 1  # 88
STATE_LEN = 4
SAMPLES_PER_FRAME = 160


def _wrap16(x: int) -> int:
    """Wrap to signed int16 range, matching the C (int16_t) casts."""
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def frame_bytes(samples: int) -> int:
    """Encoded frame size for ``samples`` samples (state header + nibbles)."""
    return STATE_LEN + (samples + 1) // 2


def encode_frame(pcm: Sequence[int], prev: int = 0, index: int = 0
                 ) -> Tuple[bytes, int, int]:
    """Encode up to one frame of samples.

    Returns ``(frame_bytes, new_prev, new_index)``.  Odd sample counts pad
    the trailing high nibble with zero, as in the firmware.
    """
    samples = len(pcm)
    out = bytearray(frame_bytes(samples))
    p = _wrap16(prev)
    idx = index
    out[0] = p & 0xFF
    out[1] = (p >> 8) & 0xFF
    out[2] = idx & 0xFF
    out[3] = (idx >> 8) & 0xFF

    pos = STATE_LEN
    for i in range(samples):
        sample = _wrap16(pcm[i])
        diff = sample - p
        sign = 0
        if diff < 0:
            sign = 8
            diff = -diff
        step = STEPS[idx]
        delta = 0
        vdiff = step >> 3
        if diff >= step:
            delta |= 4
            diff -= step
            vdiff += step
        if diff >= (step >> 1):
            delta |= 2
            diff -= (step >> 1)
            vdiff += (step >> 1)
        if diff >= (step >> 2):
            delta |= 1
            vdiff += (step >> 2)
        if sign:
            p = _wrap16(p - vdiff)
        else:
            p = _wrap16(p + vdiff)
        ni = idx + INDEX_DELTAS[sign | delta]
        if ni < 0:
            ni = 0
        if ni > STEP_MAX_INDEX:
            ni = STEP_MAX_INDEX
        idx = ni

        nibble = sign | delta
        if (i & 1) == 0:
            out[pos] = nibble          # low nibble first
        else:
            out[pos] |= (nibble << 4)
            pos += 1

    return bytes(out), p, idx


def decode_frame(data: bytes, samples: int = None
                 ) -> Tuple[List[int], int, int]:
    """Decode one ADPCM frame.

    Returns ``(pcm_samples, final_prev, final_index)``.  If ``samples`` is
    omitted it is derived from the frame length (2 samples per payload byte).
    """
    if len(data) < STATE_LEN:
        raise ValueError("ADPCM frame shorter than state header")
    if samples is None:
        samples = (len(data) - STATE_LEN) * 2
    p = data[0] | (data[1] << 8)
    if p >= 0x8000:
        p -= 0x10000
    idx = data[2] | (data[3] << 8)
    if idx >= 0x8000:
        idx -= 0x10000
    if idx < 0 or idx > STEP_MAX_INDEX:
        raise ValueError(f"ADPCM step index {idx} out of range")

    need = frame_bytes(samples)
    if len(data) < need:
        raise ValueError(
            f"ADPCM frame too short: need {need} bytes for {samples} "
            f"samples, got {len(data)}")

    out: List[int] = []
    pos = STATE_LEN
    for i in range(samples):
        if (i & 1) == 0:
            nibble = data[pos] & 0x0F
        else:
            nibble = (data[pos] >> 4) & 0x0F
            pos += 1
        step = STEPS[idx]
        vdiff = step >> 3
        if nibble & 4:
            vdiff += step
        if nibble & 2:
            vdiff += (step >> 1)
        if nibble & 1:
            vdiff += (step >> 2)
        if nibble & 8:
            p = _wrap16(p - vdiff)
        else:
            p = _wrap16(p + vdiff)
        ni = idx + INDEX_DELTAS[nibble & 0x0F]
        if ni < 0:
            ni = 0
        if ni > STEP_MAX_INDEX:
            ni = STEP_MAX_INDEX
        idx = ni
        out.append(p)

    return out, p, idx


def frames_to_pcm(frames: Sequence[bytes]) -> List[int]:
    """Decode a sequence of ADPCM frames into one flat sample list."""
    pcm: List[int] = []
    for frame in frames:
        samples, _, _ = decode_frame(frame)
        pcm.extend(samples)
    return pcm
