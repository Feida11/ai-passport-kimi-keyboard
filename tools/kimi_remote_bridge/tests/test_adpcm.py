"""ADPCM codec tests against firmware semantics (main/remote_adpcm.c)."""

import math

import pytest

from adpcm import (
    INDEX_DELTAS,
    SAMPLES_PER_FRAME,
    STEPS,
    decode_frame,
    encode_frame,
    frame_bytes,
    frames_to_pcm,
)

# Firmware-encoded reference vector: 160 samples of a 440 Hz sine, amplitude
# 12000, 16 kHz, initial state (0, 0).  Encoded by the C code in
# main/remote_adpcm.c; decode must end in state (8574, 56).
REFERENCE_HEX = (
    "0000000070777777778098989abababaaa8910424434343432220190caccbcbcc"
    "babab9918424453333433331180caccccbbbcbbbb9a08323645333443221180a9"
    "ccdbcbcbabab9a08214444434332321281b8eb"
)
REFERENCE_FRAME = bytes.fromhex(REFERENCE_HEX)
REFERENCE_ORIG_FIRST16 = [0, 2063, 4064, 5945, 7649, 9124, 10328, 11225,
                          11787, 11998, 11852, 11353, 10515, 9365, 7935, 6269]


def sine_440(n=SAMPLES_PER_FRAME, amplitude=12000):
    return [int(round(amplitude * math.sin(2 * math.pi * 440 * i / 16000)))
            for i in range(n)]


def test_tables_match_firmware():
    assert len(STEPS) == 89
    assert STEPS[0] == 7 and STEPS[88] == 32767
    assert len(INDEX_DELTAS) == 16
    assert list(INDEX_DELTAS) == [-1, -1, -1, -1, 2, 4, 6, 8,
                                  -1, -1, -1, -1, 2, 4, 6, 8]


def test_frame_bytes():
    assert frame_bytes(0) == 4
    assert frame_bytes(1) == 5
    assert frame_bytes(2) == 5
    assert frame_bytes(160) == 84


def test_reference_vector_decode():
    samples, prev, index = decode_frame(REFERENCE_FRAME, SAMPLES_PER_FRAME)

    assert len(samples) == 160
    # Exact final state, per the firmware test vector.
    assert (prev, index) == (8574, 56)

    # NOTE: the first ~9 samples cannot match the reference originals within
    # 1600 — the encoder starts from step index 0 (step size 7), so the
    # decoded signal ramps up slowly (max error in the first 16 samples is
    # ~9164).  From sample 16 onward the codec has settled and must track the
    # original sine within 1600 (observed max error: 415).
    originals = sine_440()
    for i in range(16, 160):
        assert abs(samples[i] - originals[i]) <= 1600, f"sample {i}"
    # The first sample is exact; the ramp-up is deterministic and was
    # verified byte-for-byte against the firmware vector (encode test below).
    assert samples[0] == 0


def test_reference_vector_encode_reproduces_bytes():
    frame, prev, index = encode_frame(sine_440(), prev=0, index=0)
    assert frame == REFERENCE_FRAME
    assert (prev, index) == (8574, 56)


def test_roundtrip_silence():
    pcm = [0] * 160
    frame, ep, ei = encode_frame(pcm)
    out, dp, di = decode_frame(frame, 160)
    assert out == pcm
    assert (ep, ei) == (dp, di) == (0, 0)


def test_roundtrip_sine_quality():
    # Mirror the firmware host test: 100 ms of 440 Hz sine, amplitude 12000,
    # encoded as 10 consecutive frames with carried-over state (C truncation).
    n = 1600
    pcm = [int(12000 * math.sin(2 * math.pi * 440 * i / 16000))
           for i in range(n)]
    encoded = []
    prev = index = 0
    for off in range(0, n, 160):
        frame, prev, index = encode_frame(pcm[off:off + 160], prev, index)
        encoded.append(frame)
    out = frames_to_pcm(encoded)
    sig = sum(float(s) * s for s in pcm)
    err = sum(float(a - b) * (a - b) for a, b in zip(out, pcm))
    snr = 10 * math.log10(sig / err)
    assert snr > 15.0


def test_roundtrip_extreme_values_do_not_run_away():
    wild = [32767, -32768, 32767, -32768, 0, 0, 100, -100]
    frame, _, _ = encode_frame(wild)
    out, _, _ = decode_frame(frame, len(wild))
    for i in range(len(wild) - 1):
        assert abs(out[i + 1] - out[i]) <= 30000


def test_known_state_header():
    # Header prev=1000, index=10 (step=19); nibble 0 -> +step/8 only.
    # Two zero nibbles: +2 each sample, step index 10 -> 9 -> 8 (delta -1).
    frame = (1000).to_bytes(2, "little", signed=True) + (10).to_bytes(
        2, "little", signed=True) + b"\x00"
    out, prev, index = decode_frame(frame, 2)
    assert out == [1002, 1004]
    assert prev == 1004
    assert index == 8


def test_frame_header_independence():
    # Second frame carries its own state header; decoding it must not
    # depend on the first frame.
    pcm1 = [i * 100 for i in range(160)]
    pcm2 = [-i * 50 for i in range(160)]
    f1, p, i = encode_frame(pcm1, prev=0, index=0)
    f2, _, _ = encode_frame(pcm2, prev=p, index=i)
    assert f1[:4] != f2[:4]
    out, _, _ = decode_frame(f2, 160)
    assert abs(out[159] - pcm2[159]) < 4000


def test_odd_sample_count_pads_high_nibble():
    frame, _, _ = encode_frame([0, 100, -100])
    assert len(frame) == frame_bytes(3)
    out, _, _ = decode_frame(frame, 3)
    assert len(out) == 3


def test_decode_rejects_bad_header():
    with pytest.raises(ValueError):
        decode_frame(b"\x00\x00", 10)  # too short
    bad_index = (0).to_bytes(2, "little") + (500).to_bytes(2, "little")
    with pytest.raises(ValueError):
        decode_frame(bad_index + b"\x00" * 80, 160)


def test_frames_to_pcm_concatenates():
    pcm = sine_440(320)
    f1, p, i = encode_frame(pcm[:160])
    f2, _, _ = encode_frame(pcm[160:], prev=p, index=i)
    joined = frames_to_pcm([f1, f2])
    assert len(joined) == 320
    assert joined[:160] != joined[160:]
