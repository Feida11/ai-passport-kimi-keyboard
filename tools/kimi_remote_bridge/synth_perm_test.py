#!/usr/bin/env python
"""合成权限请求验证:模拟桥接向设备发 perm_req,收 perm_resp。

用法: python synth_perm_test.py
设备需处于语音键盘模式;请在脚本运行后于设备上按键确认。
"""
import asyncio
import sys

import bleak

sys.path.insert(0, ".")
import protocol as proto

DEVICE_NAME = "KimiRemote"


async def main() -> None:
    dev = await bleak.BleakScanner.find_device_by_name(DEVICE_NAME, timeout=20)
    if dev is None:
        print("DEVICE NOT FOUND")
        return
    print("connected to", dev.address)

    lines = []
    done = asyncio.Event()

    def on_ctrl(_, data: bytearray) -> None:
        lines.append(bytes(data))
        merged = proto.reassemble(bytes(b"".join(lines))) if hasattr(proto, "reassemble") else None
        # 简化:按片前缀重组
        buf = getattr(on_ctrl, "buf", b"")
        flag = data[0]
        buf += bytes(data[1:])
        on_ctrl.buf = b"" if flag == 0 else buf
        if flag == 0:
            line = buf.decode("utf-8", errors="replace")
            print("CTRL <<", line)
            try:
                import json
                msg = json.loads(line)
                if msg.get("t") == "perm_resp":
                    print("PERM_RESP id=%s choice=%s" % (msg.get("id"), msg.get("choice")))
                    done.set()
            except Exception:
                pass

    async with bleak.BleakClient(dev) as client:
        services = client.services
        svc = services.get_service(proto.SERVICE_UUID)
        rx = svc.get_characteristic(proto.CTRL_RX_UUID)
        ctx = svc.get_characteristic(proto.CTRL_TX_UUID)
        await client.start_notify(ctx, on_ctrl)

        async def send_json(obj: dict) -> None:
            payload = (json_dumps(obj)).encode("utf-8")
            # 分片发送
            mtu_payload = 200
            if len(payload) <= mtu_payload:
                await client.write_gatt_char(rx, b"\x00" + payload, response=True)
                return
            for off in range(0, len(payload), mtu_payload):
                chunk = payload[off:off + mtu_payload]
                last = off + mtu_payload >= len(payload)
                await client.write_gatt_char(
                    rx, (b"\x00" if last else b"\x01") + chunk, response=True)

        await send_json({"t": "hello", "v": 1})
        await asyncio.sleep(1)
        await send_json({
            "t": "perm_req",
            "id": 99,
            "text": "测试:是否允许执行命令 rm -rf /tmp/test?(合成请求,验证按键流程)",
            "opts": ["允许一次", "总是允许", "拒绝"],
            "timeout": 90,
        })
        print("PERM_REQ sent id=99; 请在设备上按键(OK=确认/上键=切换/下键=取消)")
        try:
            await asyncio.wait_for(done.wait(), timeout=90)
        except asyncio.TimeoutError:
            print("TIMEOUT waiting perm_resp")


def json_dumps(obj: dict) -> str:
    import json
    return json.dumps(obj, ensure_ascii=False)


if __name__ == "__main__":
    asyncio.run(main())
