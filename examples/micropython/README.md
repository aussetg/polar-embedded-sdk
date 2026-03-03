# MicroPython examples

## smoke_test.py

`smoke_test.py` is a minimal on-device check that the **C module is compiled in** and importable.

Run with `mpremote` after flashing firmware:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/smoke_test.py :smoke_test.py
mpremote connect /dev/ttyACM0 run :smoke_test.py
```

Expected output (current baseline):
- `polar_ble.version()` prints `0.1.0-dev`
- `polar_ble.build_info()` returns preset/SHA/build-type metadata
- `h10.state()` prints `IDLE`
- `h10.stats()` includes transport counters + discovered handle fields
- `connect(timeout_ms=200)` either succeeds quickly (if target is visible) or raises `polar_ble.TimeoutError`
- `disconnect()` returns cleanly

## hello_driver.py

`hello_driver.py` captures the target lifecycle flow (`connect` + `stats` + `disconnect`).

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/hello_driver.py :hello_driver.py
mpremote connect /dev/ttyACM0 run :hello_driver.py
```

Current behavior depends on radio conditions and whether a matching Polar device is advertising.
The example also demonstrates runtime service scoping via `required_services`.

## transport_connect_cycles.py

`transport_connect_cycles.py` is a transport validation helper for repeated connect/disconnect runs.

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/transport_connect_cycles.py :transport_connect_cycles.py
mpremote connect /dev/ttyACM0 run :transport_connect_cycles.py
```

## hr_read_demo.py

`hr_read_demo.py` is an HR API demo (`start_hr`, `read_hr`, `stop_hr`).

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/hr_read_demo.py :hr_read_demo.py
mpremote connect /dev/ttyACM0 run :hr_read_demo.py
```

## hr_stability_short.py

`hr_stability_short.py` is a short HR streaming stress check.

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/hr_stability_short.py :hr_stability_short.py
mpremote connect /dev/ttyACM0 run :hr_stability_short.py
```

## hr_passive_stats.py

`hr_passive_stats.py` enables HR and prints per-second link/HR counters without calling `read_hr()`.
Useful for checking whether notifications arrive at all.

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/hr_passive_stats.py :hr_passive_stats.py
mpremote connect /dev/ttyACM0 run :hr_passive_stats.py
```

## ecg_read_demo.py

`ecg_read_demo.py` is an ECG control/read demo (`start_ecg`, `read_ecg`, `stop_ecg`).

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/ecg_read_demo.py :ecg_read_demo.py
mpremote connect /dev/ttyACM0 run :ecg_read_demo.py
```

## ecg_hr_lcd_gfx_demo.py

`ecg_hr_lcd_gfx_demo.py` renders live HR text (top-left) and an ECG waveform on a Pimoroni LCD via `picographics`.

Requirements:
- firmware includes `polar_ble` + `picographics`
- works with Pico GFX Pack (`DISPLAY_GFX_PACK`) and several Pico Display/LCD modes (auto-detected in script)

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/ecg_hr_lcd_gfx_demo.py :ecg_hr_lcd_gfx_demo.py
mpremote connect /dev/ttyACM0 run :ecg_hr_lcd_gfx_demo.py
```

## psftp_list_demo.py

`psftp_list_demo.py` performs a simple PSFTP directory listing (`list_dir`).

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/psftp_list_demo.py :psftp_list_demo.py
mpremote connect /dev/ttyACM0 run :psftp_list_demo.py
```

## psftp_download_demo.py

`psftp_download_demo.py` lists a directory and downloads one small file (`download`).

Run with `mpremote`:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/psftp_download_demo.py :psftp_download_demo.py
mpremote connect /dev/ttyACM0 run :psftp_download_demo.py
```

Legacy investigation notes/logs live under:
- `.agent/archive/investigations/`
