# Minimal Pimoroni user-module bundle for upstream/vendored MicroPython rp2 builds.
#
# Goal: provide only PicoGraphics essentials for the LCD demo, without pulling
# the full Pimoroni board profile (which can require extra modules not present
# in this repo checkout).

# Repo path assumptions:
# - this file: firmware/cmake/pimoroni_minimal.cmake
# - Pimoroni repo: vendors/pimoroni-pico/

set(PIMORONI_MP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../vendors/pimoroni-pico/micropython)
set(PIMORONI_MODULES_DIR ${PIMORONI_MP_ROOT}/modules)

include_directories(${PIMORONI_MODULES_DIR}/../../)
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR})
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR}/../)
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR}/../../)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Disable C++ exceptions globally for this build context so Pimoroni C++ module
# objects don't pull in libstdc++ exception runtime/alloc pools on rp2.
add_compile_definitions(PICO_CXX_ENABLE_EXCEPTIONS=0)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-unwind-tables -fno-rtti -fno-use-cxa-atexit")

# Essential-only stack for the LCD demo:
# - picographics
# - required Pimoroni bus/I2C helper modules used by picographics
# - bitmap_fonts (required by pico_graphics text rendering symbols)
# - cppmem (Pimoroni C++ allocation shim)
include(cppmem/micropython)
include(pimoroni_i2c/micropython)
include(pimoroni_bus/micropython)
include(bitmap_fonts/micropython)
include(picographics/micropython)

# Compatibility shim for Pimoroni picographics against newer MicroPython API,
# where mp_handle_pending now takes mp_handle_pending_behaviour_t.
#
# Force-include a C++ compatibility header for picographics.cpp only.
set_source_files_properties(
    ${PIMORONI_MODULES_DIR}/picographics/picographics.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_LIST_DIR}/pimoroni_mp_compat.h"
)

# No Pimoroni helper .py modules are frozen here on purpose (minimal footprint).
