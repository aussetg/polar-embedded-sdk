# Minimal Pimoroni user-module bundle for RP2-1 RTC support.
#
# Includes only what is needed for RV3028 BreakoutRTC access:
# - cppmem
# - pimoroni_i2c
# - pimoroni_bus
# - breakout_rtc (RV3028 driver/binding)

set(PIMORONI_MP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../vendors/pimoroni-pico/micropython)
set(PIMORONI_MODULES_DIR ${PIMORONI_MP_ROOT}/modules)

include_directories(${PIMORONI_MODULES_DIR}/../../)
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR})
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR}/../)
list(APPEND CMAKE_MODULE_PATH ${PIMORONI_MODULES_DIR}/../../)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Keep runtime footprint small for C++ user modules.
add_compile_definitions(PICO_CXX_ENABLE_EXCEPTIONS=0)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-unwind-tables -fno-rtti -fno-use-cxa-atexit")

include(cppmem/micropython)
include(pimoroni_i2c/micropython)
include(pimoroni_bus/micropython)
include(breakout_rtc/micropython)
