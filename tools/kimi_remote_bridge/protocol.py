"""Kimi voice remote control-channel protocol: newline-delimited JSON over BLE.

Pure Python (no bleak dependency) so it can be unit-tested on any host.

Message framing matches the firmware in ``main/remote_protocol.c``:

* One logical message is a JSON object encoded as UTF-8 (no trailing newline
  is required on the wire; the firmware parses one reassembled buffer).
* Long messages are split into fragments.  Every fragment carries a 1-byte
  prefix: ``0x01`` = more fragments follow, ``0x00`` = last fragment (a
  single-fragment message also uses ``0x00``).  Strip the prefix and
  concatenate the payloads to reassemble.
* Each BLE write/notify payload is at most ``MTU - 3`` bytes *including* the
  fragment prefix, so the maximum fragment payload is ``MTU - 4``.
"""

from __future__ import annotations

import json
from typing import Any, Dict, List, Optional

# Fragment prefix values.
FRAG_MORE = 0x01
FRAG_LAST = 0x00

# Audio frame layout (AUDIO_TX notify, binary).
AUDIO_MAGIC = 0xA5
AUDIO_HDR_LEN = 6
AUDIO_FLAG_LAST = 0x01

# Well-known UUIDs (must match the firmware GATT table).
SERVICE_UUID = "6b6b0001-5052-4f4a-9e43-544f4c4b494d"
CTRL_RX_UUID = "6b6b0002-5052-4f4a-9e43-544f4c4b494d"   # PC writes
CTRL_TX_UUID = "6b6b0003-5052-4f4a-9e43-544f4c4b494d"   # device notifies
AUDIO_TX_UUID = "6b6b0004-5052-4f4a-9e43-544f4c4b494d"   # device notifies

DEFAULT_REASM_CAPACITY = 4096


class ProtocolError(Exception):
    """Raised on malformed fragments/frames or oversized messages."""


# ---------------------------------------------------------------------------
# JSON line codec
# ---------------------------------------------------------------------------

def encode_line(obj: Dict[str, Any]) -> bytes:
    """Serialize a message dict to the compact UTF-8 JSON line format."""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def decode_line(data: bytes) -> Dict[str, Any]:
    """Parse one reassembled message buffer into a dict.

    Raises :class:`ProtocolError` on invalid JSON or a non-object root.
    """
    try:
        obj = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ProtocolError(f"invalid JSON line: {exc}") from exc
    if not isinstance(obj, dict):
        raise ProtocolError("JSON root is not an object")
    return obj


# ---------------------------------------------------------------------------
# Message builders (PC -> device)
# ---------------------------------------------------------------------------

def build_hello(v: int = 1) -> bytes:
    return encode_line({"t": "hello", "v": v})


def build_ping() -> bytes:
    return encode_line({"t": "ping"})


def build_asr(msg_id: int, text: Optional[str] = None,
              err: Optional[str] = None) -> bytes:
    """Build an ASR result message.  Exactly one of ``text``/``err``."""
    if (text is None) == (err is None):
        raise ValueError("exactly one of text/err must be given")
    obj: Dict[str, Any] = {"t": "asr", "id": msg_id}
    if text is not None:
        obj["text"] = text
    else:
        obj["err"] = err
    return encode_line(obj)


def build_perm_req(msg_id: int, text: str, opts: List[str],
                   timeout: int = 60) -> bytes:
    if not opts:
        raise ValueError("perm_req needs at least one option")
    return encode_line({"t": "perm_req", "id": msg_id, "text": text,
                        "opts": list(opts), "timeout": timeout})


def build_perm_cancel(msg_id: int) -> bytes:
    return encode_line({"t": "perm_cancel", "id": msg_id})


# ---------------------------------------------------------------------------
# Fragmentation (mirrors rp_frag_count / rp_frag_get in the firmware)
# ---------------------------------------------------------------------------

def frag_payload_for_mtu(mtu: int) -> int:
    """Maximum fragment payload (excluding the 1-byte prefix) for a given ATT MTU.

    Each write/notify is at most ``MTU - 3`` bytes including the prefix.
    """
    payload = mtu - 3 - 1
    if payload < 8:
        payload = 8
    return payload


def frag_count(message_len: int, frag_payload: int) -> int:
    if frag_payload == 0:
        return 0
    return (message_len + frag_payload - 1) // frag_payload


def frag_get(message: bytes, frag_payload: int, index: int) -> bytes:
    """Return fragment ``index`` of ``message`` (prefix byte + payload)."""
    offset = index * frag_payload
    if offset >= len(message) and len(message) > 0:
        raise IndexError("fragment index out of range")
    chunk = len(message) - offset
    if chunk > frag_payload:
        chunk = frag_payload
    last = (offset + chunk) >= len(message)
    prefix = FRAG_LAST if last else FRAG_MORE
    return bytes([prefix]) + message[offset:offset + chunk]


def fragment(message: bytes, frag_payload: int) -> List[bytes]:
    """Split ``message`` into a list of fragments."""
    return [frag_get(message, frag_payload, i) for i in range(frag_count(len(message), frag_payload))]


class Reassembler:
    """Reassembles fragmented control-channel messages.

    Feed raw notify/write payloads (each with the 1-byte fragment prefix) to
    :meth:`feed`; it returns the complete message bytes once the last
    fragment arrives, otherwise ``None``.
    """

    def __init__(self, capacity: int = DEFAULT_REASM_CAPACITY) -> None:
        self.capacity = capacity
        self._buf = bytearray()
        self.in_progress = False

    def reset(self) -> None:
        self._buf.clear()
        self.in_progress = False

    def feed(self, frame: bytes) -> Optional[bytes]:
        if not frame:
            self.reset()
            raise ProtocolError("empty fragment")
        more = frame[0] != FRAG_LAST
        chunk = frame[1:]
        if len(self._buf) + len(chunk) > self.capacity:
            self.reset()
            raise ProtocolError(
                f"reassembled message exceeds capacity {self.capacity}")
        self._buf.extend(chunk)
        self.in_progress = more
        if more:
            return None
        out = bytes(self._buf)
        self.reset()
        return out


# ---------------------------------------------------------------------------
# Audio frames (mirrors rp_audio_pack / rp_audio_parse)
# ---------------------------------------------------------------------------

class AudioFrame:
    """One parsed AUDIO_TX frame."""

    __slots__ = ("seq", "flags", "payload")

    def __init__(self, seq: int, flags: int, payload: bytes) -> None:
        self.seq = seq
        self.flags = flags
        self.payload = payload

    @property
    def is_last(self) -> bool:
        return bool(self.flags & AUDIO_FLAG_LAST)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"AudioFrame(seq={self.seq}, flags=0x{self.flags:02x}, "
                f"payload_len={len(self.payload)})")


def audio_pack(seq: int, flags: int, payload: bytes) -> bytes:
    n = len(payload)
    if seq < 0 or seq > 0xFFFF or n > 0xFFFF:
        raise ValueError("seq/payload length out of range")
    return bytes([AUDIO_MAGIC, seq & 0xFF, (seq >> 8) & 0xFF, flags,
                  n & 0xFF, (n >> 8) & 0xFF]) + payload


def audio_parse(frame: bytes) -> AudioFrame:
    """Parse one AUDIO_TX frame; raises :class:`ProtocolError` on corruption."""
    if len(frame) < AUDIO_HDR_LEN:
        raise ProtocolError("audio frame too short")
    if frame[0] != AUDIO_MAGIC:
        raise ProtocolError(f"bad audio magic 0x{frame[0]:02x}")
    seq = frame[1] | (frame[2] << 8)
    flags = frame[3]
    payload_len = frame[4] | (frame[5] << 8)
    if len(frame) != AUDIO_HDR_LEN + payload_len:
        raise ProtocolError(
            f"audio frame length mismatch: got {len(frame)}, "
            f"expected {AUDIO_HDR_LEN + payload_len}")
    return AudioFrame(seq, flags, frame[AUDIO_HDR_LEN:])
