# MicroPython local patches

These patches target the `vendors/micropython` submodule.

### Preferred series (git format-patch)

Order:
1. `0001-extmod-bluetooth-add-dedicated-disconnect-reason-IRQ.patch`
2. `0002-extmod-btstack-preserve-disconnect-addr-fields-and-e.patch`
3. `0003-extmod-btstack-tune-central-connect-latency-and-supe.patch`
4. `0004-extmod-btstack-central-post-connect-HCI-param-update.patch`
5. `0005-extmod-btstack-include-hci-event-sources-for-newer-btstack.patch`
6. `0006-ports-rp2-btstack-run-all-pending-run-loop-work.patch`
7. `0007-ports-rp2-reset-hci-scheduler-state-across-soft-reset.patch`
8. `0008-ports-rp2-power-cycle-cyw43-on-soft-reset.patch`
9. `0009-extmod-btstack-enable-tlv-backed-bond-storage-on-rp2-cyw43.patch`
10. `0010-extmod-btstack-add-embedded-platform-includes-for-tlv.patch`
11. `0011-extmod-btstack-expose-link-security-debug-snapshot.patch`
12. `0012-ports-rp2-reserve-top-flash-for-btstack-tlv-bank.patch`
13. `0013-ports-rp2-override-pico-flash-safety-for-dormant-core1.patch`
14. `0014-extmod-btstack-remove-link-security-debug-snapshot.patch`

Apply (recommended, preserves commit metadata):

```bash
./patches/apply_micropython_patches.sh
```

Equivalent manual command:

```bash
git -C vendors/micropython am ../../patches/micropython/*.patch
```

Abort if needed:

```bash
git -C vendors/micropython am --abort
```

Undo after apply (drop last 14 commits):

```bash
git -C vendors/micropython reset --hard HEAD~14
```

## What they do

- Add a dedicated Bluetooth IRQ for disconnect reason:
  - `_IRQ_DISCONNECT_REASON = const(32)`
  - payload: `(disconnect_event, conn_handle, reason)`
- Emit this event from NimBLE and BTstack backends.
- Improve BTstack disconnect event payload to preserve peer address + addr_type when available.
- Add optional compile-time debug mode for BTstack to overload `addr_type` with reason:
  - `MICROPY_PY_BLUETOOTH_BTSTACK_DEBUG_DISCONNECT_REASON_IN_ADDR_TYPE`
- Tune BTstack central connect defaults:
  - `conn_latency` default from `4` to `0`
  - supervision timeout decoupled from short connect scan duration defaults
  - configurable macros for timeout/latency defaults
- Add optional post-connect central parameter update in BTstack and enable it on rp2:
  - request 30 ms interval (`24` units), latency `0`, supervision timeout `72` (10 ms units)
  - use `gap_update_connection_parameters(...)` (HCI/LL path) rather than L2CAP CPUP request
- Include required HCI event sources in MicroPython BTstack integration:
  - `src/hci_event.c` (always)
  - `src/hci_event_builder.c` when present
  (required by newer BTstack versions, including 1.8)
- Fix the rp2 BTstack run loop to match BTstack embedded semantics better:
  - call `btstack_run_loop_base_init()`
  - execute deferred BTstack callbacks on each scheduled poll
  - implement `execute_on_main_thread` by queueing the callback and scheduling an immediate poll
- Reset rp2 HCI scheduler state across MicroPython soft resets:
  - clear the static HCI scheduler node on init
  - remove the outstanding HCI soft timer on deinit
  - invoke the HCI deinit hook from BTstack rp2 shutdown
- Power-cycle the CYW43 device on rp2 soft reset after Bluetooth deinit,
  so the next interpreter instance starts from a clean shared-bus BT state
- Enable TLV-backed LE bond storage on rp2 CYW43 BTstack builds so pairing
  state survives MicroPython soft resets
- Add BTstack embedded platform include paths needed by the TLV flash-bank
  backend on rp2
- Expose a small BTstack link-security debug snapshot from the MicroPython
  BTstack backend so project-side diagnostics can inspect SM engine state,
  IRK lookup state, LE device-db index, and current encryption metadata
- Reserve the rp2 top-of-flash BTstack TLV bank area from the MicroPython
  filesystem region so persisted LE bond data can use the default Pico BTstack
  flash-bank location without overlapping VFS sectors
- Override pico-sdk flash-safe execution on rp2 to match MicroPython's dynamic
  core1 usage: use multicore lockout only when core1 is actually running, and
  otherwise allow safe flash operations with IRQ disable only so BTstack TLV
  flash writes are not rejected while the second core is dormant
- Remove the temporary BTstack link-security debug snapshot hook once bond
  persistence debugging is complete

Note: the `hids_client` -> `hids_host` rename for MicroPython’s vendored
`lib/pico-sdk` is handled by the pico-sdk patch stack, applied with:

```bash
./patches/apply_pico_sdk_patches.sh --target vendors/micropython/lib/pico-sdk
```
