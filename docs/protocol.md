# CAN WebSocket JSON Protocol

The CAN WebSocket endpoint is:

```text
ws://pico-can-bridge.local/can
```

The legacy `/ws` endpoint is kept as an alias for older tests.

All messages are UTF-8 JSON text frames. Numeric CAN IDs and bytes are encoded
as JSON numbers. Data bytes are decimal numbers in the JSON representation,
although the web UI accepts hex input for convenience.

## Transmit CAN Frame

Request:

```json
{
  "type": "can.tx",
  "bus": 0,
  "id": 1234,
  "ext": false,
  "rtr": false,
  "dlc": 2,
  "data": [1, 2]
}
```

The `type` field may be omitted for simple frame transmission, but new clients
should include `"type": "can.tx"`.

Response:

```json
{
  "type": "can.tx",
  "ok": true,
  "bus": 0,
  "id": 1234,
  "ext": false,
  "rtr": false,
  "dlc": 2,
  "data": [1, 2],
  "queued": true,
  "ts": 12345678
}
```

The response means the frame was accepted into the firmware TX queue. It does
not guarantee that another CAN node acknowledged the frame on the bus.

## Received CAN Frame

Frames received from the CAN bus are sent asynchronously:

```json
{
  "type": "can.rx",
  "bus": 0,
  "id": 1234,
  "ext": false,
  "rtr": false,
  "dlc": 2,
  "data": [1, 2],
  "ts": 12345678
}
```

## Status

Request:

```json
{
  "type": "can.status"
}
```

`can.config.get` is accepted as an alias.

Response:

```json
{
  "type": "can.status",
  "ok": true,
  "bus": 0,
  "ready": true,
  "state": "error-active",
  "bitrate": 500000,
  "mode": "normal",
  "txErr": 0,
  "rxErr": 0,
  "txQueueUsed": 0,
  "txQueueFree": 8,
  "rxQueueUsed": 0,
  "rxQueueFree": 8
}
```

CAN state names follow Zephyr/CAN nomenclature:

- `error-active`
- `error-warning`
- `error-passive`
- `bus-off`
- `stopped`
- `unknown`

`error-active` is the normal healthy CAN state.

## Configure CAN

Request:

```json
{
  "type": "can.config.set",
  "bitrate": 500000,
  "mode": "normal"
}
```

Supported modes:

- `normal`
- `loopback`
- `listen-only`

The response is a `can.status` object with the applied settings.

## Error Response

Errors are returned as JSON:

```json
{
  "type": "error",
  "ok": false,
  "code": "invalid_dlc",
  "err": -22,
  "message": "CAN dlc must be between 0 and 8"
}
```

Common error codes include:

- `missing_field`
- `unsupported_bus`
- `invalid_dlc`
- `invalid_id`
- `missing_data`
- `data_length_mismatch`
- `rtr_has_data`
- `invalid_data`
- `invalid_bitrate`
- `invalid_mode`
- `can_not_ready`
- `can_tx_failed`
- `can_config_failed`
- `payload_too_large`
- `invalid_json`
- `unsupported_type`

## Field Rules

- `bus` must be `0`.
- `dlc` must be between 0 and 8.
- Standard IDs must fit in 11 bits.
- Extended IDs must fit in 29 bits.
- Non-RTR frames with `dlc > 0` must include `data`.
- For non-RTR frames, `data.length` must match `dlc`.
- RTR frames must not include data bytes.
- Data bytes must be 0-255.
