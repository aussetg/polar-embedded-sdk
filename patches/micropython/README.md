# MicroPython local patches

These patches target the `vendors/micropython` submodule.

### Preferred series (git format-patch)

Order:
1. `0001-extmod-bluetooth-add-dedicated-disconnect-reason-IRQ.patch`
2. `0002-extmod-btstack-preserve-disconnect-addr-fields-and-e.patch`
3. `0003-extmod-btstack-tune-central-connect-latency-and-supe.patch`
4. `0004-extmod-btstack-central-post-connect-HCI-param-update.patch`

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

Undo after apply (drop last 4 commits):

```bash
git -C vendors/micropython reset --hard HEAD~4
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
