#!/usr/bin/env python3

import argparse
import asyncio
import json

import websockets


def parse_can_id(value: str) -> int:
    return int(value, 0)


def parse_byte(value: str) -> int:
    byte = int(value, 16)
    if byte < 0 or byte > 0xFF:
        raise argparse.ArgumentTypeError(f"byte out of range: {value}")
    return byte


def build_frame(args: argparse.Namespace) -> dict:
    data = args.data or []
    dlc = args.dlc if args.dlc is not None else len(data)

    return {
        "type": "can.tx",
        "bus": args.bus,
        "id": args.can_id,
        "ext": args.ext,
        "rtr": args.rtr,
        "dlc": dlc,
        "data": [] if args.rtr else data,
    }


async def main() -> None:
    parser = argparse.ArgumentParser(description="Send one CAN frame over Pico CAN Bridge")
    parser.add_argument("--host", default="pico-can-bridge.local", help="bridge host or IP")
    parser.add_argument("--bus", type=int, default=0, help="CAN bus number")
    parser.add_argument("--id", dest="can_id", type=parse_can_id, required=True, help="CAN ID, decimal or 0x-prefixed")
    parser.add_argument("--ext", action="store_true", help="send an extended 29-bit frame")
    parser.add_argument("--rtr", action="store_true", help="send an RTR frame")
    parser.add_argument("--dlc", type=int, help="DLC; defaults to data length")
    parser.add_argument("--data", nargs="*", type=parse_byte, help="hex data bytes, for example: 01 02 ff")
    args = parser.parse_args()

    frame = build_frame(args)
    uri = f"ws://{args.host}/can"

    async with websockets.connect(uri) as ws:
        payload = json.dumps(frame, separators=(",", ":"))
        await ws.send(payload)
        print("> " + payload)
        print("< " + await ws.recv())


if __name__ == "__main__":
    asyncio.run(main())
