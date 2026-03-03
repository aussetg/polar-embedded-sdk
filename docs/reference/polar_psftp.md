# Polar PSFTP / "PFTP" - protocol reference

Status: Reference (vendor-sourced)
Last updated: 2026-03-03

Polar BLE SDK version: tag **6.15.0** ([GitHub](https://github.com/polarofficial/polar-ble-sdk)).

Polar's BLE SDK calls the file-transfer service **PSFTP** in code; many documents call it **PFTP**. This project treats them as synonyms.

Primary sources:
- iOS implementation:
  - [`BlePsFtpClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/psftp/BlePsFtpClient.swift)
  - [`BlePsFtpUtility.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/psftp/BlePsFtpUtility.swift)
- Android `.proto` sources:
  - [`proto/`](https://github.com/polarofficial/polar-ble-sdk/tree/master/sources/Android/android-communications/library/src/sdk/proto)

## GATT endpoints

See [`polar_h10_gatt.md`](./polar_h10_gatt.md).

## Message layering

1. **RFC60** wraps requests/queries/notifications at the message level.
2. **RFC76** fragments the RFC60 message stream into BLE-sized frames.
3. The RFC60 *request payload* is a **protobuf message** (nanopb on embedded).

## RFC60 (message wrapper)

From `BlePsFtpUtility.makeCompleteMessageStream(...)`:

### Request

A request is:

```
[ size_lo ][ size_hi (bit7=0) ][ protobuf bytes... ]
```

Where size is the protobuf payload length.

### Query

A query is:

```
[ id_lo ][ id_hi | 0x80 ][ optional parameters... ]
```

### Notification

A notification payload begins with a 1-byte notification id:

```
[ notification_id ][ optional parameters... ]
```

## RFC76 (air framing)

From `BlePsFtpUtility.buildRfc76MessageFrame(...)` and `processRfc76MessageFrame(...)`.

### Header byte layout

```
bits 0     : next (0 = first frame, 1 = continuation)
bits 1..2  : status (0 = error/response, 1 = last, 3 = more)
bit  3     : reserved (0)
bits 4..7  : sequence number (0..15)
```

In the SDK, status is parsed as:

```swift
status = (headerByte & 0x06) >> 1
```

### Header byte constants used by the SDK

When *building* frames, the SDK uses:
- `0x06 | next | (seq<<4)` for **MORE** frames
- `0x02 | next | (seq<<4)` for **LAST** frames

### Error frames

If `status == 0` and the packet length is exactly 3 bytes (`header + 2`), the SDK interprets bytes 1..2 as a **little-endian 16-bit error code**.

## BLE write/notify directions

Observed SDK behavior for request/response path:
- request frames are sent to **MTU** characteristic (`FB005C51...`),
- responses arrive via MTU notifications,
- **D2H** (`FB005C52...`) and **H2D** (`FB005C53...`) are used for PSFTP notification flow.

### Write opcode cadence in SDKs

SDK clients do not use a single opcode mode for all packets:
- iOS uses `packetChunks` (default `6`) and sends with-response periodically,
- Android uses `packetsCount` (default `5`) and similarly inserts periodic with-response writes,
- most frames are sent as write commands (without response), with periodic write requests.

For embedded parity tests, validate both endpoint and opcode cadence against the SDK behavior.

## Protobuf definitions

Requests, responses, notifications, and error codes are defined in the Android SDK `.proto` files.

See: [`polar_proto_sources.md`](./polar_proto_sources.md).
