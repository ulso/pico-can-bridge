# Software Design Description

## Purpose

Pico CAN Bridge is firmware for the [Adafruit Feather RP2040 CAN Bus board](https://www.adafruit.com/product/5724).
It turns the board into a USB CDC-NCM network device with a browser-based CAN
interface and a JSON WebSocket API.

The initial target host is macOS using IPv4 link-local networking over USB.

## Hardware

- Board: [Adafruit Feather RP2040 CAN Bus](https://www.adafruit.com/product/5724)
- MCU: RP2040
- CAN controller: MCP2515 over SPI
- CAN transceiver and onboard 120 ohm termination are provided by the board
- Status LED: onboard `led0`
- USB: full-speed device, exposed as CDC-NCM

## Major Components

### USB CDC-NCM

USB is initialized through Zephyr's USB device stack next. The firmware provides
a CDC-NCM class descriptor and starts the network interface when the USB stack
reports device configuration.

macOS sometimes toggles the CDC-NCM data interface from alternate setting 1 back
to alternate setting 0 during enumeration. If this happens, no Ethernet packets
flow and IPv4 link-local autoconfiguration never completes. The firmware uses a
startup watchdog to recover from this state.

### Network Setup

The network interface uses IPv4 link-local autoconfiguration. When an
autoconfigured address is assigned, the firmware:

- marks the network as ready
- turns the status LED solid on
- starts the HTTP server
- allows mDNS/DNS-SD announcements to become visible to the host

The hostname is:

```text
pico-can-bridge.local
```

### HTTP Server

The HTTP server listens on port 80 and serves the embedded web UI at `/`.
The server is intentionally started only after link-local IPv4 is ready. This
keeps early USB enumeration quieter and prevents the UI from being available
before the network path is usable.

### WebSocket Server

The WebSocket endpoint is:

```text
/can
```

The legacy endpoint `/ws` is kept as an alias. The implementation accepts one
active WebSocket client at a time.

The WebSocket loop:

- forwards queued CAN RX frames to the client as JSON
- accepts JSON commands from the client
- sends JSON replies for TX, status, config, and errors
- handles close and ping frames

### CAN Bridge

CAN is implemented with Zephyr's CAN API and the MCP2515 driver. The firmware
uses:

- one TX message queue
- one RX message queue
- a CAN TX worker thread
- a catch-all RX filter
- a mutex around controller reconfiguration

Transmit requests are queued from the WebSocket handler and sent by the CAN TX
thread. Received frames are queued from the CAN RX callback and drained by the
WebSocket loop.

### Status LED

The LED state is:

- blinking: USB/NCM/link-local setup is pending
- solid on: link-local IPv4 is ready
- fast blink: error or recovery path

## Startup Flow

1. Initialize USB descriptors and enable USB.
2. Register USB and network callbacks.
3. Initialize CAN in normal mode at the default bitrate.
4. Wait for USB configuration.
5. Bring the CDC-NCM network interface up.
6. Start a CDC-NCM startup watchdog.
7. Wait for IPv4 link-local address assignment.
8. Cancel the startup watchdog.
9. Start the HTTP server.
10. Announce `_http._tcp.local` via mDNS/DNS-SD.

## Recovery Behavior

If USB configuration is seen but no link-local address is assigned within the
startup timeout, the firmware assumes the CDC-NCM data interface is stuck in a
non-working state and performs a cold reboot.

This is a pragmatic workaround for the observed macOS/RP2040 enumeration race.
It prevents the board from remaining indefinitely in a blinking, unreachable
state.

The local Zephyr CDC-NCM patch also prevents notification retry spam while USB
is suspended and keeps network carrier state tied to the CDC-NCM data alternate
setting.

## Threading Model

- main thread: initialization and idle loop
- heartbeat thread: status LED
- system workqueue: network up/down work, HTTP start work, startup watchdog
- HTTP server thread: incoming HTTP/WebSocket upgrade handling
- WebSocket thread: active CAN WebSocket connection
- mDNS announce thread: service announcements
- CAN TX thread: queued CAN frame transmission

## Constraints

- USB is full-speed only.
- Only CAN bus 0 is currently supported.
- Only one WebSocket client can be active at a time.
- WebSocket JSON payloads are limited to 255 bytes.
- Classical CAN frames are supported; CAN FD is not currently exposed.
