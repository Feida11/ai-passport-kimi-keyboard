"""Asyncio orchestration for the Kimi voice-programming remote bridge.

Data flow:

* Device audio (AUDIO_TX) -> per-recording frame reassembly -> ADPCM decode
  -> WAV -> faster-whisper ASR -> ``asr`` reply to the device -> text
  injection into Kimi Code (injection failure replies ``err`` instead).
* Kimi Code permission dialogs -> ``perm_req`` to the device -> device
  ``perm_resp`` -> click the mapped button (or Esc on cancel/timeout).

Logging is Chinese with timestamps; Ctrl+C shuts everything down cleanly.
"""

from __future__ import annotations

import argparse
import asyncio
import configparser
import io
import logging
import struct
import time
import sys
import wave
from dataclasses import dataclass, field
from typing import Dict, List, Optional

import adpcm
import protocol as proto
from asr import ASRError, WhisperASR
from ble_client import BleConfig, KimiRemoteClient, NotConnectedError
from kimi_inject import KimiInjector, WindowNotFoundError
from perm_watch import (DEFAULT_ALLOW_PATTERN, DEFAULT_DENY_PATTERN,
                        PermDialog, PermWatcher, UiaBackend, click_choice)

log = logging.getLogger("kimi_remote_bridge")

SAMPLE_RATE = 16000


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

@dataclass
class BridgeConfig:
    # [ble]
    device_name: str = "KimiRemote"
    # [asr]
    model_size: str = "small"
    model_path: str = ""
    language: str = "zh"
    compute_type: str = "int8"
    # [kimi]
    mode: str = "kimi"  # kimi = 定位 Kimi Code 窗口;universal = 粘贴到任意前台窗口
    window_title_regex: str = "Kimi"
    auto_enter: bool = True
    inject_retries: int = 5
    inject_retry_interval: float = 0.5
    # [perm]
    poll_interval: float = 0.6
    allow_pattern: str = DEFAULT_ALLOW_PATTERN
    deny_pattern: str = DEFAULT_DENY_PATTERN
    perm_timeout: int = 60
    # [bridge]
    ping_interval: float = 20.0
    grace_ms: int = 500


def load_config(path: Optional[str]) -> BridgeConfig:
    cfg = BridgeConfig()
    if not path:
        return cfg
    parser = configparser.ConfigParser()
    parser.read(path, encoding="utf-8")

    def get(section, key, fallback):
        return parser.get(section, key, fallback=fallback)

    cfg.device_name = get("ble", "device_name", cfg.device_name)
    cfg.model_size = get("asr", "model_size", cfg.model_size)
    cfg.model_path = get("asr", "model_path", cfg.model_path)
    cfg.language = get("asr", "language", cfg.language)
    cfg.compute_type = get("asr", "compute_type", cfg.compute_type)
    cfg.window_title_regex = get("kimi", "window_title_regex",
                                 cfg.window_title_regex)
    cfg.mode = get("kimi", "mode", cfg.mode)
    cfg.auto_enter = parser.getboolean("kimi", "auto_enter",
                                       fallback=cfg.auto_enter)
    cfg.inject_retries = parser.getint("kimi", "inject_retries",
                                       fallback=cfg.inject_retries)
    cfg.inject_retry_interval = parser.getfloat(
        "kimi", "inject_retry_interval",
        fallback=cfg.inject_retry_interval)
    cfg.poll_interval = parser.getfloat("perm", "poll_interval",
                                        fallback=cfg.poll_interval)
    cfg.allow_pattern = get("perm", "allow_pattern", cfg.allow_pattern)
    cfg.deny_pattern = get("perm", "deny_pattern", cfg.deny_pattern)
    cfg.perm_timeout = parser.getint("perm", "timeout",
                                     fallback=cfg.perm_timeout)
    cfg.ping_interval = parser.getfloat("bridge", "ping_interval",
                                        fallback=cfg.ping_interval)
    cfg.grace_ms = parser.getint("bridge", "grace_ms", fallback=cfg.grace_ms)
    return cfg


# ---------------------------------------------------------------------------
# Recording session
# ---------------------------------------------------------------------------

@dataclass
class Recording:
    rec_id: int
    frames: Dict[int, bytes] = field(default_factory=dict)
    got_last: bool = False
    stop_reason: Optional[str] = None
    finalize_task: Optional[asyncio.Task] = None
    started: float = 0.0
    last_frame_ts: float = 0.0
    watchdog_task: Optional[asyncio.Task] = None
    src_rate: int = SAMPLE_RATE          # 设备实际采集率(rec_start 的 sr 字段)

    def add_frame(self, frame: proto.AudioFrame) -> None:
        if frame.seq in self.frames:  # duplicate (retry) — drop
            return
        self.frames[frame.seq] = frame.payload
        self.last_frame_ts = time.monotonic()
        if frame.is_last:
            self.got_last = True


# ---------------------------------------------------------------------------
# Bridge application
# ---------------------------------------------------------------------------

class BridgeApp:
    def __init__(self, cfg: BridgeConfig) -> None:
        self.cfg = cfg
        self.asr = WhisperASR(model_size=cfg.model_size,
                              model_path=cfg.model_path or None,
                              language=cfg.language,
                              compute_type=cfg.compute_type)
        self.injector = KimiInjector(cfg.window_title_regex,
                                     auto_enter=cfg.auto_enter,
                                     retries=cfg.inject_retries,
                                     retry_interval=cfg.inject_retry_interval,
                                     universal=(cfg.mode == "universal"))
        self.ble = KimiRemoteClient(
            BleConfig(device_name=cfg.device_name),
            on_control=self.on_control,
            on_audio=self.on_audio,
            on_connection=self.on_connection)
        # 始终创建权限监视器;是否生效由 _perm_loop 按当前模式动态决定
        # (config 或设备端 mode 消息),以便设备端从通用切回 Kimi 时即刻可用
        backend = UiaBackend(cfg.window_title_regex)
        self.perm_watcher = PermWatcher(backend,
                                        cfg.allow_pattern, cfg.deny_pattern,
                                        poll_interval=cfg.poll_interval)
        self.current_rec: Optional[Recording] = None
        self.perm_pending: Dict[int, asyncio.Future] = {}
        self.device_mode: Optional[str] = None  # 设备端覆盖:None=用 config
        self._perm_seq = 0
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._tasks: List[asyncio.Task] = []
        self._stopping = False

    def _eff_uni(self) -> bool:
        """通用(粘贴到焦点窗口)= true。设备端 mode 消息优先于 config。"""
        if self.device_mode is not None:
            return self.device_mode == "uni"
        return self.cfg.mode == "universal"

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    async def run(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._tasks = [
            asyncio.create_task(self.ble.run(), name="ble"),
        ]
        if self.perm_watcher is not None:
            self._tasks.append(
                asyncio.create_task(self._perm_loop(), name="perm"))
        if self.cfg.ping_interval > 0:
            self._tasks.append(
                asyncio.create_task(self._ping_loop(), name="ping"))
        log.info("桥接已启动(mode=%s),等待设备 %s ...",
                 self.cfg.mode, self.cfg.device_name)
        try:
            await asyncio.gather(*self._tasks)
        finally:
            await self._shutdown()

    def stop(self) -> None:
        self._stopping = True
        self.ble.stop()
        for task in self._tasks:
            task.cancel()
        if self.perm_watcher is not None:
            self.perm_watcher.stop()

    async def _shutdown(self) -> None:
        log.info("正在退出 ...")
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        if self.current_rec and self.current_rec.finalize_task:
            self.current_rec.finalize_task.cancel()
        if self.perm_watcher is not None:
            self.perm_watcher.in_flight = False
        for fut in self.perm_pending.values():
            if not fut.done():
                fut.cancel()
        log.info("已退出")

    # ------------------------------------------------------------------
    # BLE callbacks (run on the event loop)
    # ------------------------------------------------------------------

    def on_connection(self, connected: bool) -> None:
        if connected:
            log.info("设备已连接")
            self._run(self._send_hello())
        else:
            log.warning("设备已断开")
            if self.current_rec and self.current_rec.finalize_task:
                self.current_rec.finalize_task.cancel()
            self.current_rec = None
            if self.perm_watcher is not None:
                self.perm_watcher.in_flight = False
            for perm_id, fut in list(self.perm_pending.items()):
                if not fut.done():
                    fut.set_result(-1)
                del self.perm_pending[perm_id]

    def on_control(self, obj: dict) -> None:
        t = obj.get("t")
        if t == "hello_ack":
            log.info("设备握手成功,电量 %s%%", obj.get("bat", "?"))
        elif t == "pong":
            log.info("设备 pong,电量 %s%%", obj.get("bat", "?"))
        elif t == "rec_start":
            self._start_rec(int(obj.get("id", -1)),
                            int(obj.get("sr", SAMPLE_RATE)))
        elif t == "rec_stop":
            self._stop_rec(int(obj.get("id", -1)),
                           obj.get("reason", "ok"))
        elif t == "perm_resp":
            self._on_perm_resp(int(obj.get("id", -1)),
                               int(obj.get("choice", -1)))
        elif t == "key":
            log.info("按键:%s %s", obj.get("btn"), obj.get("ev"))
        elif t == "mode":
            # 设备端切换 Kimi/通用 模式,覆盖 config.ini 默认值
            self.device_mode = obj.get("m")
            self.injector.universal = self._eff_uni()
            log.info("设备切换模式:%s(权限监视%s)", self.device_mode,
                     "停用" if self._eff_uni() else "启用")
        else:
            log.warning("未知消息:%s", obj)

    def on_audio(self, frame: proto.AudioFrame) -> None:
        rec = self.current_rec
        if rec is None:
            return
        before = len(rec.frames)
        rec.add_frame(frame)
        if len(rec.frames) == before:
            return  # duplicate
        if frame.is_last and rec.finalize_task is None:
            self._schedule_finalize(rec)

    # ------------------------------------------------------------------
    # Recording -> ASR -> inject
    # ------------------------------------------------------------------

    def _start_rec(self, rec_id: int, src_rate: int = SAMPLE_RATE) -> None:
        if self.current_rec is not None and self.current_rec.rec_id == rec_id:
            return  # 设备 rec_start 重发去重
        if self.current_rec is not None:
            log.warning("录音 %d 未完成,收到新录音 %d,丢弃旧录音",
                        self.current_rec.rec_id, rec_id)
            if self.current_rec.finalize_task:
                self.current_rec.finalize_task.cancel()
            if self.current_rec.watchdog_task:
                self.current_rec.watchdog_task.cancel()
        rec = Recording(rec_id=rec_id)
        rec.started = time.monotonic()
        rec.src_rate = src_rate or SAMPLE_RATE
        self.current_rec = rec
        rec.watchdog_task = asyncio.create_task(self._rec_watchdog(rec))
        log.info("开始录音 #%d", rec_id)

    async def _rec_watchdog(self, rec: "Recording") -> None:
        # rec_stop/LAST 的 notify 可能丢包;以"音频流静默 1.5s"兜底结束录音。
        try:
            while self.current_rec is rec and rec.finalize_task is None:
                await asyncio.sleep(0.5)
                now = time.monotonic()
                if rec.frames and now - rec.last_frame_ts > 1.5:
                    log.warning("录音 #%d 音频流静默,兜底结束", rec.rec_id)
                    self._schedule_finalize(rec)
                    return
                if not rec.frames and now - rec.started > 25:
                    log.warning("录音 #%d 25 秒无音频帧,兜底结束", rec.rec_id)
                    self._schedule_finalize(rec)
                    return
        except asyncio.CancelledError:
            pass

    def _stop_rec(self, rec_id: int, reason: str) -> None:
        rec = self.current_rec
        if rec is None or rec.rec_id != rec_id:
            log.warning("录音 #%d 结束通知(%s),但无匹配会话", rec_id, reason)
            # rec_start 全丢时设备会一直等到 30s 识别超时;立即回错误,
            # 让设备直接显示失败而不是干等
            self._run(self._send_asr_err(rec_id, "pc missed rec_start"))
            return
        rec.stop_reason = reason
        log.info("录音 #%d 结束(%s)", rec_id, reason)
        if reason != "ok":
            # 取消/超时:丢弃音频,不做识别。
            self.current_rec = None
            return
        self._schedule_finalize(rec)

    def _schedule_finalize(self, rec: Recording) -> None:
        if rec.finalize_task is not None or self.current_rec is not rec:
            return
        rec.finalize_task = asyncio.create_task(self._finalize_later(rec))

    async def _finalize_later(self, rec: Recording) -> None:
        await asyncio.sleep(self.cfg.grace_ms / 1000.0)
        if self.current_rec is rec:
            self.current_rec = None
            await self._finalize(rec)

    async def _finalize(self, rec: Recording) -> None:
        if rec.watchdog_task and rec.watchdog_task is not asyncio.current_task():
            rec.watchdog_task.cancel()
        if not rec.frames:
            log.warning("录音 #%d 没有收到音频帧", rec.rec_id)
            await self._send_asr_err(rec.rec_id, "no audio frames")
            return
        ordered = [rec.frames[s] for s in sorted(rec.frames)]
        missing = (max(rec.frames) + 1) - len(rec.frames)
        if missing:
            log.warning("录音 #%d 丢帧 %d 个(seq 不连续)", rec.rec_id, missing)
        try:
            pcm = adpcm.frames_to_pcm(ordered)
            # 设备经 rec_start 的 sr 字段告知实际采样率(8k 降级时帧长不足 10ms,
            # 不能按帧长猜);缺省按 16k。
            src_rate = getattr(rec, "src_rate", SAMPLE_RATE) or SAMPLE_RATE
            if src_rate < SAMPLE_RATE:
                factor = SAMPLE_RATE // src_rate
                up = []
                for i in range(len(pcm)):
                    up.append(pcm[i])
                    for k in range(1, factor):
                        nxt = pcm[i + 1] if i + 1 < len(pcm) else pcm[i]
                        up.append(int((pcm[i] * (factor - k) + nxt * k) / factor))
                log.info("录音 #%d 按 %dHz 采集,重采样 x%d -> %dHz",
                         rec.rec_id, src_rate, factor, SAMPLE_RATE)
                pcm = up
            # 调试:音量电平 + 保存最近一份 WAV,便于判断是静音还是识别问题
            if pcm:
                rms = (sum(x * x for x in pcm) / len(pcm)) ** 0.5
                log.info("录音 #%d 音频电平 RMS=%.0f(接近0=没收到声音)", rec.rec_id, rms)
            wav_bytes = self._to_wav(pcm)
            try:
                import os, tempfile
                dbg = os.path.join(tempfile.gettempdir(), "kimi_remote_last.wav")
                with open(dbg, "wb") as fh:
                    fh.write(wav_bytes)
            except Exception:
                pass
            text = await self._loop.run_in_executor(
                None, self.asr.transcribe, wav_bytes, SAMPLE_RATE)
        except ASRError as exc:
            log.error("录音 #%d 识别失败:%s", rec.rec_id, exc)
            await self._send_asr_err(rec.rec_id, str(exc))
            return
        except Exception as exc:
            log.exception("录音 #%d 处理异常:%s", rec.rec_id, exc)
            await self._send_asr_err(rec.rec_id, str(exc))
            return
        # 识别结果先发回设备展示(用户首要诉求:设备必须能看到文字),
        # 再注入电脑;注入失败由桥接日志与电脑端表现兜底
        await self._send_asr_text(rec.rec_id, text)
        try:
            await self._loop.run_in_executor(None, self.injector.inject,
                                             text)
        except WindowNotFoundError as exc:
            log.error("注入失败:%s", exc)
            return
        except Exception as exc:
            log.exception("注入异常:%s", exc)
            return

    @staticmethod
    def _to_wav(pcm: List[int]) -> bytes:
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
        return buf.getvalue()

    # ------------------------------------------------------------------
    # Permission flow
    # ------------------------------------------------------------------

    async def _perm_loop(self) -> None:
        while not self._stopping:
            if self._eff_uni():
                # 通用模式:不监视权限弹窗
                await asyncio.sleep(max(self.cfg.poll_interval, 1.0))
                continue
            dialog = await self._loop.run_in_executor(
                None, self.perm_watcher.poll_once)
            if dialog is not None and not self.perm_watcher.in_flight:
                await self._handle_dialog(dialog)
            await asyncio.sleep(self.cfg.poll_interval)

    async def _handle_dialog(self, dialog: PermDialog) -> None:
        self._perm_seq += 1
        perm_id = self._perm_seq
        self.perm_watcher.in_flight = True
        fut = self._loop.create_future()
        self.perm_pending[perm_id] = fut
        opts = [o.label for o in dialog.options]
        log.info("检测到权限弹窗 #%d:%s 选项=%s", perm_id, dialog.text, opts)
        try:
            await self.ble.send_json({
                "t": "perm_req", "id": perm_id, "text": dialog.text,
                "opts": opts, "timeout": self.cfg.perm_timeout})
            try:
                choice = await asyncio.wait_for(fut, self.cfg.perm_timeout)
            except asyncio.TimeoutError:
                choice = -1
                log.warning("权限请求 #%d 等待设备响应超时", perm_id)
            try:
                await self._loop.run_in_executor(
                    None, click_choice, self.perm_watcher.backend,
                    dialog, choice)
                log.info("权限请求 #%d 已按选择 %d 执行", perm_id, choice)
            except Exception as exc:
                log.exception("权限请求 #%d 点击失败:%s", perm_id, exc)
        except NotConnectedError as exc:
            log.warning("权限请求 #%d 发送失败:%s", perm_id, exc)
        except Exception as exc:
            log.exception("权限请求 #%d 异常:%s", perm_id, exc)
        finally:
            self.perm_pending.pop(perm_id, None)
            self.perm_watcher.in_flight = False

    def _on_perm_resp(self, perm_id: int, choice: int) -> None:
        fut = self.perm_pending.get(perm_id)
        if fut is None or fut.done():
            return
        log.info("设备响应权限 #%d:choice=%d", perm_id, choice)
        fut.set_result(choice)

    # ------------------------------------------------------------------
    # Small helpers
    # ------------------------------------------------------------------

    def _run(self, coro) -> None:
        asyncio.create_task(coro)

    async def _send_hello(self) -> None:
        try:
            await self.ble.send_json({"t": "hello", "v": 1})
            log.info("已发送 hello")
        except Exception as exc:
            log.warning("hello 发送失败:%s", exc)

    async def _send_asr(self, payload: dict) -> None:
        # ctrl notify 可能丢包:重发 3 遍。设备端只在"识别中"接收同 id 结果,
        # 重复消息会被状态机自然忽略。
        for attempt in range(3):
            try:
                await self.ble.send_json(payload)
            except Exception as exc:
                log.warning("asr 发送失败(第 %d 次):%s", attempt + 1, exc)
            if attempt < 2:
                await asyncio.sleep(0.2)

    async def _send_asr_text(self, rec_id: int, text: str) -> None:
        log.info("录音 #%d 识别文本:%s", rec_id, text)
        await self._send_asr({"t": "asr", "id": rec_id, "text": text})

    async def _send_asr_err(self, rec_id: int, err: str) -> None:
        await self._send_asr({"t": "asr", "id": rec_id, "err": err[:200]})

    async def _ping_loop(self) -> None:
        while not self._stopping:
            await asyncio.sleep(self.cfg.ping_interval)
            try:
                await self.ble.send_json({"t": "ping"})
            except NotConnectedError:
                pass
            except Exception as exc:
                log.warning("ping 发送失败:%s", exc)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Kimi 语音编程遥控器 PC 端桥接")
    parser.add_argument("-c", "--config", help="配置文件路径(config.ini)")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO,
        stream=sys.stdout,
        format="%(asctime)s %(levelname)-7s %(name)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S")
    logging.getLogger("bleak").setLevel(logging.WARNING)

    cfg = load_config(args.config or "config.ini")
    app = BridgeApp(cfg)
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(app.run())
    except KeyboardInterrupt:
        log.info("收到 Ctrl+C")
    finally:
        app.stop()
        try:
            loop.run_until_complete(
                asyncio.gather(*asyncio.all_tasks(loop) - {asyncio.current_task(loop)},
                               return_exceptions=True))
        except Exception:
            pass
        loop.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
