# examples/pico_sdk_psftp probe

Standalone RP2350 BTstack/CYW43 **PSFTP smoke probe** for Polar H10.

Current scope:
- discover PSFTP service/chars,
- force security/pairing,
- enable PSFTP notifications,
- send `GET "/"` and attempt one file download per round,
- reconnect between rounds and print counters.

Current status (2026-03-03):
- discovery + security + CCC path is stable,
- probe now applies shared default BTstack SM auth policy,
- `list_dir("/")` and `download("/DEVICE.BPB")` succeed in repeated rounds,
- endpoint/opcode/cadence knobs and diagnostics remain available for regressions.

Probe behavior notes:
- When `H10_MAX_ROUNDS` is reached, probe now requests a final clean BLE disconnect.

## Build

```bash
cmake -S examples/pico_sdk_psftp -B build/pico_sdk_psftp_probe \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
cmake --build build/pico_sdk_psftp_probe -j$(nproc)
```

## Flash

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico_sdk_psftp_probe/h10_btstack_psftp_probe.elf verify reset exit"
```

## Useful CMake knobs

- `H10_TARGET_ADDR` (default `24:AC:AC:05:A3:10`)
- `H10_MAX_ROUNDS` (default `10`, `0` = infinite)
- `H10_DOWNLOAD_MAX_BYTES` (default `4096`)
- `H10_CLEAR_BOND_ON_BOOT` (default `1`)
  - clears the target bond once at startup for repeatable pairing tests.
- `H10_PSFTP_TX_CHAR_MODE` (default `0`)
  - `0` = write PSFTP requests to MTU characteristic (official SDK parity)
  - `1` = write to H2D characteristic (debug experiment)
- `H10_PSFTP_WRITE_MODE` (default `2`)
  - `0` = always ATT write request (with response)
  - `1` = always ATT write command (without response)
  - `2` = periodic mode (SDK-like): mostly command, every Nth packet request
- `H10_PSFTP_WRITE_PERIOD_N` (default `5`)
  - used when `H10_PSFTP_WRITE_MODE=2`
  - `0` = never use with-response in periodic mode
- `H10_PSFTP_RAW_VALUE_TAP` (default `1`) — unfiltered pre-router value-event tap
- `H10_PSFTP_TX_HEX_DUMP_BYTES` (default `24`)
- `H10_PSFTP_RX_HEX_DUMP_BYTES` (default `24`)
- `H10_PSFTP_TEST_PATH_LEN` (default `1`)
  - `1` => normal `GET "/"`
  - `>1` => synthetic long GET path of requested length (useful for multi-frame TX cadence tests)
- `H10_PSFTP_PRE_TX_DELAY_MS` (default `0`)
  - optional pause after channel prep/CCC and before first PSFTP frame TX
- `POLAR_PROTO_GENERATED_DIR` (default `build/polar_proto`)

## Methodical runbook

Use the 10-check session checklist before/after each change:

### 1) Security proof
- Must show encrypted link before PSFTP op (`enc_key > 0`).
- Capture pairing counters/status/reason.

Pass evidence:
- `SM pairing complete status=0x00`
- `enc=16` (or >0)

### 2) Discovery proof
- Record PSFTP handles + properties each run:
  - MTU (`FB005C51...`)
  - D2H (`FB005C52...`)
  - H2D (`FB005C53...`)

Pass evidence:
- all three discovered, non-zero handles.

### 3) CCC/listener proof
- Confirm CCC enable ATT status for MTU and D2H.
- Confirm local listener flags are true.

Pass evidence:
- query-complete `att=0x00` for both
- listener flags true in runtime state/log.

### 4) **Filter integrity check** (critical; prior pitfall)
- Add/enable temporary **unfiltered GATT value tap** (log handle + len for every value event before router filter).
- Keep routed counters separately.

Pass evidence:
- if device responds, raw tap sees events even when route misses.
- if raw tap sees data but route counter is 0 => filtering bug (not protocol).

### 5) TX endpoint proof
- Log every request write:
  - service UUID + characteristic handle + opcode mode (request/command) and packet index
  - frame length + first bytes (hex)
- For PSFTP request path, compare endpoint with official SDK behavior (MTU char for request/response path).

Pass evidence:
- deterministic handle/opcode/bytes per run.

### 6) TX completion proof
- For write request: require query-complete ATT success.
- For write command: require API success + no immediate transport/busy failure.

Pass evidence:
- no timeout/error in write completion stage.

### 7) RX arrival proof
- Record counters:
  - raw value events on MTU/D2H handles
  - routed PSFTP MTU/D2H events
  - parser input frame count

Pass evidence:
- at least one RX frame after request TX.

### 8) RFC76/parser proof
- For first 1–3 RX frames, log parsed fields:
  - `next`, `status`, `seq`, payload len, error code (if status=0)
- Verify seq progression and status transitions.

Pass evidence:
- parser leaves `MORE` and reaches `LAST` or `ERROR_OR_RESPONSE` deterministically.

### 9) Semantic decode proof
- For `GET "/"`: verify payload bytes decode as directory protobuf.
- For file GET: verify expected non-empty byte payload and first 16 bytes hex.

Pass evidence:
- decode succeeds (or mapped remote PFTP error code is explicit).

### 10) Differential verdict (end of run)
Classify failure into one bucket only:
- A: Security not ready
- B: Discovery/CCC not ready
- C: TX endpoint/opcode/transport issue
- D: RX filtering/routing issue
- E: RFC76/protobuf parse issue
- F: Remote device returned explicit PFTP error

Then define exactly one next action tied to that bucket.

---

## Quick failure-signature mapping

- `enc=0` + ATT 0x05/0x08 => bucket A
- CCC success but raw tap sees RX and routed counter stays 0 => bucket D
- TX increments, raw RX stays 0 => bucket C (or device-side reject; verify with sniffer)
- RX present but parser sequence/protocol error => bucket E
- status=0 + error code => bucket F
