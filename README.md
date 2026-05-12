# Pico CAN Bridge

Firmware for the Adafruit Feather RP2040 CAN Bus board.

Current milestone: USB CDC-NCM using Zephyr's USB device stack next, IPv4
link-local addressing, mDNS, DNS-SD, and a small HTTP service.

## Build

Run the build from the Zephyr workspace so `west build` finds Zephyr's command extensions:

```sh
cd /Users/ulf/.pico-sdk/zephyr_workspace
./venv/bin/west build --pristine \
  -b adafruit_feather_canbus_rp2040 \
  -d /Users/ulf/Documents/Projects/pico_and_zephyr/pico_can_bridge/build \
  /Users/ulf/Documents/Projects/pico_and_zephyr/pico_can_bridge
```

The UF2 ends up at:

```text
/Users/ulf/Documents/Projects/pico_and_zephyr/pico_can_bridge/build/zephyr/zephyr.uf2
```

## USB CDC-NCM

The firmware uses IPv4 link-local autoconfiguration on the CDC-NCM interface.
The host should also use link-local addressing on the new USB network
interface.

This project currently carries a local Zephyr CDC-NCM patch for macOS
alternate-setting handling:

```text
patches/zephyr-cdc-ncm-macos-altsetting.patch
```

Apply it in the Zephyr workspace before building if the workspace has been
updated or reset.

The board advertises:

```text
Hostname: pico-can-bridge.local
Service:  _http._tcp.local
Port:     80
```

After flashing, unplug and reconnect USB, then test:

```sh
ping pico-can-bridge.local
curl http://pico-can-bridge.local/
dns-sd -B _http._tcp local
dns-sd -L pico-can-bridge _http._tcp local
```

Give macOS a few seconds after reconnecting the board before starting the
DNS-SD browse.

The onboard LED blinks while CDC-NCM/link-local setup is pending and stays on
once the firmware has a link-local IPv4 address.

The root HTTP endpoint serves a small browser UI for the WebSocket endpoint.

## WebSocket

The firmware exposes the CAN WebSocket endpoint at:

```text
ws://pico-can-bridge.local/can
```

The legacy `/ws` endpoint is kept as an alias for older tests. The endpoint
accepts JSON-formatted CAN frames and replies with a JSON result.
For a quick browser-console test:

```js
const ws = new WebSocket("ws://pico-can-bridge.local/can");
ws.onmessage = (event) => console.log(event.data);
ws.onopen = () => ws.send(JSON.stringify({
  type: "can.tx",
  bus: 0,
  id: 1234,
  ext: false,
  rtr: false,
  dlc: 2,
  data: [1, 2]
}));
```
