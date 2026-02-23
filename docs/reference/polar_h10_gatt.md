# Polar H10 — GATT reference (UUIDs)

Status: Reference (vendor-sourced + BLE SIG)
Last updated: 2026-02-24

Polar BLE SDK version: tag **6.15.0** ([GitHub](https://github.com/polarofficial/polar-ble-sdk)).

This document is a **minimal, accurate UUID reference** for the Polar H10 features used by this project.

Sources:
- Polar BLE SDK (iOS):
  - [`BlePmdClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/pmd/BlePmdClient.swift)
  - [`BlePsFtpClient.swift`](https://github.com/polarofficial/polar-ble-sdk/blob/master/sources/iOS/ios-communications/Sources/iOSCommunications/ble/api/model/gatt/client/psftp/BlePsFtpClient.swift)
- Bluetooth SIG standard services (Heart Rate, Battery, Device Information)

## Standard BLE services (Heart Rate)

### Heart Rate Service
- Service UUID: `0x180D`
- Characteristics:
  - Heart Rate Measurement: `0x2A37` (Notify) — see [`ble_heart_rate_measurement.md`](./ble_heart_rate_measurement.md)
  - Body Sensor Location: `0x2A38` (Read)
  - Heart Rate Control Point: `0x2A39` (Write) *(usually not needed for H10 live HR)*

### Battery Service
- Service UUID: `0x180F`
- Characteristics:
  - Battery Level: `0x2A19` (Read, Notify)

### Device Information Service
- Service UUID: `0x180A`
- Common characteristics:
  - Model Number: `0x2A24`
  - Serial Number: `0x2A25`
  - Firmware Revision: `0x2A26`
  - Hardware Revision: `0x2A27`
  - Manufacturer Name: `0x2A29` (expected: `Polar Electro Oy`)

## Polar PMD (Polar Measurement Data)

From Polar BLE SDK iOS client (`BlePmdClient.swift`):

- Service UUID (128-bit): `FB005C80-02E7-F387-1CAD-8ACD2D8DF0C8`
- Characteristics:
  - Control Point: `FB005C81-02E7-F387-1CAD-8ACD2D8DF0C8` (Write + server update via Notify **or** Indicate; check characteristic properties at runtime)
  - Data: `FB005C82-02E7-F387-1CAD-8ACD2D8DF0C8` (Notify)

Used for ECG (and other sensor streams).

Implementation note:
- Polar iOS SDK enables PMD CP updates via `automaticEnableNotificationsOnConnect(chr: BlePmdClient.PMD_CP)` (CoreBluetooth `setNotifyValue`, which is used for both notify/indicate semantics), and processes CP updates uniformly in `processServiceData`.
- On H10 observed in this repo’s pico-sdk probe, PMD CP properties were `0x2A` (indicate path) and CP responses arrived as indications.
- Therefore, central implementations must not hardcode “CP = notification only”.

See: [`polar_pmd.md`](./polar_pmd.md)

## Polar PSFTP / “PFTP” (file transfer)

Polar BLE SDK names this **PSFTP** in code. The wider ecosystem often calls it **PFTP**.

From Polar BLE SDK iOS client (`BlePsFtpClient.swift`):

- Service UUID (16-bit): `FEEE`
- Characteristics (128-bit):
  - MTU: `FB005C51-02E7-F387-1CAD-8ACD2D8DF0C8` (Write, Notify)
  - D2H (device → host): `FB005C52-02E7-F387-1CAD-8ACD2D8DF0C8` (Notify)
  - H2D (host → device): `FB005C53-02E7-F387-1CAD-8ACD2D8DF0C8` (Write)

See: [`polar_psftp.md`](./polar_psftp.md)
