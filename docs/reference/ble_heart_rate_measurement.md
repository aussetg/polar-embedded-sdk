# BLE Heart Rate Measurement (0x2A37) — payload reference

Status: Reference (BLE SIG)
Last updated: 2026-02-24

This document describes the payload format of the **Heart Rate Measurement** characteristic (`0x2A37`) used by the standard BLE **Heart Rate Service** (`0x180D`).

Primary source:
- Bluetooth SIG: *Heart Rate Service 1.0* (Heart Rate Measurement characteristic)
  - https://www.bluetooth.com/specifications/specs/heart-rate-service-1-0/

## Frame layout

All fields are little-endian.

```
Byte 0: Flags
Byte 1..: Heart Rate Measurement Value (8-bit or 16-bit, depending on flags)
Optional: Energy Expended (uint16)
Optional: RR-Interval(s) (one or more uint16)
```

### Flags (byte 0)

Bit meaning (from the spec):

- **bit 0**: Heart Rate Value Format
  - 0 = uint8
  - 1 = uint16
- **bit 1**: Sensor Contact Status
  - only valid if bit 2 is set
  - 0 = no contact detected
  - 1 = contact detected
- **bit 2**: Sensor Contact Supported
  - 0 = not supported
  - 1 = supported
- **bit 3**: Energy Expended field present
- **bit 4**: RR-Interval field(s) present
- **bits 5..7**: reserved

## RR-interval units

RR-interval values are transmitted as `uint16` with units of **1/1024 seconds**.

Common conversion to milliseconds:

```
rr_ms = rr_raw * 1000 / 1024
```

If you need integer milliseconds (as in this repo’s current `polar.H10.read_hr()` API), a typical approach is:

```
rr_ms_int = round(rr_raw * 1000 / 1024)
```

## Parsing (pseudocode)

```
flags = data[0]
idx = 1

hr_is_u16 = (flags & 0x01) != 0
contact_supported = (flags & 0x04) != 0
contact_detected = (flags & 0x02) != 0 if contact_supported else False
energy_present = (flags & 0x08) != 0
rr_present = (flags & 0x10) != 0

if hr_is_u16:
    hr = u16le(data[idx:idx+2]); idx += 2
else:
    hr = data[idx]; idx += 1

if energy_present:
    energy = u16le(data[idx:idx+2]); idx += 2

rr_list = []
if rr_present:
    while idx + 2 <= len(data):
        rr_raw = u16le(data[idx:idx+2]); idx += 2
        rr_ms = rr_raw * 1000 / 1024
        rr_list.append(rr_ms)
```

## Notes for Polar H10

- Polar H10 typically includes **RR intervals** (bit 4 set), but you must handle frames where RR intervals are absent.
- Sensor contact bits are device-dependent; implement the spec behavior:
  - if contact is not supported (bit 2 not set), report “unknown/not supported” consistently (this repo currently uses `contact=0`).
