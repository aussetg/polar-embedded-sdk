# Research workflows

This page is for the common case:

- you have a Pico 2 W with the `polar_sdk` firmware,
- you have a Polar H10,
- you want a practical data-collection workflow,
- you do **not** want to understand BLE internals.

The workflows below are designed to be:

- easy to run,
- safe to modify a little,
- conservative about device-side work.

## Before every session

1. Wear the H10 strap correctly.
2. Make sure the Pico is powered and connected over USB.
3. If several Polar devices are nearby, first run `examples/micropython/scan_polar.py` and note the address of the correct one.
4. Keep your script simple while recording. Avoid heavy printing.

## Workflow 1: Heart-rate CSV capture to the host computer

Use this when you want:

- BPM,
- RR intervals,
- a spreadsheet-friendly file.

### Ready-made script

Use:

- `examples/micropython/hr_csv_capture.py`

### Copy it to the Pico

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/hr_csv_capture.py :hr_csv_capture.py
```

### Run it and save the output on your computer

```bash
mpremote connect /dev/ttyACM0 run :hr_csv_capture.py > hr_capture.csv
```

### What the file contains

The first non-comment line is the CSV header:

```text
host_ms,bpm,flags,rr_count,rr0_ms,rr1_ms,rr2_ms,rr3_ms
```

Meaning:

- `host_ms`: milliseconds since the script started
- `bpm`: heart rate
- `flags`: SDK public HR flags
- `rr_count`: how many RR values are valid on that line
- `rr*_ms`: RR intervals in milliseconds

### If you need the exact device address

Edit the script and set:

```python
TARGET_ADDR = "AA:BB:CC:DD:EE:FF"
```

Then the script will connect to that specific strap.

## Workflow 2: ECG capture to the host computer as hex chunks

Use this when you want:

- raw ECG samples,
- no on-device file writing,
- a simple host-side capture path.

This workflow intentionally prints **one line per ECG chunk** instead of one line per sample.
That keeps Python overhead much lower.

### Ready-made script

Use:

- `examples/micropython/ecg_hex_capture.py`

### Copy it to the Pico

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/ecg_hex_capture.py :ecg_hex_capture.py
```

### Run it and save the output on your computer

```bash
mpremote connect /dev/ttyACM0 run :ecg_hex_capture.py > ecg_capture.txt
```

### Output format

Data lines look like this:

```text
DATA,<host_ms>,<n_bytes>,<hex_payload>
```

Where:

- `host_ms` is milliseconds since script start
- `n_bytes` is the binary chunk length
- `hex_payload` is the ECG chunk encoded as hexadecimal text

Each ECG sample in the binary payload is:

- 4 bytes
- signed little-endian `int32`
- unit: microvolts

## Convert `ecg_capture.txt` into usable files on your computer

Run this normal desktop Python script on your computer:

```python
from pathlib import Path

src = Path("ecg_capture.txt")
raw_out = Path("ecg_samples.bin")
timeline_out = Path("ecg_timeline.csv")

with src.open("r", encoding="utf-8") as fin, \
     raw_out.open("wb") as fraw, \
     timeline_out.open("w", encoding="utf-8") as ft:
    ft.write("host_ms,offset_bytes,chunk_bytes\n")
    offset = 0
    for line in fin:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if not line.startswith("DATA,"):
            continue

        _, host_ms, n_bytes, hex_payload = line.split(",", 3)
        blob = bytes.fromhex(hex_payload)
        n_bytes = int(n_bytes)
        if len(blob) != n_bytes:
            raise ValueError(f"length mismatch: expected {n_bytes}, got {len(blob)}")

        fraw.write(blob)
        ft.write(f"{host_ms},{offset},{n_bytes}\n")
        offset += n_bytes
```

You then get:

- `ecg_samples.bin` — raw ECG binary data
- `ecg_timeline.csv` — when each chunk was read by the script

## Convert the binary ECG file to integers

```python
from pathlib import Path

data = Path("ecg_samples.bin").read_bytes()
samples = [
    int.from_bytes(data[i:i+4], "little", signed=True)
    for i in range(0, len(data), 4)
]

print("samples:", len(samples))
print("first 10:", samples[:10])
```

## Workflow 3: short validation run before a real session

Before a long capture, run one of these quick checks:

- `examples/micropython/hello_polar.py`
- `examples/micropython/hr_read_demo.py`
- `examples/micropython/ecg_read_demo.py`
- `examples/micropython/capabilities_demo.py`

This catches the common problems early:

- strap not advertising,
- wrong device nearby,
- build missing a feature,
- pairing/security not ready.

## Workflow 4: inspect H10 onboard recordings

Use this when you want to check the device’s own stored recordings:

- `examples/micropython/recording_hr_demo.py`
- `examples/micropython/psftp_list_demo.py`
- `examples/micropython/psftp_download_demo.py`

Important H10 behavior:

- a stopped HR recording can block creation of a new one
- if `recording_start()` fails, inspect `recording_list()` and delete old recordings first

## Recommended editing knobs

The scripts are meant to be edited only near the top.

Common settings:

- capture duration
- target address
- connection timeout
- read timeout
- sample rate

If you are not comfortable programming, change **only the constants near the top of the script**.

## Practical advice for reliable runs

- Keep the Pico powered from a stable USB source.
- Do not spam the serial console with extra prints.
- For ECG, prefer chunk-based capture rather than sample-by-sample printing.
- If a run stops unexpectedly, call `stats()` in a shorter diagnostic script and inspect disconnect/security counters.

## Related documentation

- [Quick start](./quickstart.md)
- [API reference](./api_reference.md)
- [Data formats](./data_formats.md)
- [`examples/micropython/README.md`](../../examples/micropython/README.md)