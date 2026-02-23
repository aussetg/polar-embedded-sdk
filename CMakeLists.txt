cmake_minimum_required(VERSION 3.13)

project(polar_ble_firmware_superbuild NONE)

set(POLAR_BLE_FW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/vendors/micropython/ports/rp2")
set(POLAR_BLE_FW_BUILD_DIR "${CMAKE_BINARY_DIR}/rp2" CACHE PATH "Inner rp2 firmware build directory")

set(MICROPY_BOARD "RPI_PICO2_W" CACHE STRING "MicroPython board")
set(MICROPY_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/vendors/micropython/ports/rp2/boards/${MICROPY_BOARD}" CACHE PATH "MicroPython board directory")
set(USER_C_MODULES "" CACHE STRING "Semicolon-separated list of USER_C_MODULES entries")
set(PICO_BUILD_DOCS "0" CACHE STRING "Disable Pico SDK docs in firmware builds")

set(POLAR_BLE_ENABLE_HR "ON" CACHE STRING "Enable Polar HR feature")
set(POLAR_BLE_ENABLE_ECG "ON" CACHE STRING "Enable Polar ECG feature")
set(POLAR_BLE_ENABLE_PSFTP "ON" CACHE STRING "Enable Polar PSFTP feature")
set(POLAR_BLE_VERIFY_MICROPY_PATCHES "ON" CACHE STRING "Verify local MicroPython patch series")
set(POLAR_BLE_AUTO_APPLY_PATCHES "OFF" CACHE STRING "Auto-apply patch series during configure")

set(POLAR_BLE_BUILD_PRESET_NAME "manual" CACHE STRING "Human-readable preset/build profile name")
set(POLAR_BLE_ARTIFACTS_PROFILE "${POLAR_BLE_BUILD_PRESET_NAME}" CACHE STRING "Artifact profile directory name")
set(POLAR_BLE_ARTIFACT_DIR "${CMAKE_CURRENT_LIST_DIR}/build/artifacts/${POLAR_BLE_ARTIFACTS_PROFILE}" CACHE PATH "Stable firmware artifact output directory")

if(USER_C_MODULES STREQUAL "")
    message(FATAL_ERROR "USER_C_MODULES must be set (for example: ${CMAKE_CURRENT_LIST_DIR}/polar_ble/mpy)")
endif()

# Build metadata (can be overridden by cache vars if needed).
if(NOT DEFINED POLAR_BLE_BUILD_GIT_SHA OR POLAR_BLE_BUILD_GIT_SHA STREQUAL "")
    set(_polar_ble_git_sha "unknown")
    execute_process(
        COMMAND git rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        RESULT_VARIABLE _polar_ble_git_sha_rc
        OUTPUT_VARIABLE _polar_ble_git_sha_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_polar_ble_git_sha_rc EQUAL 0 AND NOT _polar_ble_git_sha_out STREQUAL "")
        set(_polar_ble_git_sha "${_polar_ble_git_sha_out}")
    endif()
    set(POLAR_BLE_BUILD_GIT_SHA "${_polar_ble_git_sha}" CACHE STRING "Git commit SHA for firmware metadata" FORCE)
endif()

if(NOT DEFINED POLAR_BLE_BUILD_GIT_DIRTY OR POLAR_BLE_BUILD_GIT_DIRTY STREQUAL "")
    set(_polar_ble_git_dirty "unknown")
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        RESULT_VARIABLE _polar_ble_git_dirty_rc
        OUTPUT_VARIABLE _polar_ble_git_dirty_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_polar_ble_git_dirty_rc EQUAL 0)
        if(_polar_ble_git_dirty_out STREQUAL "")
            set(_polar_ble_git_dirty "0")
        else()
            set(_polar_ble_git_dirty "1")
        endif()
    endif()
    set(POLAR_BLE_BUILD_GIT_DIRTY "${_polar_ble_git_dirty}" CACHE STRING "Git dirty state for firmware metadata" FORCE)
endif()

if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
    set(POLAR_BLE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
    set(POLAR_BLE_BUILD_TYPE "MinSizeRel")
endif()

string(REPLACE ";" "\\;" _polar_ble_user_c_modules_escaped "${USER_C_MODULES}")

set(_polar_ble_fw_cmake_args
    "-DMICROPY_BOARD=${MICROPY_BOARD}"
    "-DMICROPY_BOARD_DIR=${MICROPY_BOARD_DIR}"
    "-DUSER_C_MODULES=${_polar_ble_user_c_modules_escaped}"
    "-DPICO_BUILD_DOCS=${PICO_BUILD_DOCS}"
    "-DPOLAR_BLE_ENABLE_HR=${POLAR_BLE_ENABLE_HR}"
    "-DPOLAR_BLE_ENABLE_ECG=${POLAR_BLE_ENABLE_ECG}"
    "-DPOLAR_BLE_ENABLE_PSFTP=${POLAR_BLE_ENABLE_PSFTP}"
    "-DPOLAR_BLE_VERIFY_MICROPY_PATCHES=${POLAR_BLE_VERIFY_MICROPY_PATCHES}"
    "-DPOLAR_BLE_AUTO_APPLY_PATCHES=${POLAR_BLE_AUTO_APPLY_PATCHES}"
    "-DPOLAR_BLE_BUILD_GIT_SHA=${POLAR_BLE_BUILD_GIT_SHA}"
    "-DPOLAR_BLE_BUILD_GIT_DIRTY=${POLAR_BLE_BUILD_GIT_DIRTY}"
    "-DPOLAR_BLE_BUILD_PRESET=${POLAR_BLE_BUILD_PRESET_NAME}"
    "-DPOLAR_BLE_BUILD_TYPE=${POLAR_BLE_BUILD_TYPE}"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}"
)

if(DEFINED MICROPY_C_HEAP_SIZE)
    list(APPEND _polar_ble_fw_cmake_args "-DMICROPY_C_HEAP_SIZE=${MICROPY_C_HEAP_SIZE}")
endif()

if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
    list(APPEND _polar_ble_fw_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "[polar-ble-fw] configuring inner rp2 firmware build")
message(STATUS "[polar-ble-fw] source: ${POLAR_BLE_FW_SOURCE_DIR}")
message(STATUS "[polar-ble-fw] binary: ${POLAR_BLE_FW_BUILD_DIR}")
message(STATUS "[polar-ble-fw] artifact dir: ${POLAR_BLE_ARTIFACT_DIR}")
message(STATUS "[polar-ble-fw] build metadata: sha=${POLAR_BLE_BUILD_GIT_SHA}, dirty=${POLAR_BLE_BUILD_GIT_DIRTY}, preset=${POLAR_BLE_BUILD_PRESET_NAME}, type=${POLAR_BLE_BUILD_TYPE}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${POLAR_BLE_FW_SOURCE_DIR}" -B "${POLAR_BLE_FW_BUILD_DIR}" ${_polar_ble_fw_cmake_args}
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE _polar_ble_fw_cfg_rc
)
if(NOT _polar_ble_fw_cfg_rc EQUAL 0)
    message(FATAL_ERROR "Inner rp2 firmware configure failed (exit=${_polar_ble_fw_cfg_rc})")
endif()

set(POLAR_BLE_FW_ELF "${POLAR_BLE_FW_BUILD_DIR}/firmware.elf")
set(POLAR_BLE_FW_UF2 "${POLAR_BLE_FW_BUILD_DIR}/firmware.uf2")
set(POLAR_BLE_META_SOURCE "${CMAKE_BINARY_DIR}/build_info.txt")

file(WRITE "${POLAR_BLE_META_SOURCE}"
    "preset=${POLAR_BLE_BUILD_PRESET_NAME}\n"
    "build_type=${POLAR_BLE_BUILD_TYPE}\n"
    "git_sha=${POLAR_BLE_BUILD_GIT_SHA}\n"
    "git_dirty=${POLAR_BLE_BUILD_GIT_DIRTY}\n"
    "inner_build_dir=${POLAR_BLE_FW_BUILD_DIR}\n")

add_custom_target(firmware ALL
    COMMAND "${CMAKE_COMMAND}" --build "${POLAR_BLE_FW_BUILD_DIR}" --parallel
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${POLAR_BLE_ARTIFACT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${POLAR_BLE_FW_ELF}" "${POLAR_BLE_ARTIFACT_DIR}/firmware.elf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${POLAR_BLE_FW_UF2}" "${POLAR_BLE_ARTIFACT_DIR}/firmware.uf2"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${POLAR_BLE_META_SOURCE}" "${POLAR_BLE_ARTIFACT_DIR}/build_info.txt"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    USES_TERMINAL
    COMMENT "Building rp2 firmware in ${POLAR_BLE_FW_BUILD_DIR} and exporting stable artifacts"
)
