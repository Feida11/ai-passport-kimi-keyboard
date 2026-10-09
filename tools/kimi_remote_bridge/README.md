**English** · [简体中文](README.zh_CN.md)

# Kimi Voice Remote Bridge

Windows PC-side bridge for the **Kimi voice-programming remote** (FoloToy AI
Passport, ESP32-C3). The device firmware pairs over BLE and speaks the
newline-delimited-JSON control protocol plus binary ADPCM audio frames defined
in `../../main/remote_protocol.c`; this tool is the matching PC half. It does
**not** touch firmware code.

What it does:

1. Connects to the BLE peripheral advertised as `KimiRemote`, subscribes to
   the control and audio notification characteristics, and keeps the link up
   with automatic reconnect (exponential backoff, capped at 30 s).
2. Reassembles audio recordings (ADPCM, 16 kHz / 16-bit mono), decodes them
   with the IMA/DVI codec ported 1:1 from `../../main/remote_adpcm.c`,
   transcribes them with faster-whisper (default `small`, CPU int8, Chinese),
   sends the `asr` result back to the device, and pastes the text into the
   target window (clipboard + optional Enter).
3. Two operating modes selected by `mode` in `config.ini`:
   - `kimi` (default): finds the Kimi Code window by title regex, pastes
     there, and watches the window for permission dialogs with the
     `uiautomation` library; dialogs are forwarded to the device as
     `perm_req`, and the button chosen in the device's `perm_resp` is
     clicked (Esc on cancel/timeout).
   - `universal`: a generic voice keyboard — pastes into whatever window
     currently has focus (Word, WeChat, VS Code, a browser...), with no
     permission watching.

## Layout

| File | Purpose |
| --- | --- |
| `protocol.py` | JSON line codec, fragmentation/reassembly, audio frame pack/parse (pure Python, no bleak) |
| `adpcm.py` | IMA ADPCM codec matching the firmware tables and arithmetic |
| `ble_client.py` | bleak client: scan/connect by name, subscribe, fragmented writes, reconnect |
| `asr.py` | faster-whisper wrapper (`transcribe(pcm16, sample_rate=16000) -> str`) |
| `kimi_inject.py` | Find the Kimi Code window by title regex, paste text via clipboard |
| `perm_watch.py` | Permission-dialog detection/clicking with a pluggable `DialogBackend` |
| `app.py` / `__main__.py` | asyncio orchestration, INI config, logging, graceful exit |
| `tests/` | pytest suite for protocol, ADPCM and dialog parsing (no hardware needed) |

## Setup

```bat
cd tools\kimi_remote_bridge
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
```

Copy `config.example.ini` to `config.ini` and edit as needed (BLE device
name, whisper model, window title regex, button patterns, poll interval).

## Run

```bat
.\.venv\Scripts\python.exe -m app -c config.ini
```

Console logs are timestamped. Stop with Ctrl+C.

## Test

```bat
.\.venv\Scripts\python.exe -m pytest tests -v
```

## Protocol notes (must match the firmware)

* Control channel: newline-delimited JSON. Messages longer than one ATT
  payload are fragmented; every fragment carries a 1-byte prefix (`0x01` =
  more, `0x00` = last, single-fragment messages also use `0x00`). Strip the
  prefix and concatenate. Each write/notify payload is at most `MTU - 3`
  bytes *including* the prefix, so the fragment payload is `MTU - 4` (the
  device exchanges MTU 247, giving 243 payload bytes per fragment).
* Audio frames on AUDIO_TX: `[0]=0xA5 [1..2]=seq LE [3]=flags(bit0=last)
  [4..5]=len LE [6..]=payload`. Seq increases per recording and may skip
  (lost frames); duplicates are dropped.
* ADPCM payload: 4-byte state header (predictor int16 LE, step index int16
  LE) then 2 samples per byte, low nibble first; sign bit `0x8`, magnitude
  bits `0x4/0x2/0x1`; step table (89 entries) and index-delta table (16
  entries) are copied from the firmware. Decoding is per-frame independent.

## Known limitations

* ASR runs in a thread-pool executor; the `small` model on CPU adds a few
  seconds of latency per utterance.
* The permission-dialog heuristic matches buttons by configurable name
  patterns; unusual dialogs may need pattern tweaks in `config.ini`.
* Audio frame seq is a 16-bit per-recording counter; reassembly sorts by
  raw seq, so recordings longer than ~44 minutes (65535 frames) would
  misorder after wraparound.
