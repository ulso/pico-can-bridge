# Pico CAN Bridge

Firmware for the [Adafruit Feather RP2040 CAN Bus board](https://www.adafruit.com/product/5724).
The board appears as a USB CDC-NCM network interface and exposes a small web UI
plus a JSON WebSocket API for CAN traffic.

Current features:

- USB CDC-NCM using Zephyr's USB device stack next
- IPv4 link-local addressing
- mDNS hostname and DNS-SD HTTP service announcement
- HTTP web UI served from the firmware
- WebSocket CAN bridge at `/can`
- JSON CAN TX/RX/status/config protocol
- MCP2515 CAN controller in normal, loopback, or listen-only mode
- Startup recovery for a macOS/RP2040 CDC-NCM alternate-setting race

## Build

Run the build from the Zephyr workspace so `west build` finds Zephyr's command extensions.
Replace `<zephyr-workspace>` and `<repo>` with your local paths:

```sh
cd <zephyr-workspace>
./venv/bin/west build --pristine \
  -b adafruit_feather_canbus_rp2040 \
  -d <repo>/build \
  <repo>
```

The UF2 ends up at:

```text
<repo>/build/zephyr/zephyr.uf2
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

```sh
cd <zephyr-workspace>/zephyr-main
git apply <repo>/patches/zephyr-cdc-ncm-macos-altsetting.patch
```

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

## Web UI

Open:

```text
http://pico-can-bridge.local/
```

The UI can connect to the CAN WebSocket, transmit frames, display received
frames, show CAN status, change bitrate/mode, and copy the current transmit
frame as JSON for scripts.

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

See [docs/protocol.md](docs/protocol.md) for the JSON protocol.

Python client examples are available in [examples/](examples/).

## Documentation

- [docs/sdd.md](docs/sdd.md): software design description
- [docs/protocol.md](docs/protocol.md): WebSocket JSON protocol
- [docs/known-issues.md](docs/known-issues.md): known USB/macOS notes and local Zephyr patch

## License

Licensed under either of:

- [Apache License, Version 2.0](LICENSE-APACHE)
- [MIT license](LICENSE-MIT)

at your option.

## Development

This project was developed in close collaboration with OpenAI Codex, used for
firmware implementation, debugging support, documentation, and iteration on the
built-in web UI.
