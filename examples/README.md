# Examples

These examples help test the Pico CAN Bridge WebSocket CAN endpoint:

```text
ws://pico-can-bridge.local/can
```

## Python WebSocket Clients

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

## Uploaded User Page

`user-led.html` is a small custom web UI that can be uploaded to the bridge's
LittleFS user page storage.

Upload it from the built-in web UI's User Page panel, or with curl:

```sh
curl -i -X PUT \
  -H "Content-Type: text/html; charset=utf-8" \
  --data-binary @examples/user-led.html \
  http://pico-can-bridge.local/user/index.html
```

Then open:

```text
http://pico-can-bridge.local/user
```

The page connects to `/can` and sends a one-byte CAN frame:

- On: `dlc = 1`, `data = [1]`
- Off: `dlc = 1`, `data = [0]`

The target CAN ID can be entered in decimal or hex form.

## Adafruit Feather M4 CAN Test Node

`feather-m4-can-node/feather-m4-can-node.ino` is an Arduino sketch for an
[Adafruit Feather M4 CAN Express](https://www.adafruit.com/product/4759).
It is useful as a second CAN node when testing `user-led.html`.

The sketch:

- starts CAN at 500 kbit/s
- listens for incoming CAN frames
- reads the first data byte from non-RTR frames
- turns the onboard NeoPixel red when that byte is non-zero
- turns the NeoPixel off when that byte is zero
- prints received frames to the serial console at 115200 baud

The sketch currently accepts any CAN ID. This keeps the demo simple: the ID in
`user-led.html` can be left at its default or changed freely. For stricter
tests, add a `CAN.packetId()` check in the sketch.

Both boards have onboard termination, so a short CANH/CANL connection between
the Pico CAN Bridge and the Feather M4 CAN is enough for a bench test.
