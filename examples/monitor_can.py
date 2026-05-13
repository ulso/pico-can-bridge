#!/usr/bin/env python3

import argparse
import asyncio
import json

import websockets


def format_can_id(frame: dict) -> str:
    width = 8 if frame.get("ext") else 3
    return f"0x{int(frame.get('id', 0)):0{width}X}"


def format_data(frame: dict) -> str:
    return " ".join(f"{int(byte):02X}" for byte in frame.get("data", []))


async def main() -> None:
    parser = argparse.ArgumentParser(description="Monitor CAN traffic from Pico CAN Bridge")
    parser.add_argument("--host", default="pico-can-bridge.local", help="bridge host or IP")
    parser.add_argument("--status", action="store_true", help="print periodic status responses")
    args = parser.parse_args()

    uri = f"ws://{args.host}/can"

    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "can.status"}))

        while True:
            message = await ws.recv()
            frame = json.loads(message)
            msg_type = frame.get("type")

            if msg_type == "can.rx":
                flags = []
                if frame.get("ext"):
                    flags.append("EXT")
                else:
                    flags.append("STD")
                if frame.get("rtr"):
                    flags.append("RTR")

                print(
                    f"RX {format_can_id(frame)} "
                    f"{'/'.join(flags)} "
                    f"dlc={frame.get('dlc')} "
                    f"data={format_data(frame)} "
                    f"ts={frame.get('ts')}"
                )
            elif msg_type == "can.status" and args.status:
                print(
                    "STATUS "
                    f"state={frame.get('state')} "
                    f"bitrate={frame.get('bitrate')} "
                    f"mode={frame.get('mode')} "
                    f"txErr={frame.get('txErr')} "
                    f"rxErr={frame.get('rxErr')}"
                )
            elif msg_type == "error":
                print("ERROR " + message)


if __name__ == "__main__":
    asyncio.run(main())
