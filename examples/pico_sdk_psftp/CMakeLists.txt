cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pimoroni_pico_plus2_w_rp2350" CACHE STRING "Pico SDK board")
endif()

if(DEFINED ENV{PICO_SDK_PATH})
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
    message(FATAL_ERROR "PICO_SDK_PATH environment variable must be set")
endif()

project(h10_btstack_psftp_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(POLAR_PROTO_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/polar_proto" CACHE PATH "Generated nanopb directory")

set(_polar_psftp_generated_files
    ${POLAR_PROTO_GENERATED_DIR}/types.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/types.pb.h
    ${POLAR_PROTO_GENERATED_DIR}/structures.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/structures.pb.h
    ${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.h
    ${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.h
    ${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.h
)

set(_polar_psftp_missing_generated "")
foreach(_generated_file IN LISTS _polar_psftp_generated_files)
    if(NOT EXISTS "${_generated_file}")
        list(APPEND _polar_psftp_missing_generated "${_generated_file}")
    endif()
endforeach()

if(_polar_psftp_missing_generated)
    string(REPLACE ";" "\n  " _polar_psftp_missing_generated_msg "${_polar_psftp_missing_generated}")
    message(FATAL_ERROR
        "Missing generated nanopb PSFTP files for probe.\n"
        "Run: ./polar_sdk/proto/generate_nanopb.sh\n"
        "Missing:\n  ${_polar_psftp_missing_generated_msg}\n")
endif()

add_executable(h10_btstack_psftp_probe
    main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_common.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_connect.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_transport.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_wait.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_security.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_runtime_context.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_transport_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_link.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_gatt.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_gatt_route.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_helpers.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_scan.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_adv_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_sm.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_sm_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_btstack_dispatch.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_apply.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_notify_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_query_complete.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_write.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_pmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_psftp.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_psftp_runtime.c
    ${POLAR_PROTO_GENERATED_DIR}/types.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/structures.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.c
    ${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.c
    ${CMAKE_CURRENT_LIST_DIR}/../../vendors/nanopb/pb_common.c
    ${CMAKE_CURRENT_LIST_DIR}/../../vendors/nanopb/pb_encode.c
    ${CMAKE_CURRENT_LIST_DIR}/../../vendors/nanopb/pb_decode.c
)

target_include_directories(h10_btstack_psftp_probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/include
    ${POLAR_PROTO_GENERATED_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../vendors/nanopb
)

target_link_libraries(h10_btstack_psftp_probe
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_ble
    pico_btstack_cyw43
)

set(H10_TARGET_ADDR "24:AC:AC:05:A3:10" CACHE STRING "Target H10 BLE address")
set(H10_MAX_ROUNDS "10" CACHE STRING "Maximum PSFTP rounds (0=infinite)")
set(H10_DOWNLOAD_MAX_BYTES "4096" CACHE STRING "Maximum bytes for download test")
set(H10_CLEAR_BOND_ON_BOOT "1" CACHE STRING "Clear target bond once on startup for repeatable pairing")
set(H10_PSFTP_TX_CHAR_MODE "0" CACHE STRING "PSFTP TX characteristic mode: 0=MTU, 1=H2D")
set(H10_PSFTP_WRITE_MODE "2" CACHE STRING "PSFTP write mode: 0=always with-response, 1=always without-response, 2=periodic with-response")
set(H10_PSFTP_WRITE_PERIOD_N "5" CACHE STRING "When write mode=2, every Nth packet uses with-response (0=never)")
set(H10_PSFTP_RAW_VALUE_TAP "1" CACHE STRING "Enable unfiltered GATT value tap logging (1/0)")
set(H10_PSFTP_TX_HEX_DUMP_BYTES "24" CACHE STRING "Max TX bytes printed in hex")
set(H10_PSFTP_RX_HEX_DUMP_BYTES "24" CACHE STRING "Max RX bytes printed in hex")
set(H10_PSFTP_TEST_PATH_LEN "1" CACHE STRING "1='/' normal; >1 uses synthetic long GET path length")
set(H10_PSFTP_PRE_TX_DELAY_MS "0" CACHE STRING "Optional delay between channel prep and first PSFTP TX")


target_compile_definitions(h10_btstack_psftp_probe PRIVATE
    H10_TARGET_ADDR="${H10_TARGET_ADDR}"
    H10_MAX_ROUNDS=${H10_MAX_ROUNDS}
    H10_DOWNLOAD_MAX_BYTES=${H10_DOWNLOAD_MAX_BYTES}
    H10_CLEAR_BOND_ON_BOOT=${H10_CLEAR_BOND_ON_BOOT}
    H10_PSFTP_TX_CHAR_MODE=${H10_PSFTP_TX_CHAR_MODE}
    H10_PSFTP_WRITE_MODE=${H10_PSFTP_WRITE_MODE}
    H10_PSFTP_WRITE_PERIOD_N=${H10_PSFTP_WRITE_PERIOD_N}
    H10_PSFTP_RAW_VALUE_TAP=${H10_PSFTP_RAW_VALUE_TAP}
    H10_PSFTP_TX_HEX_DUMP_BYTES=${H10_PSFTP_TX_HEX_DUMP_BYTES}
    H10_PSFTP_RX_HEX_DUMP_BYTES=${H10_PSFTP_RX_HEX_DUMP_BYTES}
    H10_PSFTP_TEST_PATH_LEN=${H10_PSFTP_TEST_PATH_LEN}
    H10_PSFTP_PRE_TX_DELAY_MS=${H10_PSFTP_PRE_TX_DELAY_MS}
    POLAR_CFG_ENABLE_PSFTP=1
)

pico_enable_stdio_usb(h10_btstack_psftp_probe 1)
pico_enable_stdio_uart(h10_btstack_psftp_probe 0)

pico_add_extra_outputs(h10_btstack_psftp_probe)
