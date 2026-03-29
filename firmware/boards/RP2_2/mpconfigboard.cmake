# cmake file for RP2-2 prototype logger
# (Pimoroni Pico LiPo 2 XL W + Adafruit PiCowbell Adalogger)

# The Pico LiPo 2 XL W is not present in the vendored pico-sdk board list, so
# provide the custom board header from this board directory.
list(APPEND PICO_BOARD_HEADER_DIRS ${MICROPY_BOARD_DIR})

set(PICO_BOARD "pimoroni_pico_lipo2xl_w")
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
