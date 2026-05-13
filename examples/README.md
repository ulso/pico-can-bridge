# Python Examples

These examples use the WebSocket CAN endpoint:

```text
ws://pico-can-bridge.local/can
```

Install the Python dependency:

```sh
python3 -m pip install websockets
```

Send one CAN frame:

```sh
python3 examples/send_can.py --id 0x123 --data 01 02 03
```

Monitor CAN traffic:

```sh
python3 examples/monitor_can.py
```

Use `--host` if mDNS is not available:

```sh
python3 examples/monitor_can.py --host 169.254.x.y
```
