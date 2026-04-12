# BTstack status triage for Polar SDK debugging

Status: Reference
Last updated: 2026-03-03

This is a quick triage table for statuses/counters exposed by `h10.stats()`.

It focuses on values we repeatedly see in this project and what to do first.

## Where these appear

Common `stats()` fields:
- `last_hci_status`
- `last_att_status`
- `last_disconnect_reason`
- `conn_encryption_key_size`
- `conn_bonded`
- `sm_last_pairing_status`
- `sm_last_pairing_reason`
- per-op ATT fields (`pmd_cfg_att_status`, `pmd_write_att_status`, `psftp_cfg_att_status`, `psftp_write_att_status`, `psftp_last_att_status`)

## Quick triage table

| Signal | Typical meaning | First actions |
|---|---|---|
| `last_disconnect_reason = 0x08` | Link supervision timeout (stalled link progression) | Reduce host-side load, avoid WiFi during BLE soak, inspect connection params and CYW43 contention; see `KNOWN_ISSUES.md` KI-01 and `howto/pico2w_ble_stability.md`. |
| `last_disconnect_reason = 0x3e` | LE connection failed / connection establishment issue | Check scan/connect timing, target visibility, RSSI environment, retry behavior. |
| `pmd_*_att_status = 0x05` | ATT insufficient authentication | Trigger/verify pairing flow, wait for encryption, retry operation. |
| `pmd_*_att_status = 0x08` | ATT insufficient authorization | Same as above: pairing/authorization/encryption path + retry. |
| `psftp_*_att_status = 0x05` or `0x08` | PSFTP op blocked by security state | Ensure SM policy applied, pairing completed, and connection is encrypted before retry. |
| `conn_encryption_key_size = 0` while protected ops fail | Link not encrypted | Use `polar_sdk_btstack_security_request_pairing(...)` or `polar_sdk_btstack_security_ensure(...)`, then retry PMD/PSFTP writes/CCC after the link is secure. |
| `conn_bonded = false` after repeated attempts | Bond not established/reused | Clear stale bond both sides, re-pair, then repeat operation. |
| `sm_last_pairing_status != 0` | Pairing failed | Use `sm_last_pairing_reason`, clear stale bond if needed, retry from clean session. |
| `sm_last_pairing_status = 19` with `conn_encryption_key_size=0` and PSFTP timeouts | Observed sticky pairing-failed session signature in current MicroPython path | Reset board/session and retry from clean state; see KI-04. |
| `last_hci_status != 0` right after GAP/GATT call | Immediate transport/API-level failure | Treat separately from deferred ATT status; check call ordering/state and retry path. |

## Status layering reminder

Always separate:

1. **Immediate call return / `last_hci_status`**
   - e.g. GAP/GATT function returns error immediately.
2. **Deferred ATT result (`GATT_EVENT_QUERY_COMPLETE`)**
   - operation accepted initially but failed at ATT layer later.
3. **SM pairing outcome (`SM_EVENT_PAIRING_COMPLETE`)**
   - security state that gates retries for protected operations.

Many failures require correlating all three, not one field in isolation.

## Related docs

- Known issues and signatures: [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md)
- BTstack API surface used by this SDK: [`btstack_api_surface.md`](./btstack_api_surface.md)
- Polar↔BTstack protocol mapping: [`btstack_protocol_mapping.md`](./btstack_protocol_mapping.md)
- BLE stability checklist (Pico 2 W): [`../howto/pico2w_ble_stability.md`](../howto/pico2w_ble_stability.md)
