# MicroPython user C module integration for the "polar_sdk" module.
#
# Usage example (rp2 port):
#   cmake -S vendors/micropython/ports/rp2 -B build/mpy-rp2 \
#     -DMICROPY_BOARD=RPI_PICO2_W \
#     -DUSER_C_MODULES=${CMAKE_CURRENT_LIST_DIR}
#
# Optional feature switches:
#   -DPOLAR_ENABLE_HR=ON|OFF
#   -DPOLAR_ENABLE_ECG=ON|OFF
#   -DPOLAR_ENABLE_PSFTP=ON|OFF
#   -DPOLAR_VERIFY_MICROPY_PATCHES=ON|OFF (default ON)
#   -DPOLAR_AUTO_APPLY_PATCHES=ON|OFF (default OFF)
#   -DPOLAR_BUILD_GIT_SHA=<sha> (default: unknown)
#   -DPOLAR_BUILD_GIT_DIRTY=0|1|unknown (default: unknown)
#   -DPOLAR_BUILD_PRESET=<name> (default: manual)
#   -DPOLAR_BUILD_TYPE=<type> (default: CMAKE_BUILD_TYPE or unknown)
#   -DPOLAR_PROTO_GENERATED_DIR=<path> (default: <repo>/build/polar_proto)

option(POLAR_ENABLE_HR "Enable Polar HR support" ON)
option(POLAR_ENABLE_ECG "Enable Polar ECG support" ON)
option(POLAR_ENABLE_PSFTP "Enable Polar PSFTP support" ON)

option(POLAR_VERIFY_MICROPY_PATCHES "Verify required local MicroPython patch series is present" ON)
option(POLAR_AUTO_APPLY_PATCHES "Auto-apply local MicroPython patch series during configure (mutates vendors/micropython)" OFF)

set(POLAR_BUILD_GIT_SHA "unknown" CACHE STRING "Firmware build metadata: git commit SHA")
set(POLAR_BUILD_GIT_DIRTY "unknown" CACHE STRING "Firmware build metadata: git dirty state (0/1/unknown)")
set(POLAR_BUILD_PRESET "manual" CACHE STRING "Firmware build metadata: preset/profile name")
set(POLAR_BUILD_TYPE "" CACHE STRING "Firmware build metadata: build type")

set(POLAR_ENABLE_HR_NUM 0)
if(POLAR_ENABLE_HR)
    set(POLAR_ENABLE_HR_NUM 1)
endif()

set(POLAR_ENABLE_ECG_NUM 0)
if(POLAR_ENABLE_ECG)
    set(POLAR_ENABLE_ECG_NUM 1)
endif()

set(POLAR_ENABLE_PSFTP_NUM 0)
if(POLAR_ENABLE_PSFTP)
    set(POLAR_ENABLE_PSFTP_NUM 1)
endif()

if(POLAR_BUILD_TYPE STREQUAL "")
    if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
        set(POLAR_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    else()
        set(POLAR_BUILD_TYPE "unknown")
    endif()
endif()

set(POLAR_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(POLAR_MICROPY_SUBMODULE_DIR "${POLAR_REPO_ROOT}/vendors/micropython")
set(POLAR_MICROPY_PATCH_DIR "${POLAR_REPO_ROOT}/patches/micropython")
set(POLAR_PATCH_APPLY_SCRIPT "${POLAR_REPO_ROOT}/patches/apply_micropython_patches.sh")

function(polar_patch_subject patch_path out_var)
    file(STRINGS "${patch_path}" _subject_line REGEX "^Subject: " LIMIT_COUNT 1)
    if(_subject_line STREQUAL "")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    string(REGEX REPLACE "^Subject: " "" _subject "${_subject_line}")
    string(REGEX REPLACE "^\\[PATCH[^]]*\\][ ]*" "" _subject "${_subject}")
    string(STRIP "${_subject}" _subject)
    set(${out_var} "${_subject}" PARENT_SCOPE)
endfunction()

if(POLAR_AUTO_APPLY_PATCHES)
    if(NOT EXISTS "${POLAR_PATCH_APPLY_SCRIPT}")
        message(FATAL_ERROR
            "POLAR_AUTO_APPLY_PATCHES=ON but patch apply script was not found: ${POLAR_PATCH_APPLY_SCRIPT}")
    endif()

    message(STATUS "[polar] POLAR_AUTO_APPLY_PATCHES=ON -> applying local MicroPython patch series")
    execute_process(
        COMMAND "${POLAR_PATCH_APPLY_SCRIPT}"
        WORKING_DIRECTORY "${POLAR_REPO_ROOT}"
        RESULT_VARIABLE _polar_patch_apply_rc
    )
    if(NOT _polar_patch_apply_rc EQUAL 0)
        message(FATAL_ERROR
            "Failed to auto-apply MicroPython patches (exit=${_polar_patch_apply_rc}).\n"
            "Run ./patches/apply_micropython_patches.sh manually, resolve conflicts, then reconfigure.")
    endif()
endif()

if(POLAR_VERIFY_MICROPY_PATCHES)
    if(NOT EXISTS "${POLAR_MICROPY_PATCH_DIR}")
        message(WARNING
            "[polar] Patch verification requested but patch directory was not found: ${POLAR_MICROPY_PATCH_DIR}. "
            "Skipping patch verification.")
    elseif(NOT EXISTS "${POLAR_MICROPY_SUBMODULE_DIR}/.git")
        message(WARNING
            "[polar] Patch verification requested but vendors/micropython checkout was not found: ${POLAR_MICROPY_SUBMODULE_DIR}. "
            "Skipping patch verification.")
    else()
        find_program(POLAR_GIT_EXECUTABLE git)
        if(NOT POLAR_GIT_EXECUTABLE)
            message(FATAL_ERROR
                "POLAR_VERIFY_MICROPY_PATCHES=ON requires git, but no git executable was found.")
        endif()

        file(GLOB _polar_patch_files "${POLAR_MICROPY_PATCH_DIR}/*.patch")
        if(_polar_patch_files STREQUAL "")
            message(WARNING
                "[polar] Patch verification requested but no .patch files were found in ${POLAR_MICROPY_PATCH_DIR}. "
                "Skipping patch verification.")
        else()
            list(SORT _polar_patch_files)
            set(_polar_missing_patch_subjects "")
            foreach(_patch_file IN LISTS _polar_patch_files)
                polar_patch_subject("${_patch_file}" _patch_subject)
                if(_patch_subject STREQUAL "")
                    message(FATAL_ERROR "Could not parse patch subject from ${_patch_file}")
                endif()

                execute_process(
                    COMMAND "${POLAR_GIT_EXECUTABLE}" -C "${POLAR_MICROPY_SUBMODULE_DIR}" log --fixed-strings --grep=${_patch_subject} --format=%s -n1
                    RESULT_VARIABLE _polar_git_log_rc
                    OUTPUT_VARIABLE _polar_git_log_subject
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(NOT _polar_git_log_rc EQUAL 0)
                    message(FATAL_ERROR
                        "Failed checking patch presence in ${POLAR_MICROPY_SUBMODULE_DIR} (exit=${_polar_git_log_rc}).")
                endif()

                if(_polar_git_log_subject STREQUAL "")
                    list(APPEND _polar_missing_patch_subjects "- ${_patch_subject}")
                endif()
            endforeach()

            if(_polar_missing_patch_subjects)
                string(JOIN "\n" _polar_missing_patch_subjects_msg ${_polar_missing_patch_subjects})
                message(FATAL_ERROR
                    "Missing required local MicroPython patches in vendors/micropython.\n\n"
                    "Run:\n"
                    "  ./patches/apply_micropython_patches.sh\n\n"
                    "Or opt in to configure-time mutation:\n"
                    "  -DPOLAR_AUTO_APPLY_PATCHES=ON\n\n"
                    "Missing patch subjects:\n"
                    "${_polar_missing_patch_subjects_msg}")
            else()
                message(STATUS "[polar] Verified local MicroPython patch series is present")
            endif()
        endif()
    endif()
endif()

set(POLAR_PROTO_GENERATED_DIR "${POLAR_REPO_ROOT}/build/polar_proto" CACHE PATH "Directory containing generated Polar nanopb sources")

set(POLAR_PSFTP_GENERATED_REQUIRED_FILES
    "${POLAR_PROTO_GENERATED_DIR}/types.pb.c"
    "${POLAR_PROTO_GENERATED_DIR}/types.pb.h"
    "${POLAR_PROTO_GENERATED_DIR}/structures.pb.c"
    "${POLAR_PROTO_GENERATED_DIR}/structures.pb.h"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.c"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.h"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.c"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.h"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.c"
    "${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.h")

if(POLAR_ENABLE_PSFTP)
    set(_polar_psftp_missing_generated "")
    foreach(_generated_file IN LISTS POLAR_PSFTP_GENERATED_REQUIRED_FILES)
        if(NOT EXISTS "${_generated_file}")
            list(APPEND _polar_psftp_missing_generated "${_generated_file}")
        endif()
    endforeach()

    if(_polar_psftp_missing_generated)
        string(REPLACE ";" "\n  " _polar_psftp_missing_generated_msg "${_polar_psftp_missing_generated}")
        message(FATAL_ERROR
            "POLAR_ENABLE_PSFTP=ON but required generated protobuf files were not found.\n\n"
            "Expected files under: ${POLAR_PROTO_GENERATED_DIR}\n\n"
            "Missing:\n  ${_polar_psftp_missing_generated_msg}\n\n"
            "Run:\n"
            "  ./polar_sdk/proto/generate_nanopb.sh <polar-sdk-proto-dir>\n")
    endif()
endif()

add_library(usermod_polar_sdk INTERFACE)

target_sources(usermod_polar_sdk INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_polar_sdk.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_common.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_connect.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_transport.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_wait.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_security.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_session.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_runtime_context.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_transport_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_link.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_gatt.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_gatt_route.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_helpers.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_scan.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_adv_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_security.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_sm.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_sm_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_btstack_dispatch.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery_orchestrator.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery_dispatch.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery_btstack_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_discovery_apply.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_gatt_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_gatt_notify_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_gatt_query_complete.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_gatt_write.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_gatt_mtu.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_hr.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_ecg.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_imu.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_pmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_pmd_control.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_pmd_start.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_psftp.c
    ${CMAKE_CURRENT_LIST_DIR}/../core/src/polar_sdk_psftp_runtime.c
    # Keep the MicroPython BTstack firmware aligned with the pico-sdk probe
    # builds used in this repo for H10 security-sensitive paths.
    # The local btstack config enables software AES + micro-ecc, so add the
    # matching implementation units here instead of relying on vendor defaults.
    ${POLAR_MICROPY_SUBMODULE_DIR}/lib/btstack/3rd-party/rijndael/rijndael.c
    ${POLAR_MICROPY_SUBMODULE_DIR}/lib/btstack/3rd-party/micro-ecc/uECC.c
)

if(POLAR_ENABLE_PSFTP)
    target_sources(usermod_polar_sdk INTERFACE
        ${POLAR_PROTO_GENERATED_DIR}/types.pb.c
        ${POLAR_PROTO_GENERATED_DIR}/structures.pb.c
        ${POLAR_PROTO_GENERATED_DIR}/pftp_error.pb.c
        ${POLAR_PROTO_GENERATED_DIR}/pftp_request.pb.c
        ${POLAR_PROTO_GENERATED_DIR}/pftp_response.pb.c
        ${POLAR_REPO_ROOT}/vendors/nanopb/pb_common.c
        ${POLAR_REPO_ROOT}/vendors/nanopb/pb_decode.c
        ${POLAR_REPO_ROOT}/vendors/nanopb/pb_encode.c
    )
endif()

target_include_directories(usermod_polar_sdk INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../core/include
    ${POLAR_MICROPY_SUBMODULE_DIR}/lib/btstack/3rd-party/rijndael
    ${POLAR_MICROPY_SUBMODULE_DIR}/lib/btstack/3rd-party/micro-ecc
)

if(POLAR_ENABLE_PSFTP)
    target_include_directories(usermod_polar_sdk INTERFACE
        ${POLAR_PROTO_GENERATED_DIR}
        ${POLAR_REPO_ROOT}/vendors/nanopb
    )
endif()

target_compile_definitions(usermod_polar_sdk INTERFACE
    POLAR_CFG_ENABLE_HR=${POLAR_ENABLE_HR_NUM}
    POLAR_CFG_ENABLE_ECG=${POLAR_ENABLE_ECG_NUM}
    POLAR_CFG_ENABLE_PSFTP=${POLAR_ENABLE_PSFTP_NUM}
    POLAR_BUILD_GIT_SHA=\"${POLAR_BUILD_GIT_SHA}\"
    POLAR_BUILD_GIT_DIRTY=\"${POLAR_BUILD_GIT_DIRTY}\"
    POLAR_BUILD_PRESET=\"${POLAR_BUILD_PRESET}\"
    POLAR_BUILD_TYPE=\"${POLAR_BUILD_TYPE}\"
)

target_compile_options(usermod_polar_sdk INTERFACE
    -UMICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE
    -DMICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/btstack_inc/btstack_config.h\"
    # The rp2 port otherwise injects its own immediate post-connect parameter
    # update for every central connection (24/24/0/72). Our Polar transport
    # layer already performs its own deterministic post-connect update aligned
    # with the working pico-sdk probes, so disable the generic MicroPython one
    # to avoid a competing early HCI update on the same link.
    -UMICROPY_PY_BLUETOOTH_BTSTACK_CENTRAL_POST_CONNECT_PARAM_UPDATE
    -DMICROPY_PY_BLUETOOTH_BTSTACK_CENTRAL_POST_CONNECT_PARAM_UPDATE=0
)

# Link our INTERFACE library to the usermod target (provided by MicroPython build).
# The target name "usermod" is defined by MicroPython when USER_C_MODULES is used.
target_link_libraries(usermod INTERFACE usermod_polar_sdk)
