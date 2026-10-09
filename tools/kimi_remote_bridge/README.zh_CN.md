[English](README.md) · **简体中文**

# Kimi 语音编程遥控器 PC 桥接

Windows 电脑端桥接程序,配套 **Kimi 语音编程遥控器**(FoloToy AI Passport,ESP32-C3)。设备固件通过 BLE 通信,控制通道协议(换行分隔 JSON + 二进制 ADPCM 音频帧)定义在 `../../main/remote_protocol.c`,本工具是与之完全对应的 PC 端。本工具**不修改任何固件代码**。

功能:

1. 按广播名连接 `KimiRemote`,订阅控制/音频 notify 特征,断线自动重连(指数退避,上限 30 秒)。
2. 按录音 id 重组音频帧,用与 `../../main/remote_adpcm.c` 逐语义一致的 IMA ADPCM 解码,送 faster-whisper 识别(默认 `small`、CPU int8、中文),把 `asr` 结果回发设备,并把文本粘贴到目标窗口(剪贴板 + 可选回车)。
3. 两种工作模式,由 `config.ini` 的 `mode` 选择:
   - `kimi`(默认):按标题正则找到 Kimi Code 窗口并粘贴;用 `uiautomation` 轮询其权限弹窗,向设备发 `perm_req`,按设备 `perm_resp` 的选项点击对应按钮(取消/超时则按 Esc 或点拒绝)。
   - `universal`:通用语音键盘——直接粘贴到当前焦点窗口(Word、微信、VS Code、浏览器等任意应用),不监视权限弹窗。

## 文件结构

| 文件 | 说明 |
| --- | --- |
| `protocol.py` | JSON 行编解码、分片/重组、音频帧打包/解析(纯 Python,不依赖 bleak) |
| `adpcm.py` | 与固件表和算术完全一致的 IMA ADPCM 编解码 |
| `ble_client.py` | bleak 客户端:按名扫描连接、订阅、分片写、自动重连 |
| `asr.py` | faster-whisper 封装,接口 `transcribe(pcm16, sample_rate=16000) -> str` |
| `kimi_inject.py` | 按窗口标题正则找到 Kimi Code 窗口,剪贴板粘贴文本 |
| `perm_watch.py` | 权限弹窗识别/点击,`DialogBackend` 接口可替换(生产用 uiautomation,测试用 FakeBackend) |
| `app.py` / `__main__.py` | asyncio 编排、INI 配置、日志、优雅退出 |
| `tests/` | pytest:协议、ADPCM、弹窗解析(无需硬件) |

## 安装

```bat
cd tools\kimi_remote_bridge
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
```

将 `config.example.ini` 复制为 `config.ini` 后按需修改(BLE 设备名、whisper 模型、窗口标题正则、按钮匹配模式、轮询间隔等)。

## 运行

```bat
.\.venv\Scripts\python.exe -m app -c config.ini
```

控制台日志带时间戳;Ctrl+C 优雅退出。

## 测试

```bat
.\.venv\Scripts\python.exe -m pytest tests -v
```

## 协议要点(必须与固件一致)

- 控制通道为换行分隔 JSON;超长消息分片,每片首字节 `0x01`=还有后续、`0x00`=最后一片(单片消息也用 `0x00`),去掉首字节拼接。每条 write/notify 载荷 ≤ MTU-3(含分片前缀),故分片载荷为 MTU-4(设备协商 MTU 247,即每片 243 字节载荷)。
- 音频帧(AUDIO_TX):`[0]=0xA5 [1..2]=seq 小端 [3]=flags(bit0=本录音最后一帧) [4..5]=len 小端 [6..]=payload`。seq 每条录音内递增、可能跳号(丢帧);重复帧直接丢弃。
- ADPCM 载荷:4 字节状态头(预测值 int16 小端 + 步进索引 int16 小端),之后每字节 2 个采样、低半字节先;符号位 `0x8`,幅值位 `0x4/0x2/0x1`;步进表(89 项)与索引表(16 项)照抄固件,解码逐帧独立、无帧间依赖。

## 已知限制

- ASR 在线程池中执行,`small` 模型在 CPU 上每段话有几秒延迟。
- 权限弹窗启发式依赖可配置的按钮名称模式,特殊弹窗可能需要在 `config.ini` 中调整模式。
- 音频 seq 是每条录音内 16 位计数,重组按原始 seq 排序,录音超过约 44 分钟(65535 帧)回绕后会乱序。

## 界面文案示例

权限弹窗文本示例:`允许 Kimi 执行终端命令吗?`;选项示例:`总是允许` / `允许一次` / `拒绝`。设备端选项展示与这些文案一致,按钮匹配模式见 `config.ini` 的 `allow_pattern` / `deny_pattern`。
