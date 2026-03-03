# How-to — BLE stability checklist (Pico 2 W / RP2350 + CYW43)

Status: How-to (troubleshooting)
Last updated: 2026-02-24

This repo targets **Pico 2 W** (RP2350 + CYW43439 “CYW43”). BLE on this platform can look like “resource/IRQ contention” when the host cannot service the CYW43 bus/IRQ quickly enough.

This guide collects **practical, repeatable** things to check when you see:
- disconnect reason `0x08` (*Connection Timeout* / supervision timeout),
- long stalls (no notifications) followed by disconnect,
- instability that correlates with heavy Python load, timers, or WiFi usage.

For MicroPython-module-specific symptoms/mitigations see: [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md)

## 1) Understand the CYW43 bus/IRQ coupling on Pico 2 W

On Pico 2 W the CYW43 uses a **gSPI-style PIO SPI** bus where **data and “host wake” share the same GPIO**:

- `CYW43_DEFAULT_PIN_WL_DATA_OUT = 24`
- `CYW43_DEFAULT_PIN_WL_DATA_IN  = 24`
- `CYW43_DEFAULT_PIN_WL_HOST_WAKE = 24`

Source: `vendors/micropython/lib/pico-sdk/src/boards/include/boards/pico2_w.h`.

MicroPython’s rp2 port therefore uses a **level GPIO interrupt** for CYW43 host-wake and **disables the IRQ until `cyw43_poll()` runs**, otherwise it would immediately retrigger.

Source: `vendors/micropython/ports/rp2/mpnetworkport.c` (see the comment in `gpio_irq_handler`).

**Implication:** Anything that delays the `cyw43_poll()`/PendSV path (or starves core 0) can turn into “radio timeouts”.

## 2) Firmware baseline: use a Pico SDK with RP2350 CYW43 fixes

Upstream reports (MicroPython + Pico SDK) indicate that older SDK versions had **RP2350-specific CYW43 failures** (notably around DMA/abort/chaining) that can manifest as random wireless hangs or disconnects.

Mitigation:
- Prefer **Pico SDK ≥ 2.1.1**.
- This repo’s vendored MicroPython currently uses **Pico SDK 2.2.0**, so it should already include relevant fixes.

Reference:
- MicroPython issue: https://github.com/micropython/micropython/issues/16627

## 3) Avoid self-inflicted contention

### 3.1 Don’t use WiFi while doing BLE soak tests (unless you *must*)

WiFi and BLE share:
- the **same CYW43 chip**,
- the same **PIO SPI bus**,
- the same **host-wake IRQ mechanism**.

If your goal is “hours-long stable BLE connection”, keep WiFi **inactive** and avoid importing/initialising network stacks during the run.

### 3.2 Avoid touching `Pin('LED')` during BLE tests

On Pico W / Pico 2 W the onboard LED is a **CYW43 GPIO**, not an RP2350 GPIO.

Touching `'LED'` can keep the CYW43 powered/active and can interfere with low-power tests and (depending on your program structure) with wireless lifecycle.

Reference (MicroPython discussion on sleep + CYW43):
- https://github.com/orgs/micropython/discussions/10889

### 3.3 Keep IRQ/timer callbacks tiny

If you have high-frequency timers or GPIO IRQ handlers, keep them short and avoid allocations/prints.

Prefer:
- do minimal work in ISR,
- push heavy work to the VM via `micropython.schedule()`.

Reference:
- https://github.com/orgs/micropython/discussions/11704

## 4) Multi-core and bus-priority caveats (RP2350)

Upstream reports suggest that **bus priority changes** or heavy core-1 workloads can starve core 0 enough to break CYW43 initialisation or ongoing operation.

Mitigation:
- Keep CYW43/BTstack “critical progress” on core 0.
- If you use core 1, ensure it cannot starve core 0 (no busy loops without sleeps/yields; avoid bus-priority tweaks).

Reference:
- Pico SDK issue: https://github.com/raspberrypi/pico-sdk/issues/2123

## 5) Sleep / power-cycling caveats

Upstream reports indicate that **BLE can be unreliable across init/deinit cycles** (“power cycling”) on Pico 2 W in some SDK versions.

Mitigation (for now, in this project):
- Treat “sleep with BLE” as a separate feature to validate.
- For long-running logging, prefer keeping BLE up rather than repeated deinit/reinit.

Reference:
- Pico SDK issue: https://github.com/raspberrypi/pico-sdk/issues/2833

## 6) When you see disconnect reason `0x08`

A supervision timeout is often the symptom of “the host did not service the link/controller in time”, not necessarily RF range.

When debugging:
- capture `h10.stats()` periodically (for this repo’s MicroPython module),
- correlate with Python workload (printing, timers, other I/O),
- rerun with WiFi fully disabled and no LED access,
- verify your firmware baseline (Pico SDK version).
