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

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(LOGGER_FIRMWARE_VERSION "0.1.0-dev" CACHE STRING "Semantic firmware version string")
set(LOGGER_BUILD_ID "logger-fw-dev" CACHE STRING "Opaque firmware build identifier")
set(PICO_MBEDTLS_CONFIG_FILE "logger_mbedtls_config.h" CACHE STRING "Project-local Mbed TLS configuration header")

set(LOGGER_TLS_ROOTS_SOURCE_PEM "${CMAKE_CURRENT_LIST_DIR}/tls/public_roots_v1.pem")
set(LOGGER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LOGGER_UPLOAD_TLS_ROOTS_HEADER "${LOGGER_GENERATED_DIR}/upload_tls_roots.h")

add_custom_command(
    OUTPUT "${LOGGER_UPLOAD_TLS_ROOTS_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGGER_GENERATED_DIR}"
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_LIST_DIR}/../scripts/logger_tls_codegen.py"
            --input-pem "${LOGGER_TLS_ROOTS_SOURCE_PEM}"
            --output-header "${LOGGER_UPLOAD_TLS_ROOTS_HEADER}"
            --profile-name "logger-public-roots-v1"
    DEPENDS
        "${LOGGER_TLS_ROOTS_SOURCE_PEM}"
        "${CMAKE_CURRENT_LIST_DIR}/../scripts/logger_tls_codegen.py"
    VERBATIM
)

pico_sdk_init()

add_executable(logger_appliance
    ${LOGGER_UPLOAD_TLS_ROOTS_HEADER}
    src/main.c
    src/app_main.c
    src/app_state.c
    src/battery.c
    src/button.c
    src/capture_stats.c
    src/chunk_builder.c
    src/clock.c
    src/config_store.c
    src/faults.c
    src/h10.c
    src/identity.c
    src/mbedtls_platform.c
    src/net.c
    src/json.c
    src/json_writer.c
    src/json_stream_writer.c
    src/journal.c
    src/journal_writer.c
    src/queue.c
    src/session.c
    src/service_cli.c
    src/sha256.c
    src/storage.c
    src/system_log.c
    src/upload.c
    src/upload_bundle.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_helpers.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_gatt.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_adv_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_security.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_dispatch.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_link.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_scan.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_btstack_sm.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_common.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_connect.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_discovery_apply.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_gatt_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_gatt_mtu.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_gatt_notify_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_pmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_pmd_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_pmd_start.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_runtime_context.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_security.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_sm_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/src/polar_sdk_transport_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/microtar/src/microtar.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ff.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ffunicode.c
)

target_include_directories(logger_appliance PRIVATE
    ${LOGGER_GENERATED_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/boards/rp2_2
    ${CMAKE_CURRENT_LIST_DIR}/../polar_sdk/core/include
    ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/jsmn
    ${CMAKE_CURRENT_LIST_DIR}/../vendors/microtar/src
)

target_link_libraries(logger_appliance PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mbedtls
    pico_btstack_ble
    pico_btstack_cyw43
    pico_mbedtls
    pico_rand
    pico_sha256
    pico_unique_id
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_i2c
    hardware_dma
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
    -Wconversion
    -Wsign-conversion
    -Wformat=2
    -Wnull-dereference
    -Wundef
    -Wmissing-prototypes
    -Wstrict-prototypes
    -Wold-style-definition
    -Wredundant-decls
    -Wnested-externs
    -Wcast-align=strict
    -Wwrite-strings
)

pico_enable_stdio_usb(logger_appliance 1)
pico_enable_stdio_uart(logger_appliance 0)

pico_add_extra_outputs(logger_appliance)
