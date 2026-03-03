#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

RUN_HR_SMOKE=0
PORT="${POLAR_MP_PORT:-}"
ADDR="${POLAR_H10_ADDR:-}"

usage() {
  cat <<'EOF'
Usage: scripts/btstack_change_checklist.sh [--with-hr-smoke] [--port /dev/ttyACM0] [--addr AA:BB:CC:DD:EE:FF]

Runs the BTstack change checklist:
  1) Build pico-sdk probe (examples/pico_sdk)
  2) Build MicroPython rp2 firmware (fw-rp2-1)
  3) Minimal HR smoke test (optional --with-hr-smoke)

Environment:
  POLAR_MP_PORT    default serial port for --with-hr-smoke
  POLAR_H10_ADDR   optional explicit H10 address for --with-hr-smoke
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-hr-smoke)
      RUN_HR_SMOKE=1
      shift
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --addr)
      ADDR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

echo "[1/3] Build pico-sdk probe"
rm -rf build/pico_sdk_probe_btstack_check
PICO_SDK_PATH="${REPO_ROOT}/vendors/pico-sdk" \
PICO_BTSTACK_PATH="${REPO_ROOT}/vendors/pico-sdk/lib/btstack" \
cmake -S examples/pico_sdk -B build/pico_sdk_probe_btstack_check \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
cmake --build build/pico_sdk_probe_btstack_check -j"$(nproc)"

echo "[2/3] Build MicroPython rp2 firmware"
cmake --preset fw-rp2-1
cmake --build --preset fw-rp2-1 -j"$(nproc)"

if [[ "${RUN_HR_SMOKE}" -eq 0 ]]; then
  echo "[3/3] HR smoke test skipped (use --with-hr-smoke to run it)"
  echo "Checklist complete: build steps passed."
  exit 0
fi

if ! command -v mpremote >/dev/null 2>&1; then
  echo "mpremote not found; cannot run HR smoke test" >&2
  exit 2
fi

if [[ -z "${PORT}" ]]; then
  PORT="$(ls /dev/ttyACM* 2>/dev/null | head -n1 || true)"
fi

if [[ -z "${PORT}" ]]; then
  echo "No serial port found (set --port or POLAR_MP_PORT)." >&2
  exit 2
fi

echo "[3/3] Run minimal HR smoke test on ${PORT}"
TMP_SCRIPT="$(mktemp /tmp/polar_btstack_hr_smoke_XXXX.py)"
cat >"${TMP_SCRIPT}" <<'PY'
import time
import polar_sdk

ADDR = ""  # injected by shell

if ADDR:
    h10 = polar_sdk.H10(ADDR, required_services=polar_sdk.SERVICE_HR)
else:
    h10 = polar_sdk.H10(None, required_services=polar_sdk.SERVICE_HR)

h10.connect(timeout_ms=15000)
h10.start_hr()

sample = None
start = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), start) < 15000:
    hr = h10.read_hr(timeout_ms=2000)
    if hr is not None:
        sample = hr
        break

print("hr_sample_ok", sample is not None)
print("state", h10.state())
print("stats", h10.stats())

try:
    h10.stop_hr()
finally:
    h10.disconnect()

if sample is None:
    raise Exception("No HR sample received during smoke window")

print("OK")
PY

python - <<PY
from pathlib import Path
p = Path("${TMP_SCRIPT}")
s = p.read_text()
s = s.replace('ADDR = ""  # injected by shell', 'ADDR = "${ADDR}"')
p.write_text(s)
PY

mpremote connect "${PORT}" run "${TMP_SCRIPT}"
rm -f "${TMP_SCRIPT}"

echo "Checklist complete: all steps passed."
