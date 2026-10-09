# Kimi Voice Keyboard - AI Passport Play

Turn the AI Passport into a **Bluetooth voice keyboard**: speak to the device and
the recognized text is typed into a window on your PC - either Kimi Code (voice
programming + on-device permission-dialog confirmation) or any Windows app
(WeChat, Word, browsers...). The stock XiaoZhi assistant mode is kept: a root
menu on boot lets you pick either mode.

## Features

| Feature | Description |
|---------|-------------|
| Voice typing | UP starts recording, UP/OK stops; text is pasted + Enter on the PC |
| Cancel | DOWN cancels while recording |
| Mode switch | Short-press OK on the keyboard home screen toggles Kimi / Universal |
| Kimi mode | Text is injected into the Kimi Code window; when Kimi Code shows a permission dialog, options appear on the device (UP=other choices, DOWN=cancel, OK=confirm) |
| Universal mode | Text is pasted into whatever window has focus - any Windows app |
| XiaoZhi AI | Stock XiaoZhi assistant retained (bring your own network/agent config) |
| Back to menu | Long-press OK (twice within 5s in XiaoZhi mode, as an RF-interference guard) |

## Requirements

- AI Passport (ESP32-C3, 8MB flash)
- A Windows PC with Bluetooth (runs the bridge)
- Device microphone; faster-whisper model auto-downloads on first run (~500MB)

## 1. Flash the firmware

Use the [web flasher](https://ai-passport.folotoy.cn/web-flasher/) with the
`kimi-keyboard-full.bin` from Releases, or:

```bash
esptool.py --chip esp32c3 -p COMx write-flash -fs 8MB 0x0 kimi-keyboard-full.bin
```

## 2. Run the PC bridge

```bash
cd tools/kimi_remote_bridge
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
copy config.example.ini config.ini
HF_ENDPOINT=https://hf-mirror.com .venv\Scripts\python.exe app.py
```

Keep the window running (add to autostart for daily use).

## 3. Use it

1. On the device, pick "Voice Keyboard" in the root menu; the bridge connects
2. The home screen shows the current mode; short-press OK to toggle
3. Universal mode: focus the target window first (e.g. a chat input), then press
   UP and speak
4. Kimi mode: text goes to Kimi Code; confirm its permission dialogs from the
   device keys

## Rebuild

ESP-IDF 6.1:

```bash
python3 scripts/build.py folotoy/ai-passport --name ai-passport
idf.py -p COMx flash
# merged release image:
cd build && esptool.py --chip esp32c3 merge_bin -o full.bin   --flash-mode dio --flash-freq 80m --flash-size 8MB   0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin   0xd000 ota_data_initial.bin 0x20000 xiaozhi.bin 0x600000 generated_assets.bin
```

## Implementation notes

- Dual BLE channels: control (bidirectional JSON, 3x resend + dedup) and audio
  (ADPCM frames with seq). The bridge verifies CCCD subscription by reading it
  back and retries - Windows can silently drop the subscribe write.
- MTU is queried live per connection before recording: 16kHz when ATT MTU >= 96,
  8kHz fallback; the peripheral can initiate an MTU exchange itself.
- ADC buttons share one pin via a resistor ladder; the OK window covers the top
  band (447-1900mV), so WiFi RF bursts can fake a long press. Destructive
  actions therefore require a second long-press within 5 seconds.
- LVGL subset fonts live in `main/keyboard/fonts/`; generators in `tools/fonts/`.

## License & credits

- Firmware based on [FoloToy AI Passport XiaoZhi](https://github.com/FoloToy/folo-ai-passport-xiaozhi) (XiaoZhi v2.5, MIT)
- Voice keyboard mode + bridge are community contributions, also MIT
- faster-whisper (MIT); Bocha/Tavily search APIs are optional and governed by their own terms
