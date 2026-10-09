# 进度存档(2026-10-08 晚)

## 状态:双模式固件 v1.0 交付,真机验收基本完成

## 真机验收结果
- 根菜单/字体/BLE 注册/握手/录音→ASR→注入:全部实测通过(丢帧 30/3000)
- 麦克风:codec 唤醒修复 + MTU 自适应(16k@247 / 8k@23,PC 重采样)
- 小智模式:界面交接修复;黑底大字主题已改(待用户在配网/对话页最终确认观感)
- 退回:键盘模式长按 OK 回菜单(实测);小智模式长按 OK 重启回菜单(实测,已加菜单防抖)
- 权限三键流:合成 perm_req 实测通过(设备显示请求/取消键返回/perm_resp 回传 choice=-1)
- 真实 Kimi Code 弹窗的 UIA 检测:结构与逻辑已验证,待日常使用中自然复验

## 关键修复(本会话)
1. NimBLE 注册必须先 ble_gatts_count_cfg(属性池为 0 导致 ENOMEM 崩溃)
2. 音频服务空闲超时关闭 codec 输入 → 录音前 EnableInput(true)
3. MTU 协商在 Windows 上不稳定 → 自适应采样率+帧长;连接间隔 15-30ms
4. rec_start/rec_stop notify 丢包 → 设备重发 3 遍,桥接按 id 去重
5. ASR 超时(设备 30s)+ 桥接看门狗(音频静默 1.5s 兜底结束)
6. perm_watch:窗口精确匹配(KimiCU Overlay 误匹配)、深度 40、最小双角色容器、
   服务端 ClassName 快速查找(0.17s)、原生 InvokePattern 点击
7. 小智屏幕交接 + 菜单按键防抖(600ms)

## 提交
- ai-passport(独立版): 628e5f4, dd5f797 — full.bin 已重编(1.39MB,含全部修复)
- ai-passport-duo: 9581612, 75baf33 — build/merged-binary.bin 7.6MB 最终版已刷入设备

## 日常使用
1. 设备开机 → 根菜单选模式
2. 键盘模式:电脑需运行桥接: cd tools/kimi_remote_bridge && HF_ENDPOINT=https://hf-mirror.com ./.venv/Scripts/python.exe app.py
3. 桥接 config.ini: mode=kimi(默认,含权限监视) / universal(粘贴到焦点窗口)

## 未验证/待办
- 真实 Kimi Code 权限弹窗的端到端点击(下次弹窗时自然验证;检测逻辑已按真实结构校准)
- 小智联网配置后的对话页黑底大字最终观感
- 独立版固件未刷机实测(代码与 duo 同源修复)
- 刷机注意:Windows 下用 idf.py flash 或带 --flash-size 8MB 的 esptool
  (不带 flash-size 会把 bootloader 头改成 2MB 导致启动循环!)

## 2026-10-08 下午排障会话:三 bug 实锤修复 + 音频通道自愈

### 实锤的三个 bug(均有日志证据)
1. **设备显示空白(自始存在,字体是冤枉的)**: `keyboard_mode.cc` 传 `s_asr_err`
   (static 数组,永不 NULL)给 `kb_ui_show_result`,UI 的 `if(err)` 恒真,
   永远渲染错误分支(空串/残留"识别超时"),识别文本被顶掉。
   修复: `s_asr_err[0] ? s_asr_err : NULL`。已验证(错误文本能显示了)。
2. **音频 71% 丢失**: 全局 `s_att_mtu` 每次 CONNECT 重置 23,只被" whichever 连接
   发起 MTU 交换"的事件更新;Windows 幽灵连接把它污染回 23 → 8kHz 降级 +
   每秒 400 notify 洪流 → ENOMEM 丢 1303/1835 帧。
   修复: `kb_ble_att_mtu()` 按连接句柄现查 `ble_att_mtu()` + 未交换时外设侧
   `ble_gattc_exchange_mtu` 主动补一刀。已验证(`mic start: mtu=247 rate=16000`)。
3. **注入粘到悬浮层**: 窗口正则 /Kimi/ 首个匹配到本会话的 KimiCU Overlay。
   修复: 三级优先(精确标题"Kimi Code" → 进程 exe → 排除 Overlay/工具窗),
   多候选全记录。已验证(候选日志正确列出两个窗口并选中 Kimi Code)。

### 音频订阅未落地(缓存问题,已自愈)
- 现象: 控制通道双向正常,音频 notify 100% 被 `s_audio_subscribed=false` 挡下
  (帧根本没发起,串口无 notify 痕迹)。Windows 写音频 CCCD 没落设备(刷机后
  GATT 缓存不一致的经典症状)。
- 修复: 桥接 `_start_notify_verified` —— start_notify 后读回 CCCD 校验,
  未生效重试 3 次。重连后 `audio 通知 CCCD=0100`,音频 333 帧仅丢 13。

### 遗留问题(明天修,全部桥接端,无需刷机)
1. **SetForegroundWindow 前台锁**: 候选窗口已正确选中 [Kimi Code],但激活抛
   `pywintypes.error(0,'SetForegroundWindow')`,且 fallback 里的第二次调用
   在 `try/finally` 中无 except,异常上抛导致粘贴没执行。
   → fallback 全程包 try/except;激活前先查 `GetForegroundWindow()==hwnd`;
   加 Alt 键预输入解除前台锁;**粘贴无论激活成败都继续**。
2. **app.py _finalize 顺序**: 注入失败导致 `_send_asr_text` 没执行,设备收不到
   识别文本(type=3 text=-)。→ 先发设备、再注入(或 try/finally 保证)。
3. **音频质量待查**: RMS=796(17:09 正常时 4244),ASR 出"措施措施"。
   → 听 %TEMP%\kimi_remote_last.wav 判断是说话轻/远还是解码问题。

### 当前状态
- duo 固件(三修复版)已刷入设备;桥接 37 测试全绿,CCCD 验证版运行中
- 设备留键盘模式即可;桥接常驻后台,明早可直接测

## 2026-10-09 上午:全链路打通(端到端实测通过)
- 设备上键录音 → BLE → 桥接 ASR 识别"今天天气很好" → 注入 [Kimi Code]
  窗口并回车发送,设备屏同步显示识别文字(状态机日志证实)
- 关键补丁:
  1. 设备: kb_ble_send_audio 去掉 s_audio_subscribed 闸门(NimBLE notify_custom
     不校验 CCCD;Windows 按客户端状态投递)。同时修 len 上限用实时 MTU,
     补 SUBSCRIBE 事件日志(本次证实 CCCD 订阅 attr=18/21 均 cur_notify=1)
  2. 桥接: 识别结果先发设备再注入(_finalize 顺序);SetForegroundWindow
     前台锁加固(已前台跳过/Alt 键解锁/AttachThreadInput 全防抛,失败照粘贴);
     窗口三级精确匹配;rec_stop 无会话时立即回错(不再等 30s 超时);
     start_notify 后读回 CCCD 验证(注意:Windows 缓存陈旧时读回不可靠,
     会误报 0100,真正有效的是设备端 SUBSCRIBE 日志)
- 本次指标: 409 音频帧丢 85(79% 到达),RMS=6541,3.24s 音频
- 提交: duo 6e34a9b / 桥接(ai-passport) 4354204

### 遗留(不阻塞)
- 丢帧率 21% 可再压(连接间隔/notify 节奏),当前不影响识别
- Windows GATT 缓存根因未清(设备端去闸门已绕开);若反复异常可在
  设备管理器卸载蓝牙设备或删注册表 GattCache 项
- 权限三键流待真实 Kimi Code 弹窗自然复验
