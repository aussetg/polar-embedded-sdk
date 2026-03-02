# cmake file for RP2-1 prototype logger (Pimoroni Pico Plus 2 W based)

set(PICO_BOARD "pimoroni_pico_plus2_w_rp2350")
set(PICO_PLATFORM "rp2350")
set(PICO_NUM_GPIOS 48)

set(MICROPY_PY_LWIP ON)
set(MICROPY_PY_NETWORK_CYW43 ON)

# Bluetooth
set(MICROPY_PY_BLUETOOTH ON)
set(MICROPY_BLUETOOTH_BTSTACK ON)
set(MICROPY_PY_BLUETOOTH_CYW43 ON)

# Board specific version of the frozen manifest
set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)
