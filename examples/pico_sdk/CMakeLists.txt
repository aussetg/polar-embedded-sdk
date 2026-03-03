cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pimoroni_pico_plus2_w_rp2350" CACHE STRING "Pico SDK board")
endif()

if(DEFINED ENV{PICO_SDK_PATH})
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
    message(FATAL_ERROR "PICO_SDK_PATH environment variable must be set")
endif()

project(h10_btstack_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(POLAR_ENSURE_BTSTACK_ALIGNMENT "ON" CACHE STRING "Run BTstack alignment + patch script during configure")
set(POLAR_BTSTACK_ALIGN_NO_FETCH "ON" CACHE STRING "Run alignment script with --no-fetch during configure")

if(POLAR_ENSURE_BTSTACK_ALIGNMENT)
    set(POLAR_BTSTACK_ALIGN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../../patches/align_btstack_trees.sh")
    if(NOT EXISTS "${POLAR_BTSTACK_ALIGN_SCRIPT}")
        message(FATAL_ERROR "BTstack alignment script not found: ${POLAR_BTSTACK_ALIGN_SCRIPT}")
    endif()

    set(_polar_btstack_align_args)
    if(POLAR_BTSTACK_ALIGN_NO_FETCH)
        list(APPEND _polar_btstack_align_args "--no-fetch")
    endif()

    message(STATUS "[h10-probe] ensuring BTstack alignment + patch stacks")
    execute_process(
        COMMAND "${POLAR_BTSTACK_ALIGN_SCRIPT}" ${_polar_btstack_align_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
        RESULT_VARIABLE _polar_btstack_align_rc
    )
    if(NOT _polar_btstack_align_rc EQUAL 0)
        message(FATAL_ERROR "BTstack alignment step failed (exit=${_polar_btstack_align_rc})")
    endif()
endif()

pico_sdk_init()

add_executable(h10_btstack_probe
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
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_orchestrator.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_dispatch.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_btstack_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_discovery_apply.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_notify_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_query_complete.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_write.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_gatt_mtu.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_hr.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_ecg.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_imu.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_pmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_pmd_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/src/polar_sdk_pmd_start.c
)

target_include_directories(h10_btstack_probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../polar_sdk/core/include
)

target_link_libraries(h10_btstack_probe
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_ble
    pico_btstack_cyw43
)

# Optional compile-time knobs (can be overridden with -D... at CMake configure time)
set(H10_TARGET_ADDR "24:AC:AC:05:A3:10" CACHE STRING "Target H10 BLE address")
set(H10_FORCE_PAIRING "0" CACHE STRING "Call sm_request_pairing() after connect")
set(H10_ENABLE_HR "1" CACHE STRING "Enable HR service discovery + notify")
set(H10_ENABLE_ECG_POLICY "1" CACHE STRING "Enable shared-sdk PMD ECG start policy run")
set(H10_POST_CONNECT_UPDATE "1" CACHE STRING "Use gap_update_connection_parameters after connect")
set(H10_POST_CONNECT_CONN_INTERVAL_MIN "24" CACHE STRING "Conn interval min (1.25ms units)")
set(H10_POST_CONNECT_CONN_INTERVAL_MAX "24" CACHE STRING "Conn interval max (1.25ms units)")
set(H10_POST_CONNECT_CONN_LATENCY "0" CACHE STRING "Slave latency")
set(H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS "72" CACHE STRING "Supervision timeout (10ms units)")

target_compile_definitions(h10_btstack_probe PRIVATE
    H10_TARGET_ADDR="${H10_TARGET_ADDR}"
    H10_FORCE_PAIRING=${H10_FORCE_PAIRING}
    H10_ENABLE_HR=${H10_ENABLE_HR}
    H10_ENABLE_ECG_POLICY=${H10_ENABLE_ECG_POLICY}
    H10_POST_CONNECT_UPDATE=${H10_POST_CONNECT_UPDATE}
    H10_POST_CONNECT_CONN_INTERVAL_MIN=${H10_POST_CONNECT_CONN_INTERVAL_MIN}
    H10_POST_CONNECT_CONN_INTERVAL_MAX=${H10_POST_CONNECT_CONN_INTERVAL_MAX}
    H10_POST_CONNECT_CONN_LATENCY=${H10_POST_CONNECT_CONN_LATENCY}
    H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS=${H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS}
)

pico_enable_stdio_usb(h10_btstack_probe 1)
pico_enable_stdio_uart(h10_btstack_probe 0)

pico_add_extra_outputs(h10_btstack_probe)
