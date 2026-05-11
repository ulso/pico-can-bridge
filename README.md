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
