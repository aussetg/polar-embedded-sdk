# Spec — Potential upstream MicroPython BTstack reliability patch (global)

Status: Draft proposal
Last updated: 2026-02-24
Audience: MicroPython maintainers / BTstack backend maintainers

## 1) Problem statement

On rp2/BTstack/CYW43, observed behavior suggests BLE link stability can be impacted by runtime orchestration under some workloads.

Empirical clues:
- C-only probe on same hardware is stable in tested windows.
- MicroPython BLE path can show timeout-based failure signatures under specific operation timing.

This indicates a likely integration/scheduling sensitivity rather than a pure controller limitation.

### 1.1 Platform context (Pico 2 W: RP2350 + CYW43)

On Pico 2 W the CYW43 “gSPI” transport uses a **single GPIO for both SPI data and host-wake IRQ**:
- `CYW43_DEFAULT_PIN_WL_DATA_OUT = 24`
- `CYW43_DEFAULT_PIN_WL_DATA_IN  = 24`
- `CYW43_DEFAULT_PIN_WL_HOST_WAKE = 24`

Source: `lib/pico-sdk/src/boards/include/boards/pico2_w.h`.

MicroPython’s rp2 port therefore uses a **level GPIO interrupt** for CYW43 host-wake and temporarily disables that IRQ until the scheduled `cyw43_poll()` runs (see the comment in `ports/rp2/mpnetworkport.c`, `gpio_irq_handler`).

Implication: if PendSV dispatch and/or `mp_bluetooth_hci_poll()` is delayed under Python load, CYW43 progress can stall long enough to look like “random timeouts” at the BLE link layer (e.g. disconnect reason `0x08`).

## 2) Goal

Improve robustness of BTstack-based ports in MicroPython under callback load and bursty event patterns, while preserving existing API semantics.

## 3) Non-goals

- Rewriting BTstack itself.
- Changing Python BLE public API behavior.
- Port-specific hacks that cannot be gated/configured.

## 4) Proposed solution areas

## 4.1 Scheduler/polling hardening (port-level)

Target files (rp2 first):
- `ports/rp2/mpbthciport.c`
- optionally shared scheduler logic where appropriate.

Proposals:
1. **Poll-gap guardrails**
   - Track max time between `mp_bluetooth_hci_poll()` runs.
   - If gap exceeds threshold, trigger immediate poll scheduling.
2. **Coalescing visibility**
   - Count schedule-node coalescing events.
   - Expose counters for diagnostics.
3. **Bounded drain loop**
   - Ensure one scheduled poll call drains enough pending work before returning.

## 4.2 Event delivery resilience (extmod-level)

Target files:
- `extmod/modbluetooth.c`
- `extmod/btstack/modbluetooth_btstack.c`

Proposals:
1. **Optional deferred-delivery mode for BTstack ports**
   - Route BT events through queue/ring path where feasible.
   - Reduce risk that long Python handlers stall BLE progress.
2. **Priority classing of events**
   - Preserve critical events (disconnect/security/update) ahead of noisy scan traffic.
3. **Explicit overflow accounting**
   - Per-event drop counters and high-water marks.

## 4.3 Diagnostics API (opt-in)

Add debug stats retrieval under config flag:
- scheduler poll stats,
- event queue depth/high-water,
- dropped event counters,
- callback latency histogram.

This enables maintainers/users to report evidence rather than symptoms.

## 5) Compatibility strategy

- Keep default behavior unchanged unless port enables hardened mode.
- Introduce feature flags with conservative defaults.
- Provide migration note and testing guide.

## 6) Rollout plan

1. **Step A: instrumentation only**
   - No behavior change.
   - Gather data on representative boards (rp2/esp32/others).
2. **Step B: opt-in hardening mode**
   - Port-level enablement for rp2 first.
3. **Step C: broaden if proven safe**
   - Extend to other BTstack ports where beneficial.

## 7) Validation matrix

For each candidate port:
- high-rate scan + connect loops,
- GATT notify stream with Python callback load,
- pairing/encryption transitions,
- reconnection after forced disconnect,
- long-run soak test.

Pass criteria:
- no regression in existing BLE test suite,
- reduced timeout/disconnect incidence under stress,
- stable memory and scheduler behavior.

## 8) Risks

- Added complexity in event ordering and queue semantics.
- Potential latency increase for some callbacks.
- Cross-port behavior divergence if not clearly gated.

Mitigations:
- feature-flag rollout,
- instrumentation-first approach,
- explicit per-port opt-in.

## 9) Deliverables

- patch series (instrumentation + optional hardening),
- docs for new config flags and debug counters,
- reproducible stress scripts and captures,
- maintainer-facing summary of measured improvements/regressions.
