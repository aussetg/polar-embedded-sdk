cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../vendors/pico-sdk" CACHE PATH "Path to pico-sdk")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "pico-sdk import file not found under PICO_SDK_PATH=${PICO_SDK_PATH}")
endif()

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pimoroni_pico_lipo2xl_w" CACHE STRING "Pico SDK board for the logger appliance")
endif()

set(_logger_board_header_dir "${CMAKE_CURRENT_LIST_DIR}/../firmware/boards/RP2_2")
if(DEFINED PICO_BOARD_HEADER_DIRS AND NOT PICO_BOARD_HEADER_DIRS STREQUAL "")
    set(PICO_BOARD_HEADER_DIRS "${PICO_BOARD_HEADER_DIRS};${_logger_board_header_dir}" CACHE STRING "Additional Pico board header search paths" FORCE)
else()
    set(PICO_BOARD_HEADER_DIRS "${_logger_board_header_dir}" CACHE STRING "Additional Pico board header search paths" FORCE)
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(logger_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(LOGGER_FIRMWARE_VERSION "0.1.0-dev" CACHE STRING "Semantic firmware version string")
set(LOGGER_BUILD_ID "logger-fw-dev" CACHE STRING "Opaque firmware build identifier")

pico_sdk_init()

add_executable(logger_appliance
    src/main.c
    src/app_main.c
    src/app_state.c
    src/battery.c
    src/button.c
    src/clock.c
    src/config_store.c
    src/faults.c
    src/identity.c
    src/json.c
    src/journal.c
    src/queue.c
    src/session.c
    src/service_cli.c
    src/sha256.c
    src/storage.c
    src/system_log.c
    src/upload.c
    src/upload_bundle.c
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/microtar/src/microtar.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ff.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ffunicode.c
)

target_include_directories(logger_appliance PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/boards/rp2_2
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/jsmn
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/microtar/src
)

target_link_libraries(logger_appliance PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_rand
    pico_sha256
    pico_unique_id
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_i2c
    hardware_spi
    hardware_watchdog
)

target_compile_definitions(logger_appliance PRIVATE
    LOGGER_FIRMWARE_VERSION="${LOGGER_FIRMWARE_VERSION}"
    LOGGER_BUILD_ID="${LOGGER_BUILD_ID}"
)

target_compile_options(logger_appliance PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wdouble-promotion
)

pico_enable_stdio_usb(logger_appliance 1)
pico_enable_stdio_uart(logger_appliance 0)

pico_add_extra_outputs(logger_appliance)
