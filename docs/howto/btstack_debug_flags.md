# How-to — BTstack debug/config flags (practical shortlist)

Status: How-to (debugging)
Last updated: 2026-03-03

This guide is a **practical companion** to BTstack’s `how_to` manual section.

Use it when debugging:
- intermittent disconnects,
- missing/late notifications,
- pairing/authorization surprises,
- memory/resource-related behavior changes.

Primary upstream reference:
- <https://bluekitchen-gmbh.com/btstack/how_to/>

Important note:
- BTstack web docs are for current `master` (not versioned per release).
- Always interpret flag docs with our pinned version policy in [`btstack_version_alignment.md`](./btstack_version_alignment.md).

## Where BTstack config comes from in this repo

### pico-sdk probes (safe to edit directly)

- `examples/pico_sdk/btstack_config.h`
- `examples/pico_sdk_psftp/btstack_config.h`

### MicroPython rp2 build (vendor-managed)

Effective include chain:
- `vendors/micropython/extmod/btstack/btstack_config.h`
- `vendors/micropython/ports/rp2/btstack_inc/btstack_config.h`
- `vendors/micropython/extmod/btstack/btstack_config_common.h`

Repository rule reminder:
- Do **not** edit `vendors/` directly.
- If MicroPython-side BTstack config changes are needed, carry them via `patches/micropython/`.

## Flag shortlist that matters most for this project

## 1) Logging/visibility

- `ENABLE_LOG_INFO`
- `ENABLE_LOG_ERROR`
- `ENABLE_LOG_DEBUG` *(noisy; use temporarily)*
- `ENABLE_PRINTF_HEXDUMP`

Why useful:
- first pass for timing/state regressions,
- validating ordering of connect/discovery/pairing/CCC writes,
- quick payload sanity checks in probes.

## 2) Controller→host flow control and ACL buffering (CYW43-sensitive)

- `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL`
- `HCI_HOST_ACL_PACKET_LEN`
- `HCI_HOST_ACL_PACKET_NUM`
- `HCI_ACL_PAYLOAD_SIZE`
- `MAX_NR_CONTROLLER_ACL_BUFFERS`

Why useful:
- directly impacts burst handling and backpressure behavior,
- particularly relevant on shared-bus CYW43 setups (Pico W/Pico 2 W).

## 3) Connection/client resource pool sizing

- `MAX_NR_HCI_CONNECTIONS`
- `MAX_NR_GATT_CLIENTS`
- `MAX_NR_L2CAP_CHANNELS`
- `MAX_NR_L2CAP_SERVICES`
- `MAX_NR_SM_LOOKUP_ENTRIES`
- `MAX_NR_WHITELIST_ENTRIES`
- `MAX_NR_LE_DEVICE_DB_ENTRIES`

Why useful:
- explains failures that look random but are allocation/slot exhaustion,
- useful when adding concurrent features or more complex retry flows.

## 4) Security and pairing behavior

- `ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS`
- `ENABLE_LE_SECURE_CONNECTIONS`
- `ENABLE_GATT_CLIENT_PAIRING` *(optional behavior knob in BTstack docs)*

Why useful:
- affects whether/when pairing is triggered and retried,
- relevant for PMD/PSFTP operations that can fail with ATT security errors.

## 5) ATT/GATT-side limits and defaults

- `MAX_ATT_DB_SIZE` *(mainly server-side footprint control, but appears in shared configs)*
- `HCI_RESET_RESEND_TIMEOUT_MS` *(controller init resilience)*

Why useful:
- helps classify startup/reinit weirdness vs runtime link issues.

## Debug profile presets (ready-to-toggle)

Use these as temporary investigation presets (not default shipping config).

### Preset A — "trace-lite"

Enable:
- `ENABLE_LOG_INFO`
- `ENABLE_LOG_ERROR`
- `ENABLE_PRINTF_HEXDUMP`

Keep `ENABLE_LOG_DEBUG` disabled.

Expected side effects:
- moderate extra logging overhead,
- usually acceptable for short/medium runs,
- better event sequencing visibility.

### Preset B — "deep-trace"

Enable Preset A +:
- `ENABLE_LOG_DEBUG`

Expected side effects:
- high log volume and host overhead,
- can perturb timing-sensitive behavior,
- use only for short reproductions.

### Preset C — "conservative-burst"

Use Preset A and make flow-control/pool changes explicit (one variable at a time):
- `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL`
- `HCI_HOST_ACL_PACKET_LEN`
- `HCI_HOST_ACL_PACKET_NUM`
- `MAX_NR_CONTROLLER_ACL_BUFFERS`

Expected side effects:
- lower peak throughput in some cases,
- improved resilience under bursty traffic on CYW43 shared-bus systems.

## Suggested debug workflow

1. Reproduce on `examples/pico_sdk` or `examples/pico_sdk_psftp` first.
2. Capture baseline config diff:
   - `python scripts/diff_btstack_config.py`
3. Apply one preset (or one flag group) at a time.
4. Record:
   - disconnect reason/status,
   - ATT status from query-complete,
   - pairing status/reason (and completion counts where available),
   - success/failure rate across N attempts.
5. Port stable findings to MicroPython via patch stack (not direct vendor edits).
6. Re-run both validation paths:
   - pico-sdk probe,
   - MicroPython module validation.

## Related docs in this repo

- BTstack API usage surface: [`../reference/btstack_api_surface.md`](../reference/btstack_api_surface.md)
- Polar/BTstack protocol mapping: [`../reference/btstack_protocol_mapping.md`](../reference/btstack_protocol_mapping.md)
- Status triage table: [`../reference/btstack_status_triage.md`](../reference/btstack_status_triage.md)
- BTstack alignment/version policy: [`btstack_version_alignment.md`](./btstack_version_alignment.md)
- BTstack change checklist: [`btstack_change_checklist.md`](./btstack_change_checklist.md)
- Validation workflow: [`validation.md`](./validation.md)
