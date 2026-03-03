# Glossary

Acronyms and terms used throughout this repository.

## BLE / protocol

- **ATT**: Attribute Protocol. The low-level protocol used to read/write GATT attributes.
- **GATT**: Generic Attribute Profile. Organises services/characteristics/descriptors.
- **CCC / CCCD**: Client Characteristic Configuration (Descriptor). Written by the client to enable notifications/indications.
- **Notify / Indicate**: Two ways a peripheral can push characteristic value updates.
- **MTU**: Maximum Transmission Unit. Affects how much attribute payload can be transferred per ATT packet.
- **HCI**: Host Controller Interface. Transport between host stack and controller.
- **SM**: Security Manager (pairing/bonding/encryption).

## Polar-specific

- **H10**: Polar H10 chest strap.
- **PMD**: Polar Measurement Data service (streaming sensor data such as ECG/ACC/PPG).
- **PSFTP / PFTP**: Polar’s file transfer protocol used over BLE (SDK calls it PSFTP; “PFTP” is also used in docs).
- **RFC60**: Message wrapper used by PSFTP (request/query/notification framing).
- **RFC76**: Fragmentation framing used by PSFTP to split RFC60 streams into BLE-sized packets.

## Measurements

- **ECG**: Electrocardiogram.
- **ACC**: Accelerometer.
- **PPG**: Photoplethysmography.
- **PPI**: Peak-to-Peak Interval.
- **RR interval**: Time between heartbeats as transmitted in the standard BLE Heart Rate Measurement characteristic.

## This repo / platform

- **rp2**: MicroPython port for Raspberry Pi Pico / RP2xxx targets.
- **Pico 2 W / RP2350**: Target microcontroller + CYW43 Wi-Fi/BLE combo.
- **CYW43**: Broadcom/Cypress radio used on Pico W / Pico 2 W.
- **BTstack**: BLE stack used by MicroPython on rp2.
- **SDK core** (a.k.a. “portable core”): `polar_sdk/core/` code that contains no MicroPython-specific types. This is a **Polar BLE SDK reimplementation layer** (Polar-specific, BTstack-backed), not a generic BLE driver.
- **MicroPython module / binding layer**: `polar_sdk/mpy/` MicroPython C module glue that exposes the user-facing `polar_sdk.H10` API.
