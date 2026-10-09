# Kimi 语音键盘 · AI Passport 玩法

把 AI Passport 变成**蓝牙语音键盘**:对着设备说话,文字自动打进电脑窗口
——可以是 Kimi Code(语音编程 + 权限弹窗遥控确认),也可以是任何 Windows
应用(微信、Word、浏览器…)。同时保留官方小智 AI 模式,开机根菜单二选一。

![模式](https://img.shields.io/badge/modes-2-blue) 固件:双模式(小智 AI / 语音键盘)
桥接:Windows 端 Python 程序(BLE + 本地 faster-whisper 识别)

## 功能一览

| 功能 | 说明 |
|------|------|
| 语音输入 | 上键开始录音,再按上键或 OK 结束,文字自动粘贴到电脑并回车 |
| 取消录音 | 录音中按下键取消 |
| 模式切换 | 键盘主页短按 OK 在「Kimi / 通用」间切换(设备有记忆) |
| Kimi 模式 | 文字注入 Kimi Code 窗口;Kimi Code 弹出权限确认时,设备屏幕显示选项,上键=其他选择、下键=取消、OK=确认 |
| 通用模式 | 文字粘贴到**当前焦点窗口**,任意 Windows 应用可用 |
| 小智 AI | 官方小智模式完整保留(需自行配置网络与智能体) |
| 退回菜单 | 任一模式长按 OK(小智模式需 5 秒内按两次,防误触) |

## 需要准备

- AI Passport(ESP32-C3,8MB)
- 一台带蓝牙的 Windows 电脑(桥接程序跑在这台电脑上)
- 麦克风(设备自带)、faster-whisper 语音识别模型(首次运行自动下载,约 500MB)

## 一、刷固件

浏览器打开[本地玩法安装工具](https://ai-passport.folotoy.cn/web-flasher/),或本仓库
Release 页下载 `kimi-keyboard-full.bin`,用 esptool 从 0x0 刷入:

```bash
esptool.py --chip esp32c3 -p COMx write-flash -fs 8MB 0x0 kimi-keyboard-full.bin
```

刷完开机进入根菜单。

## 二、电脑端桥接

```bash
cd tools/kimi_remote_bridge
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
copy config.example.ini config.ini
HF_ENDPOINT=https://hf-mirror.com .venv\Scripts\python.exe app.py
```

保持窗口常驻(可放开机启动)。

## 三、使用

1. 设备根菜单选「语音键盘」,桥接会自动连上
2. 主页显示当前模式(Kimi / 通用);短按 OK 切换
3. 通用模式:先点一下要输入的窗口(如微信聊天框),再按上键说话
4. Kimi 模式:文字直接进 Kimi Code 对话框;弹出权限确认时看设备屏幕按键选择

## 重新编译

```bash
# ESP-IDF 6.1
python3 scripts/build.py folotoy/ai-passport --name ai-passport
idf.py -p COMx flash        # 开发用
# 发布合并固件:
cd build && esptool.py --chip esp32c3 merge_bin -o full.bin \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xd000 ota_data_initial.bin 0x20000 xiaozhi.bin 0x600000 generated_assets.bin
```

## 设计要点(给想改代码的人)

- **BLE 双通道**:控制通道(双向 JSON,重发 3 次去重)+ 音频通道(ADPCM 帧,
  带 seq)。Windows 的 CCCD 订阅写入可能丢失,桥接侧带"订阅后读回验证+重试"
- **MTU 自适应**:录音时按连接句柄实时查询 ATT MTU,≥96 走 16kHz,否则 8kHz
  降级;外设侧可主动发起 MTU 交换
- **ADC 按键防护**:OK 键判定窗口占 ladder 顶档(447–1900mV),WiFi 射频
  串扰可伪造长按,退出类操作已做二次确认
- 字体:LVGL 子集字体(`main/keyboard/fonts/`),生成脚本在
  `tools/fonts/`(需要 lv_font_conv)

## 许可与致谢

- 固件基于 [FoloToy AI Passport XiaoZhi](https://github.com/FoloToy/folo-ai-passport-xiaozhi)(XiaoZhi v2.5,MIT)
- 语音键盘模式与桥接为社区贡献,同样以 MIT 发布
- faster-whisper(MIT)、博查/Tavily 搜索 API(可选,各自条款)
