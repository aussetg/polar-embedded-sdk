# How-to — Compare MicroPython vs probe PSFTP sessions on air

Status: Investigation note
Last updated: 2026-03-18

Note: this is a historical investigation snapshot. The later rp2 TLV flash-bank
reservation + flash-safety fixes resolved the soft-reset bond-persistence
problem that was still present when this comparison was captured.

This note captures the first useful **nRF Sniffer reference comparison** between:

- the pure C pico-sdk PSFTP probe, and
- the MicroPython `polar_sdk` module on the same Pico 2 W hardware.

The goal was to stop guessing about the remaining MPY PSFTP security failure and compare the two paths **on air**.

## Why this comparison was needed

By this point we already knew:

- H10 legacy recording status/query works from the pure C probe.
- The public PSFTP query/control helpers are hardware-validated in the probe.
- MPY still intermittently fails with:
  - `SecurityError: PSFTP security setup failed`
  - `sm_last_pairing_status = 19`
  - `last_disconnect_reason = 22`

That made it important to answer a narrower question:

> Does MPY actually perform a different SMP / ATT / PSFTP sequence on air than the probe?

## Session artifacts

Local, non-reference capture files from this session:

- MPY failure capture:
  - `captures/mpy_psftp_retry_20260307_220422.pcapng`
  - `captures/mpy_psftp_retry_20260307_220422.log`
- Probe reference capture:
  - `captures/probe_psftp_ref_20260307_220559.pcapng`
  - `captures/probe_psftp_ref_20260307_220559.log`

These filenames are just the concrete artifacts from this run; the important part is the behavioral summary below.

## High-level result

The sniffer comparison shows that the remaining divergence is **not** in H10 PSFTP framing.

Instead, the critical difference is in the **security / pairing sequence**:

- the **probe** completes the expected SMP pairing flow and then successfully enables PSFTP channels and exchanges PSFTP traffic,
- while **MPY** starts pairing, receives an extra **Security Request** from the H10, and then the pairing flow stalls before it reaches the confirm/random phase.

That shifts the working assumption toward an **MPY runtime sequencing / BTstack integration issue**, not a PSFTP protocol-encoding issue.

## Shared behavior between probe and MPY

The capture also ruled out some simpler explanations.

Both paths used the same visible initiator identity and initial connection profile:

- initiator address: `28:cd:c1:15:88:16`
- target address: `24:ac:ac:05:a3:10`
- initial connection parameters observed in CONNECT_IND:
  - interval `24`
  - latency `4`
  - timeout `72`

So the remaining issue is **not** explained by:

- a different central address,
- obviously different initial LL connection parameters,
- or a different H10 target selection.

## MPY failure signature on air

In the useful MPY failure capture, repeated connections were observed.

For the first failed session the SMP sequence was:

1. `0x01` — Pairing Request (from the Pico/central)
2. `0x0b` — Security Request (from the H10)
3. `0x02` — Pairing Response (from the H10)

And then the exchange stopped.

Notably absent from the MPY failure trace:

- `0x03` Pairing Confirm
- `0x04` Pairing Random
- later key distribution messages

The same capture also showed that MPY never reached the richer ATT/PSFTP phase that the probe reaches later.

Observed ATT mix in the MPY failure sessions was mostly limited to early discovery/setup traffic such as:

- `0x10`, `0x11`
- `0x08`, `0x09`
- `0x01`

and then the session terminated.

## Probe reference signature on air

In the probe reference capture, the first session showed the expected SMP progression:

1. `0x01` — Pairing Request
2. `0x02` — Pairing Response
3. `0x03` — Pairing Confirm
4. `0x03` — Pairing Confirm
5. `0x04` — Pairing Random
6. `0x04` — Pairing Random
7. later key distribution traffic:
   - `0x06`
   - `0x07`
   - `0x08`
   - `0x09`

The probe serial log from the same run matches that interpretation:

- `SM just works request`
- `SM pairing complete status=0x00 reason=0x00`
- encryption becomes active (`enc=16`)
- CCC writes complete
- PSFTP query executes
- H10 recording status response is decoded

The probe trace also shows the ATT activity that is missing from the MPY failure path, including:

- MTU exchange (`0x02`, `0x03`)
- additional attribute reads/writes
- PSFTP-related traffic observed in the summary as opcodes including `0x52` and `0x1b`

## The most important differential

The single most important on-air difference from this session is:

- **Probe:** `Pairing Request -> Pairing Response -> Confirm/Random -> key distribution -> encrypted PSFTP traffic`
- **MPY:** `Pairing Request -> Security Request -> Pairing Response -> stall/disconnect`

That is the clearest evidence so far that MPY is not merely “failing later”; it is entering a **different security interaction** with the H10.

## Interpretation

Current working interpretation:

- the probe owns a clean, deterministic sequence:
  1. connect
  2. discover
  3. explicitly request pairing when needed
  4. complete SMP
  5. enable PSFTP notifications
  6. execute PSFTP operations
- the MPY path is still triggering a different sequence around the first security-sensitive PSFTP step,
- and that altered sequence is visible on air before PSFTP traffic really begins.

This does **not** prove the exact root cause yet, but it strongly supports the following narrower assumption:

> The remaining MPY failure is primarily a runtime sequencing / BTstack-integration problem around SMP and the first secured PSFTP preparation step.

## What this comparison ruled out

This session made several earlier theories less likely:

- **not primarily a PSFTP query-encoding problem**
  - the probe uses the same public PSFTP helpers successfully.
- **not primarily a wrong target / wrong central identity problem**
  - initiator/target pairing on air matched expectations.
- **not explained only by initial LL connection parameters**
  - probe and MPY showed the same initial CONNECT_IND values.

## What to do with this result

The capture suggests future MPY debugging should focus on:

1. what MPY does immediately before the H10 emits the extra `Security Request`,
2. whether MPY touches a security-sensitive ATT/PSFTP path too early,
3. whether MicroPython BTstack lifecycle/handlers are causing SMP to start in a different state than the probe,
4. whether PSFTP channel preparation should be made more probe-like and more explicitly serialized.

## Repeatable capture workflow

Use the repo-local sniffer helper:

```bash
python .pi/skills/polar-host-tools/scripts/bluetooth_sniffer.py capture \
  --interface /dev/serial/by-id/usb-ZEPHYR_nRF_Sniffer_for_Bluetooth_LE_C2FEC66FEE9BAA46-if00 \
  --addr 24:ac:ac:05:a3:10 \
  --addr-type public \
  --scan-follow-rsp \
  --duration 70 \
  --output captures/<name>.pcapng
```

Analyze with:

```bash
python .pi/skills/polar-host-tools/scripts/bluetooth_sniffer.py analyze \
  captures/<name>.pcapng \
  --addr 24:ac:ac:05:a3:10
```

### Minimal MPY repro during capture

Use a minimal script that makes the PSFTP/recording path fail as early and reproducibly as possible, for example:

```python
import polar_sdk

h = polar_sdk.Device(addr='24:AC:AC:05:A3:10')
try:
    h.connect(timeout_ms=20000, required_capabilities=('psftp:read',))
    print(h.recording_status())
finally:
    try:
        h.disconnect(timeout_ms=5000)
    except Exception:
        pass
```

### Probe reference during capture

Flash the pure C PSFTP probe and capture its serial log alongside the pcap. The probe is the reference because it already performs the validated H10 PSFTP path successfully.

## tshark snippets used in this comparison

List CONNECT_IND frames for the target:

```bash
tshark -r captures/<name>.pcapng \
  -Y 'btle.advertising_header.pdu_type==0x05' \
  -T fields \
  -e frame.number \
  -e frame.time_relative \
  -e btle.advertising_address \
  -e btle.initiator_address
```

Inspect one connection by access address:

```bash
tshark -r captures/<name>.pcapng \
  -Y 'btle.access_address==0x<aa>' \
  -T fields \
  -e frame.number \
  -e frame.time_relative \
  -e btle.control_opcode \
  -e btatt.opcode \
  -e btsmp.opcode \
  -e btsmp.reason
```

Inspect SMP details for one connection:

```bash
tshark -r captures/<name>.pcapng \
  -Y 'btle.access_address==0x<aa> && btsmp.opcode' \
  -T fields \
  -e frame.number \
  -e frame.time_relative \
  -e nordic_ble.direction \
  -e btsmp.opcode \
  -e btsmp.io_capability \
  -e btsmp.authreq \
  -e btsmp.max_enc_key_size \
  -e btsmp.initiator_key_distribution \
  -e btsmp.responder_key_distribution \
  -e btsmp.reason
```

## Next debugging checklist

When repeating this comparison, check these in order:

1. **Did both paths connect with the same initiator and initial connection parameters?**
2. **Does MPY show an extra `Security Request (0x0b)` where the probe does not?**
3. **Does MPY ever reach `Pairing Confirm (0x03)` / `Pairing Random (0x04)`?**
4. **Does encryption become active before CCC writes / PSFTP traffic in the good probe path?**
5. **What ATT operation happens immediately before the MPY SMP divergence?**
6. **Does MPY ever reach the same PSFTP-bearing ATT phase as the probe (`0x52`, `0x1b`, etc.)?**

If the answer to steps 2–3 still shows the same divergence, keep treating the issue as an MPY-side runtime/security sequencing problem first.

## Practical takeaway

If the pure C probe works but the MPY path still fails, the first thing to compare now is **the SMP sequence on air**, not the PSFTP payload bytes.