"""bleak client for the KimiRemote BLE device.

Scans by advertised name, connects, subscribes to CTRL_TX / AUDIO_TX,
reassembles fragmented control notifications, fragments outgoing writes, and
auto-reconnects with exponential backoff (capped at 30 s by default).

Callbacks:

* ``on_control(obj: dict)`` — one reassembled JSON message from the device.
* ``on_audio(frame: AudioFrame)`` — one binary audio frame notification.
* ``on_connection(connected: bool)`` — link state changes.

All callbacks are marshalled onto the asyncio event loop that runs
:meth:`KimiRemoteClient.run`.
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass
from typing import Callable, Optional, Tuple

import bleak  # noqa: F401  (re-exported for callers)
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice

import protocol as proto

log = logging.getLogger(__name__)

ControlCallback = Callable[[dict], None]
AudioCallback = Callable[[proto.AudioFrame], None]
ConnectionCallback = Callable[[bool], None]


class NotConnectedError(RuntimeError):
    """Raised when sending without an active connection."""


@dataclass
class BleConfig:
    device_name: str = "KimiRemote"
    service_uuid: str = proto.SERVICE_UUID
    ctrl_rx_uuid: str = proto.CTRL_RX_UUID
    ctrl_tx_uuid: str = proto.CTRL_TX_UUID
    audio_tx_uuid: str = proto.AUDIO_TX_UUID
    scan_timeout: float = 10.0
    base_reconnect_delay: float = 1.0
    max_reconnect_delay: float = 30.0


class KimiRemoteClient:
    def __init__(self, config: BleConfig,
                 on_control: Optional[ControlCallback] = None,
                 on_audio: Optional[AudioCallback] = None,
                 on_connection: Optional[ConnectionCallback] = None) -> None:
        self.config = config
        self.on_control = on_control
        self.on_audio = on_audio
        self.on_connection = on_connection
        self._stopping = False
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._client: Optional[BleakClient] = None
        self._disconnect_event = asyncio.Event()
        self._send_lock = asyncio.Lock()
        self._reasm = proto.Reassembler()
        self._rx_char: Optional[BleakGATTCharacteristic] = None
        self._frag_payload = proto.frag_payload_for_mtu(23)

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    async def run(self) -> None:
        """Connect (reconnecting forever) until :meth:`stop` is called."""
        self._loop = asyncio.get_running_loop()
        delay = self.config.base_reconnect_delay
        self._stopping = False
        while not self._stopping:
            try:
                device = await self._scan()
                if device is None:
                    log.warning("未找到设备 %s,%.0f 秒后重试",
                                self.config.device_name, delay)
                    await asyncio.sleep(delay)
                    delay = min(delay * 2, self.config.max_reconnect_delay)
                    continue
                await self._session(device)
                delay = self.config.base_reconnect_delay
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                log.exception("BLE 会话异常:%s", exc)
            if self._stopping:
                break
            log.warning("连接已断开,%.1f 秒后重连", delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, self.config.max_reconnect_delay)
        log.info("BLE 客户端已停止")

    def stop(self) -> None:
        self._stopping = True
        self._disconnect_event.set()

    async def send_json(self, obj: dict) -> None:
        await self.send_line(proto.encode_line(obj))

    async def send_line(self, line: bytes) -> None:
        """Fragment ``line`` and write every fragment with response."""
        client = self._client
        if client is None or not client.is_connected or self._rx_char is None:
            raise NotConnectedError("device not connected")
        frags = proto.fragment(line, self._frag_payload)
        async with self._send_lock:
            for frag in frags:
                await client.write_gatt_char(self._rx_char, frag,
                                             response=True)

    @property
    def is_connected(self) -> bool:
        return (self._client is not None
                and self._client.is_connected)

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    async def _scan(self) -> Optional[BLEDevice]:
        name = self.config.device_name

        def matches(dev: BLEDevice, ad) -> bool:
            local = getattr(ad, "local_name", None) or ""
            return local == name or (dev.name or "") == name

        log.info("扫描 BLE 设备 %s ...", name)
        device = await BleakScanner.find_device_by_filter(
            matches, timeout=self.config.scan_timeout)
        if device is None:
            return None
        log.info("发现设备:%s (rssi=%s)", device.address,
                 getattr(device, "rssi", "?"))
        return device

    _CCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"

    async def _start_notify_verified(self, client: BleakClient,
                                     char: "BleakGATTCharacteristic",
                                     cb, name: str) -> None:
        # Windows GATT 缓存陈旧时,start_notify 可能"成功"但 CCCD 写没落设备,
        # 表现为设备端收不到订阅事件、通知单向。订阅后读回 CCCD 验证。
        for attempt in range(1, 4):
            await client.start_notify(char, cb)
            try:
                for desc in char.descriptors:
                    if str(desc.uuid) == self._CCD_UUID:
                        val = await client.read_gatt_descriptor(desc.handle)
                        ok = bool(val and (val[0] & 0x03))
                        log.info("%s 通知 CCCD=%s", name,
                                 (val or b"").hex() or "empty")
                        if ok:
                            return
                        break
            except Exception as exc:
                log.warning("%s CCCD 读取失败:%s", name, exc)
            log.warning("%s 通知未生效(第 %d/3 次),重新订阅", name, attempt)
            await asyncio.sleep(0.5)
        log.error("%s 订阅 3 次仍未生效,该通道可能单向", name)

    async def _session(self, device: BLEDevice) -> None:
        self._disconnect_event.clear()
        client = BleakClient(device,
                             disconnected_callback=self._on_disconnected)
        self._client = client
        try:
            async with client:
                mtu = self._negotiated_mtu(client)
                self._frag_payload = proto.frag_payload_for_mtu(mtu)
                rx, ctrl_tx, audio_tx = self._resolve_chars(client)
                self._rx_char = rx
                await self._start_notify_verified(
                    client, ctrl_tx, self._on_ctrl_notify, "ctrl")
                await self._start_notify_verified(
                    client, audio_tx, self._on_audio_notify, "audio")
                log.info("已连接 MTU=%d 分片载荷=%d", mtu, self._frag_payload)
                self._emit(self.on_connection, True)
                await self._disconnect_event.wait()
        finally:
            self._rx_char = None
            self._emit(self.on_connection, False)
            if self._client is client:
                self._client = None

    @staticmethod
    def _negotiated_mtu(client: BleakClient) -> int:
        try:
            mtu = int(client.mtu_size)  # type: ignore[attr-defined]
        except Exception:
            mtu = 23
        return min(max(mtu, 23), 247)

    def _resolve_chars(self, client: BleakClient
                       ) -> Tuple[BleakGATTCharacteristic,
                                  BleakGATTCharacteristic,
                                  BleakGATTCharacteristic]:
        cfg = self.config
        services = client.services
        svc = services.get_service(cfg.service_uuid)
        if svc is not None:
            rx = svc.get_characteristic(cfg.ctrl_rx_uuid)
            ctx = svc.get_characteristic(cfg.ctrl_tx_uuid)
            atx = svc.get_characteristic(cfg.audio_tx_uuid)
            if rx is not None and ctx is not None and atx is not None:
                return rx, ctx, atx
            log.warning("服务 UUID 匹配但特征不全,回退启发式发现")
        return self._resolve_chars_heuristic(services)

    def _resolve_chars_heuristic(self, services
                                 ) -> Tuple[BleakGATTCharacteristic,
                                            BleakGATTCharacteristic,
                                            BleakGATTCharacteristic]:
        writables = []
        notifiers = []
        for svc in services:
            for ch in svc.characteristics:
                props = ch.properties or []
                if "write" in props and ch.uuid not in [w.uuid for w in writables]:
                    writables.append(ch)
                if "notify" in props and ch.uuid not in [n.uuid for n in notifiers]:
                    notifiers.append(ch)
        if len(writables) != 1 or len(notifiers) < 2:
            raise RuntimeError(
                f"启发式发现失败:可写特征 {len(writables)} 个,notify 特征 "
                f"{len(notifiers)} 个(期望 1 写 + ≥2 notify)")
        notifiers.sort(key=lambda c: c.handle)
        ctrl_tx = self._pick(notifiers, self.config.ctrl_tx_uuid)
        audio_tx = self._pick(notifiers, self.config.audio_tx_uuid)
        if ctrl_tx is audio_tx or ctrl_tx is None or audio_tx is None:
            ctrl_tx, audio_tx = notifiers[0], notifiers[1]
        log.info("启发式匹配特征:rx=%s ctrl_tx=%s audio_tx=%s",
                 writables[0].uuid, ctrl_tx.uuid, audio_tx.uuid)
        return writables[0], ctrl_tx, audio_tx

    @staticmethod
    def _pick(chars, uuid: str):
        for ch in chars:
            if ch.uuid.lower() == uuid.lower():
                return ch
        return None

    # ------------------------------------------------------------------
    # bleak callbacks (possibly from a non-loop thread)
    # ------------------------------------------------------------------

    def _emit(self, callback, *args) -> None:
        if callback is None:
            return
        if self._loop is not None:
            self._loop.call_soon_threadsafe(callback, *args)
        else:
            callback(*args)

    def _on_disconnected(self, client: BleakClient) -> None:
        log.warning("BLE 连接已断开")
        self._disconnect_event.set()

    def _on_ctrl_notify(self, char: BleakGATTCharacteristic,
                        data: bytearray) -> None:
        payload = bytes(data)
        try:
            message = self._reasm.feed(payload)
            if message is None:
                return
            obj = proto.decode_line(message)
        except proto.ProtocolError as exc:
            log.warning("控制通道消息无效:%s", exc)
            return
        self._emit(self.on_control, obj)

    def _on_audio_notify(self, char: BleakGATTCharacteristic,
                         data: bytearray) -> None:
        try:
            frame = proto.audio_parse(bytes(data))
        except proto.ProtocolError as exc:
            log.warning("音频帧无效:%s", exc)
            return
        self._emit(self.on_audio, frame)
