"""Protocol tests: JSON line codec, fragmentation/reassembly, audio frames."""

import pytest

import protocol as P
from protocol import (
    ProtocolError,
    Reassembler,
    audio_pack,
    audio_parse,
    build_asr,
    build_hello,
    build_perm_cancel,
    build_perm_req,
    build_ping,
    decode_line,
    encode_line,
    frag_count,
    frag_get,
    frag_payload_for_mtu,
    fragment,
)


def test_encode_decode_line_chinese():
    obj = {"t": "perm_req", "id": 3, "text": "允许访问文件吗？",
           "opts": ["允许一次", "总是允许", "拒绝"], "timeout": 60}
    line = encode_line(obj)
    assert decode_line(line) == obj
    # Compact, UTF-8 (no \uXXXX escapes for Chinese).
    assert "允许".encode("utf-8") in line
    assert b", " not in line


def test_decode_line_rejects_garbage():
    with pytest.raises(ProtocolError):
        decode_line(b"not json")
    with pytest.raises(ProtocolError):
        decode_line(b"[1,2,3]")  # non-object root


def test_builders():
    assert decode_line(build_hello()) == {"t": "hello", "v": 1}
    assert decode_line(build_hello(2)) == {"t": "hello", "v": 2}
    assert decode_line(build_ping()) == {"t": "ping"}
    assert decode_line(build_asr(7, text="你好")) == {"t": "asr", "id": 7, "text": "你好"}
    assert decode_line(build_asr(7, err="no speech")) == {"t": "asr", "id": 7, "err": "no speech"}
    assert decode_line(build_perm_req(1, "继续?", ["允许一次", "拒绝"], 60)) == {
        "t": "perm_req", "id": 1, "text": "继续?",
        "opts": ["允许一次", "拒绝"], "timeout": 60}
    assert decode_line(build_perm_cancel(9)) == {"t": "perm_cancel", "id": 9}


def test_build_asr_requires_exactly_one_field():
    with pytest.raises(ValueError):
        build_asr(1)
    with pytest.raises(ValueError):
        build_asr(1, text="a", err="b")


def test_build_perm_req_requires_options():
    with pytest.raises(ValueError):
        build_perm_req(1, "x", [])


# ---------------------------------------------------------------------------
# Fragmentation
# ---------------------------------------------------------------------------

def test_frag_count():
    assert frag_count(0, 10) == 0
    assert frag_count(1, 10) == 1
    assert frag_count(10, 10) == 1
    assert frag_count(11, 10) == 2
    assert frag_count(0, 0) == 0


def test_single_fragment_message():
    frags = fragment(b"hello", 244)
    assert len(frags) == 1
    assert frags[0] == b"\x00hello"


def test_multi_fragment_roundtrip():
    message = b'{"t":"perm_req","id":1,"text":"' + "长".encode() * 500 + b'"}'
    frags = fragment(message, 20)
    assert len(frags) == frag_count(len(message), 20)
    assert all(f[0] == P.FRAG_MORE for f in frags[:-1])
    assert frags[-1][0] == P.FRAG_LAST
    reasm = Reassembler(capacity=4096)
    result = None
    for f in frags:
        result = reasm.feed(f)
        if f is not frags[-1]:
            assert result is None
    assert result == message


def test_frag_get_boundaries():
    msg = b"abcdefghij"  # 10 bytes, payload 4 -> 3 fragments: 4/4/2
    assert frag_get(msg, 4, 0) == b"\x01abcd"
    assert frag_get(msg, 4, 1) == b"\x01efgh"
    assert frag_get(msg, 4, 2) == b"\x00ij"
    with pytest.raises(IndexError):
        frag_get(msg, 4, 3)


def test_reassembler_incremental_and_reset():
    r = Reassembler(capacity=16)
    assert r.feed(b"\x01he") is None
    assert r.in_progress
    assert r.feed(b"\x00llo") == b"hello"
    assert not r.in_progress
    # Reusable after completion.
    assert r.feed(b"\x00xy") == b"xy"


def test_reassembler_overflow_resets():
    r = Reassembler(capacity=4)
    r.feed(b"\x01he")
    with pytest.raises(ProtocolError):
        r.feed(b"\x01llo")
    assert not r.in_progress
    # Clean state after error.
    assert r.feed(b"\x00ab") == b"ab"


def test_reassembler_rejects_empty_fragment():
    r = Reassembler()
    with pytest.raises(ProtocolError):
        r.feed(b"")


def test_frag_payload_for_mtu():
    assert frag_payload_for_mtu(247) == 243
    assert frag_payload_for_mtu(23) == 19
    assert frag_payload_for_mtu(11) == 8  # clamped floor
    assert frag_payload_for_mtu(1) == 8


# ---------------------------------------------------------------------------
# Audio frames
# ---------------------------------------------------------------------------

def test_audio_pack_parse_roundtrip():
    payload = bytes(range(80))
    frame = audio_pack(0x1234, P.AUDIO_FLAG_LAST, payload)
    assert len(frame) == 6 + 80
    parsed = audio_parse(frame)
    assert parsed.seq == 0x1234
    assert parsed.flags == P.AUDIO_FLAG_LAST
    assert parsed.is_last
    assert parsed.payload == payload


def test_audio_parse_rejects_corruption():
    frame = audio_pack(1, 0, b"1234")
    with pytest.raises(ProtocolError):
        audio_parse(b"\x00" + frame[1:])          # bad magic
    with pytest.raises(ProtocolError):
        audio_parse(frame[:-1])                    # length mismatch
    with pytest.raises(ProtocolError):
        audio_parse(b"\xa5\x01")                   # too short
