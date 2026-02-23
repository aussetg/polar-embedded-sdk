# Polar PMD (Polar Measurement Data) — protocol reference

Status: Reference (vendor-sourced)
Last updated: 2026-02-24

Polar BLE SDK version: tag **6.15.0** ([GitHub](https://github.com/polarofficial/polar-ble-sdk)).

This is a **reference** for implementing **PMD** (mainly ECG) based on the official Polar BLE SDK sources.

Primary sources (Polar BLE SDK iOS):
- PMD client (incl. delta-frame decoding):
  - [`BlePmdClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/BlePmdClient.swift)
- Commands:
  - [`PmdControlPointCommand.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdControlPointCommand.swift)
- Measurement types + feature bitmap parsing:
  - [`PmdMeasurementType.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdMeasurementType.swift)
- CP response parse:
  - [`PmdControlPointResponse.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdControlPointResponse.swift)
- Data frame parse:
  - [`PmdDataFrame.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdDataFrame.swift)
- Timestamp utilities:
  - [`PmdTimeStampUtils.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/model/PmdTimeStampUtils.swift)
- ECG parse:
  - [`EcgData.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/model/EcgData.swift)
- PMD settings encoding:
  - [`PmdSetting.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdSetting.swift)

Additional sources (Polar BLE SDK Android):
- PMD settings field sizes (notably `SECURITY = 16`):
  - [`PmdSetting.kt`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/Android/android-communications/library/src/main/java/com/polar/androidcommunications/api/ble/model/gatt/client/pmd/PmdSetting.kt)

## GATT endpoints

See [`polar_h10_gatt.md`](./polar_h10_gatt.md).

### Transport note — PMD Control Point server updates

Implementation requirement (derived from vendor behavior + H10 observations):
- Do not assume PMD CP updates are notification-only.
- Select CCC mode from characteristic properties at runtime:
  - if notify supported → write notify CCC,
  - else if indicate supported → write indicate CCC.
- Parse PMD CP responses from either ATT notification or ATT indication events.

Rationale/source pointers:
- Polar iOS SDK enables PMD CP updates with `automaticEnableNotificationsOnConnect(chr: BlePmdClient.PMD_CP)` and handles CP updates in `processServiceData` without splitting by transport flavor.
- On H10 observed in this repo’s pico-sdk probe, PMD CP properties were `0x2A` and CP responses arrived as indications.

## Feature discovery (read PMD Control Point)

From Polar BLE SDK iOS:
- [`BlePmdClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/BlePmdClient.swift) (`processServiceData`)
- [`PmdMeasurementType.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/PmdMeasurementType.swift) (`fromByteArray`)

A `READ` of the PMD Control Point characteristic returns a **feature bitmap** (this is *not* a normal control point response; those start with `0xF0`).

Format:

```
Byte 0: 0x0F   (PMD_FEATURES)
Byte 1: feature bits (see bit mapping below)
Byte 2: feature bits (see bit mapping below)
```

Byte 1 bit mapping (LSB = bit 0):
- bit 0: ECG
- bit 1: PPG
- bit 2: ACC
- bit 3: PPI
- bit 5: GYRO
- bit 6: MAGNETOMETER
- bit 7: SKIN_TEMPERATURE

Byte 2 bit mapping:
- bit 1: SDK_MODE
- bit 2: LOCATION
- bit 3: PRESSURE
- bit 4: TEMPERATURE
- bit 5: OFFLINE_RECORDING
- bit 6: OFFLINE_HR

For Polar H10 we only care about **ECG** and **ACC**, both represented in `Byte 1`.

## Control point commands (client → device)

From `PmdControlPointCommand.swift`:

| Name | Value (hex) |
|---|---:|
| GET_MEASUREMENT_SETTINGS | `0x01` |
| REQUEST_MEASUREMENT_START | `0x02` |
| STOP_MEASUREMENT | `0x03` |
| GET_SDK_MODE_SETTINGS | `0x04` |
| GET_MEASUREMENT_STATUS | `0x05` |
| GET_SDK_MODE_STATUS | `0x06` |
| GET_OFFLINE_RECORDING_TRIGGER_STATUS | `0x07` |
| SET_OFFLINE_RECORDING_TRIGGER_MODE | `0x08` |
| SET_OFFLINE_RECORDING_TRIGGER_SETTINGS | `0x09` |

### Measurement types

From `PmdMeasurementType.swift`:

| Type | Value (hex) |
|---|---:|
| ECG | `0x00` |
| PPG | `0x01` |
| ACC | `0x02` |
| PPI | `0x03` |
| GYRO | `0x05` |
| MAGNETOMETER | `0x06` |
| SKIN_TEMPERATURE | `0x07` |
| SDK_MODE | `0x09` |
| LOCATION | `0x0A` |
| PRESSURE | `0x0B` |
| TEMPERATURE | `0x0C` |
| OFFLINE_RECORDING | `0x0D` |
| OFFLINE_HR | `0x0E` |

## Control point response format (device → client)

From `PmdControlPointResponse.swift`:

```
Byte 0: 0xF0   (CONTROL_POINT_RESPONSE_CODE)
Byte 1: opCode (echoed)
Byte 2: type   (measurement type)
Byte 3: error  (PmdResponseCode)
Byte 4: more   (0/1)  [only present if data.count > 4]
Byte 5..: parameters
```

`PmdResponseCode` values (selected):
- `0` success
- `6` already in state
- `8` invalid sample rate
- `10` invalid MTU

## PMD Settings encoding (used in START)

From `PmdSetting.serialize()`:

Each selected setting is encoded as:

```
[setting_type:1][count:1=0x01][value:little-endian, N bytes]
```

Field sizes (iOS `PmdSetting.mapTypeToFieldSize`, plus Android `PmdSetting.typeToFieldSize` for `security`):

| Setting | Type id | Size |
|---|---:|---:|
| sampleRate | `0x00` | 2 |
| resolution | `0x01` | 2 |
| range | `0x02` | 2 |
| rangeMilliUnit | `0x03` | 4 |
| channels | `0x04` | 1 |
| factor | `0x05` | 4 *(not sent in start; may be returned by device)* |
| security | `0x06` | 16 *(Android SDK defines this in `PmdSetting.typeToFieldSize`)* |

## START measurement packet layout

From `BlePmdClient.startMeasurement(...)`:

```
[0x02][requestByte] + settingsBytes + optionalSecretBytes
```

- `requestByte = recordingTypeBitField | measurementType`
- For normal online streaming: `recordingTypeBitField = 0x00`.

Example (ECG @ 130 Hz):

```
02 00   00 01 82 00
^  ^    ^  ^  ^  ^
|  |    |  |  |  +-- 0x0082 = 130 (LE)
|  |    |  |  +----- value
|  |    |  +-------- count = 1
|  |    +----------- setting_type = sampleRate (0)
|  +---------------- requestByte = ECG (0) | online (0)
+------------------- START
```

## PMD Data characteristic frame layout

From `PmdDataFrame.init(data: ...)`:

```
Byte 0: measurementType
Bytes 1..8: timestamp (uint64, little-endian)
Byte 9: frameTypeByte
    - bit7: 1 = compressed (delta) frame
    - bits0..6: frame type id
Bytes 10..: frame payload
```

Timestamp unit: **nanoseconds** (see `PmdTimeStampUtils.deltaFromSamplingRate`: `1/sr * 1e9`).

Timestamp epoch: **2000-01-01T00:00:00Z** (Polar epoch). See [`TimeSystemExplained.md`](https://github.com/polarofficial/polar-ble-sdk/blob/master/documentation/TimeSystemExplained.md).

Unix epoch conversion (ns):

```
unix_ts_ns = polar_ts_ns + 946684800000000000
```

## Compressed (delta) frame payload structure (overview)

From [`BlePmdClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/BlePmdClient.swift) (`Pmd.parseDeltaFramesToSamples`):

- Payload starts with a **reference sample** for each channel:
  - length = `channels * ceil(resolution_bits/8)` bytes
- Then repeats “delta blocks”:
  - `delta_bit_width` (1 byte)
  - `sample_count` (1 byte)
  - `delta_bits` packed into bytes:
    - `ceil(sample_count * delta_bit_width * channels / 8)` bytes
    - bit order is **LSB-first within each byte**
- Samples are reconstructed by cumulative sum of deltas starting from the reference sample.

## ECG payload (raw frame type 0)

From `EcgData.dataFromRawType0`:
- Only **uncompressed** frame type `0` is supported by Polar’s iOS ECG parser.
- Payload is a sequence of **signed 24-bit little-endian** samples (3 bytes each) representing **microvolts**.

Sample timestamps are derived from:
- this frame timestamp,
- previous frame timestamp (if available),
- and/or the configured sample rate.

(See `PmdTimeStampUtils.getTimeStamps` for the exact algorithm.)
