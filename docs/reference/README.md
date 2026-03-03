# Protocol reference

These documents capture **protocol facts** needed to implement the Polar SDK core and its MicroPython module.

Rules:
- Polar-specific protocol details are sourced from the [Polar BLE SDK](https://github.com/polarofficial/polar-ble-sdk) (tag **6.15.0**).
- Standard BLE service details are sourced from Bluetooth SIG specifications.

## Index

### BLE standard services

- **Heart Rate Measurement (0x2A37) payload parsing:** [`ble_heart_rate_measurement.md`](./ble_heart_rate_measurement.md)
- **UUIDs for standard services + Polar services/chars:** [`polar_h10_gatt.md`](./polar_h10_gatt.md)

### Polar services / protocols

- **PMD (Polar Measurement Data):** [`polar_pmd.md`](./polar_pmd.md)
- **PSFTP/PFTP (file transfer):** [`polar_psftp.md`](./polar_psftp.md)
- **Which `.proto` files matter and where they live:** [`polar_proto_sources.md`](./polar_proto_sources.md)

### BTstack dependency surface

- **BTstack APIs/events/constants used by this SDK:** [`btstack_api_surface.md`](./btstack_api_surface.md)
- **How Polar features map onto BTstack protocol/events:** [`btstack_protocol_mapping.md`](./btstack_protocol_mapping.md)
- **Quick status triage table for debugging (`stats()`):** [`btstack_status_triage.md`](./btstack_status_triage.md)
