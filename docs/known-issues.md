# Known Issues

## macOS CDC-NCM Alternate Setting Race

On macOS, the RP2040 CDC-NCM interface sometimes enumerates like this:

```text
cdc_ncm: New configuration, interface 1 alternate 1
udc_rpi_pico: Canceled ep 0x01 transaction
cdc_ncm: New configuration, interface 1 alternate 0
```

When the final state is alternate setting 0, the CDC-NCM data endpoints are not
active. IPv4 link-local autoconfiguration then never completes.

The firmware has a startup watchdog for this case. If USB is configured but no
link-local address appears within the timeout, the firmware performs a cold
reboot and tries enumeration again.

This is a workaround for the observed host/device timing behavior, not a claim
that the root cause is fully fixed.

## Local Zephyr CDC-NCM Patch

The project relies on a local Zephyr patch:

```text
patches/zephyr-cdc-ncm-macos-altsetting.patch
```

The patch adjusts CDC-NCM carrier and notification behavior:

- carrier is kept off until CDC-NCM notification setup succeeds
- alternate setting 0 cancels notification work
- suspend cancels notification work
- notification work does not retry while the USB class is suspended
- resume restarts notification work only when the data interface and net-if are up

This avoids repeated log spam such as:

```text
cdc_ncm: USB device is suspended (FIXME)
```

when the board is powered from battery and the USB cable is unplugged.

If the Zephyr workspace is reset or updated, reapply the patch before building.

## Sleep/Wake

macOS sleep/wake can suspend the USB device while the firmware remains powered.
The firmware handles resume/VBUS-ready messages by restarting CDC-NCM network
state. This fixed the observed case where the host woke from sleep and the board
remained powered but unreachable.

## DNS-SD Timing

mDNS/DNS-SD announcements may take a few seconds to become visible on macOS.
The firmware sends faster initial announcements, then falls back to a slower
periodic announcement.

## WebSocket Limitations

- Only one active WebSocket client is supported.
- Payloads larger than the firmware WebSocket buffer are rejected.
- The server supports text, close, and ping/pong frames. Fragmentation is not a
  primary design goal.

## CAN Limitations

- Only bus 0 is supported.
- Classical CAN frames up to 8 data bytes are supported.
- CAN FD is not exposed.
- TX responses confirm queueing, not physical bus acknowledgement by another
  node.
