// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
// MicroPython user C module: polar_sdk
//
// Current implementation status:
// - skeleton: lifecycle + state()/stats() + error types + feature flags
// - transport core:
//   - scan filtering (addr or name prefix)
//   - connect with timeout and deterministic backoff retries
//   - GATT discovery + handle cache for HR/PMD/PSFTP
//   - disconnect reason/counter tracking
// - HR service:
//   - start/stop HR notifications
//   - parse 0x2A37 into fixed-width sample tuple
//   - read_hr(timeout_ms=...)
// - ECG support:
//   - PMD start/stop ECG control path
//   - PMD ECG frame-type-0 parsing to packed int32 ring buffer
//   - read_ecg(max_bytes=..., timeout_ms=...)
// - ACC support (H10 ACC raw frame type 0x01):
//   - PMD start/stop ACC control path
//   - read_acc(max_bytes=..., timeout_ms=...)
//
// Notes:
// - BLE implementation targets MicroPython rp2 + BTstack central mode.
// - Only a single active transport instance is supported at a time.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "polar_sdk_common.h"
#include "polar_sdk_session.h"
#include "polar_sdk_connect.h"
#include "polar_sdk_discovery.h"
#include "polar_sdk_discovery_orchestrator.h"
#include "polar_sdk_discovery_dispatch.h"
#include "polar_sdk_discovery_runtime.h"
#include "polar_sdk_discovery_btstack_runtime.h"
#include "polar_sdk_discovery_apply.h"
#include "polar_sdk_gatt_control.h"
#include "polar_sdk_gatt_notify_runtime.h"
#include "polar_sdk_gatt_query_complete.h"
#include "polar_sdk_gatt_write.h"
#include "polar_sdk_gatt_mtu.h"
#include "polar_sdk_security.h"
#include "polar_sdk_ecg.h"
#include "polar_sdk_imu.h"
#include "polar_sdk_runtime.h"
#include "polar_sdk_runtime_context.h"
#include "polar_sdk_transport.h"
#include "polar_sdk_wait.h"
#include "polar_sdk_transport_adapter.h"
#include "polar_sdk_btstack_link.h"
#include "polar_sdk_btstack_gatt.h"
#include "polar_sdk_btstack_gatt_route.h"
#include "polar_sdk_btstack_helpers.h"
#include "polar_sdk_btstack_scan.h"
#include "polar_sdk_btstack_adv_runtime.h"
#include "polar_sdk_btstack_sm.h"
#include "polar_sdk_sm_control.h"
#include "polar_sdk_btstack_dispatch.h"
#include "polar_sdk_hr.h"
#include "polar_sdk_pmd.h"
#include "polar_sdk_pmd_control.h"
#include "polar_sdk_psftp.h"
#include "polar_sdk_psftp_runtime.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objexcept.h"
#include "py/objstr.h"
#include "py/runtime.h"

// Feature flags (normally provided by the build system via CMake).
// Defaults are ON for local/dev builds.
#ifndef POLAR_CFG_ENABLE_HR
#define POLAR_CFG_ENABLE_HR (1)
#endif
#ifndef POLAR_CFG_ENABLE_ECG
#define POLAR_CFG_ENABLE_ECG (1)
#endif
#ifndef POLAR_CFG_ENABLE_PSFTP
#define POLAR_CFG_ENABLE_PSFTP (0)
#endif

// Build metadata (normally injected by CMake).
#ifndef POLAR_BUILD_GIT_SHA
#define POLAR_BUILD_GIT_SHA "unknown"
#endif
#ifndef POLAR_BUILD_GIT_DIRTY
#define POLAR_BUILD_GIT_DIRTY "unknown"
#endif
#ifndef POLAR_BUILD_PRESET
#define POLAR_BUILD_PRESET "manual"
#endif
#ifndef POLAR_BUILD_TYPE
#define POLAR_BUILD_TYPE "unknown"
#endif

#if POLAR_CFG_ENABLE_PSFTP
#include "pftp_error.pb.h"
#endif

#if MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BTSTACK
#define POLAR_SDK_HAS_BTSTACK (1)
#include "extmod/modbluetooth.h"
#include "extmod/btstack/modbluetooth_btstack.h"
#include "btstack.h"
#include "pico/unique_id.h"
#else
#define POLAR_SDK_HAS_BTSTACK (0)
#endif

#define POLAR_SDK_SERVICE_HR (0x01)
#define POLAR_SDK_SERVICE_ECG (0x02)
#define POLAR_SDK_SERVICE_PSFTP (0x04)
#define POLAR_SDK_SERVICE_ALL (POLAR_SDK_SERVICE_HR | POLAR_SDK_SERVICE_ECG | POLAR_SDK_SERVICE_PSFTP)
#define POLAR_SDK_CAP_STREAM_PMD_MASK (POLAR_CAP_STREAM_ECG | POLAR_CAP_STREAM_ACC | POLAR_CAP_STREAM_PPG | POLAR_CAP_STREAM_PPI | POLAR_CAP_STREAM_GYRO | POLAR_CAP_STREAM_MAG)

// Conservative defaults for connect scan and retry behavior.
#define POLAR_SDK_SCAN_INTERVAL_UNITS (0x0030) // 30 ms
#define POLAR_SDK_SCAN_WINDOW_UNITS   (0x0030) // 30 ms
#define POLAR_SDK_CONNECT_ATTEMPT_SLICE_MS (3500)
#define POLAR_SDK_DISCONNECT_WAIT_MS (2500)
#define POLAR_SDK_GATT_OP_TIMEOUT_MS (2000)

#define POLAR_SDK_POST_CONN_INTERVAL_MIN (24)
#define POLAR_SDK_POST_CONN_INTERVAL_MAX (40)
#define POLAR_SDK_POST_CONN_LATENCY (0)
#define POLAR_SDK_POST_CONN_SUPERVISION_TIMEOUT_10MS (600)

#define POLAR_SDK_ECG_RING_BYTES (4096)
#define POLAR_SDK_IMU_RING_BYTES (4096)

#define POLAR_SDK_PMD_OP_REQUEST_MEASUREMENT_START (POLAR_SDK_PMD_OPCODE_START_MEASUREMENT)
#define POLAR_SDK_PMD_OP_STOP_MEASUREMENT (POLAR_SDK_PMD_OPCODE_STOP_MEASUREMENT)
#define POLAR_SDK_PMD_OP_GET_MEASUREMENT_SETTINGS (0x01)
#define POLAR_SDK_PMD_OP_GET_MEASUREMENT_STATUS (0x05)
#define POLAR_SDK_PMD_MEAS_ECG (POLAR_SDK_PMD_MEASUREMENT_ECG)
#define POLAR_SDK_PMD_MEAS_ACC (POLAR_SDK_PMD_MEASUREMENT_ACC)
#define POLAR_SDK_PMD_MEAS_OFFLINE_HR (0x0Eu)

#define POLAR_SDK_PMD_MIN_MTU (70)
#define POLAR_SDK_PMD_SECURITY_ROUNDS (3)
#define POLAR_SDK_PMD_SECURITY_WAIT_MS (3500)
#define POLAR_SDK_PMD_CCC_ATTEMPTS (4)
#define POLAR_SDK_IMU_DEFAULT_RESOLUTION (16)
#define POLAR_SDK_IMU_DEFAULT_RANGE_G (8)

#define POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS (12000)
#define POLAR_SDK_PSFTP_GATT_OP_TIMEOUT_MS (6000)
#define POLAR_SDK_PSFTP_RECONNECT_TIMEOUT_MS (12000)
#define POLAR_SDK_PSFTP_DEFAULT_DOWNLOAD_MAX_BYTES (8192u)
#define POLAR_SDK_PSFTP_MAX_DOWNLOAD_BYTES (32768u)
#define POLAR_SDK_PSFTP_MAX_DIR_ENTRIES (48u)
#define POLAR_SDK_PSFTP_MAX_DIR_RESPONSE_BYTES (8192u)

#define POLAR_SDK_STATE_IDLE (POLAR_SDK_RUNTIME_STATE_IDLE)
#define POLAR_SDK_STATE_SCANNING (POLAR_SDK_RUNTIME_STATE_SCANNING)
#define POLAR_SDK_STATE_CONNECTING (POLAR_SDK_RUNTIME_STATE_CONNECTING)
#define POLAR_SDK_STATE_DISCOVERING (POLAR_SDK_RUNTIME_STATE_DISCOVERING)
#define POLAR_SDK_STATE_READY (POLAR_SDK_RUNTIME_STATE_READY)
#define POLAR_SDK_STATE_RECOVERING (POLAR_SDK_RUNTIME_STATE_RECOVERING)

static const char *const polar_state_strs[] = {
    "idle",
    "scanning",
    "connecting",
    "discovering",
    "ready",
    "recovering",
};

#if POLAR_SDK_HAS_BTSTACK
static void polar_fill_btstack_host_key(sm_key_t out_key, uint8_t salt_a, uint8_t salt_b) {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    for (size_t i = 0; i < sizeof(sm_key_t); ++i) {
        uint8_t src = board_id.id[i % PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
        uint8_t mix = (uint8_t)(src ^ (uint8_t)(salt_a + (uint8_t)(i * 29u)) ^ (uint8_t)(salt_b * (uint8_t)(i + 1u)));
        if (mix == 0) {
            mix = (uint8_t)(0x5Au ^ salt_a ^ (uint8_t)i);
        }
        out_key[i] = mix;
    }
}

static void polar_apply_btstack_host_identity_keys(void) {
    // MicroPython's BTstack init path overwrites BTstack's built-in default ER/IR
    // values with all-zero dummy keys to silence warnings. For our central-only
    // H10 PSFTP flows that is a bad trade: it removes a stable non-zero local
    // identity basis and can destabilize pairing/bonding behavior.
    //
    // Restore deterministic per-board non-zero keys after stack init without
    // editing vendors/. These keys are local host identity material for the
    // BTstack Security Manager, not user-facing API state.
    sm_key_t er;
    sm_key_t ir;
    polar_fill_btstack_host_key(er, 0x41u, 0x97u);
    polar_fill_btstack_host_key(ir, 0xA5u, 0x2Fu);
    sm_set_er(er);
    sm_set_ir(ir);
}
#endif

static bool polar_service_mask_is_valid(uint32_t mask) {
    uint32_t allowed = POLAR_SDK_SERVICE_ALL;
    if (!POLAR_CFG_ENABLE_HR) {
        allowed &= ~POLAR_SDK_SERVICE_HR;
    }
    if (!POLAR_CFG_ENABLE_ECG) {
        allowed &= ~POLAR_SDK_SERVICE_ECG;
    }
    if (!POLAR_CFG_ENABLE_PSFTP) {
        allowed &= ~POLAR_SDK_SERVICE_PSFTP;
    }
    return polar_sdk_service_mask_is_valid(mask, allowed);
}

static polar_capability_mask_t polar_build_available_capabilities_mask(void) {
    polar_capability_mask_t mask = 0;
    if (POLAR_CFG_ENABLE_HR) {
        mask |= POLAR_CAP_STREAM_HR;
    }
    if (POLAR_CFG_ENABLE_ECG) {
        mask |= POLAR_CAP_STREAM_ECG;
        mask |= POLAR_CAP_STREAM_ACC;
    }
    if (POLAR_CFG_ENABLE_PSFTP) {
        mask |= POLAR_CAP_PSFTP_READ;
        mask |= POLAR_CAP_PSFTP_DELETE;
        if (POLAR_CFG_ENABLE_HR) {
            mask |= POLAR_CAP_RECORDING;
        }
    }
    return mask;
}

static uint8_t polar_capability_mask_to_service_mask(polar_capability_mask_t mask) {
    uint8_t service_mask = 0;
    if (mask & POLAR_CAP_STREAM_HR) {
        service_mask |= POLAR_SDK_SERVICE_HR;
    }
    if (mask & POLAR_SDK_CAP_STREAM_PMD_MASK) {
        service_mask |= POLAR_SDK_SERVICE_ECG;
    }
    if (mask & (POLAR_CAP_RECORDING | POLAR_CAP_PSFTP_READ | POLAR_CAP_PSFTP_DELETE)) {
        service_mask |= POLAR_SDK_SERVICE_PSFTP;
    }
    return service_mask;
}

static uint8_t polar_build_default_services_mask(void) {
    return polar_capability_mask_to_service_mask(polar_build_available_capabilities_mask());
}

static bool polar_capability_name_to_bit(const char *name, size_t len, polar_capability_mask_t *out_bit) {
    if (len == 9 && memcmp(name, "stream:hr", 9) == 0) {
        *out_bit = POLAR_CAP_STREAM_HR;
        return true;
    }
    if (len == 10 && memcmp(name, "stream:ecg", 10) == 0) {
        *out_bit = POLAR_CAP_STREAM_ECG;
        return true;
    }
    if (len == 10 && memcmp(name, "stream:acc", 10) == 0) {
        *out_bit = POLAR_CAP_STREAM_ACC;
        return true;
    }
    if (len == 10 && memcmp(name, "stream:ppg", 10) == 0) {
        *out_bit = POLAR_CAP_STREAM_PPG;
        return true;
    }
    if (len == 10 && memcmp(name, "stream:ppi", 10) == 0) {
        *out_bit = POLAR_CAP_STREAM_PPI;
        return true;
    }
    if (len == 11 && memcmp(name, "stream:gyro", 11) == 0) {
        *out_bit = POLAR_CAP_STREAM_GYRO;
        return true;
    }
    if (len == 10 && memcmp(name, "stream:mag", 10) == 0) {
        *out_bit = POLAR_CAP_STREAM_MAG;
        return true;
    }
    if (len == 9 && memcmp(name, "recording", 9) == 0) {
        *out_bit = POLAR_CAP_RECORDING;
        return true;
    }
    if (len == 10 && memcmp(name, "psftp:read", 10) == 0) {
        *out_bit = POLAR_CAP_PSFTP_READ;
        return true;
    }
    if (len == 12 && memcmp(name, "psftp:delete", 12) == 0) {
        *out_bit = POLAR_CAP_PSFTP_DELETE;
        return true;
    }
    return false;
}

static bool polar_required_capabilities_parse(
    mp_obj_t caps_obj,
    polar_capability_mask_t *out_caps,
    uint8_t *out_services_mask) {
    polar_capability_mask_t caps = 0;

    if (caps_obj == mp_const_none) {
        caps = 0;
    } else {
        if (!mp_obj_is_type(caps_obj, &mp_type_tuple) && !mp_obj_is_type(caps_obj, &mp_type_list)) {
            mp_raise_TypeError(MP_ERROR_TEXT("required_capabilities must be tuple/list or None"));
        }

        mp_obj_t *items = NULL;
        size_t n_items = 0;
        mp_obj_get_array(caps_obj, &n_items, &items);
        for (size_t i = 0; i < n_items; i++) {
            if (!mp_obj_is_str(items[i])) {
                mp_raise_TypeError(MP_ERROR_TEXT("required_capabilities entries must be str"));
            }
            size_t len = 0;
            const char *name = mp_obj_str_get_data(items[i], &len);
            polar_capability_mask_t bit = 0;
            if (!polar_capability_name_to_bit(name, len, &bit)) {
                mp_raise_ValueError(MP_ERROR_TEXT("unknown capability"));
            }
            caps |= bit;
        }
    }

    if ((caps & ~polar_build_available_capabilities_mask()) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("required capabilities unavailable in this build"));
    }

    uint8_t services_mask = polar_capability_mask_to_service_mask(caps);
    if (!polar_service_mask_is_valid(services_mask)) {
        mp_raise_ValueError(MP_ERROR_TEXT("required capabilities unavailable in this build"));
    }

    *out_caps = caps;
    *out_services_mask = services_mask;
    return true;
}

static mp_obj_t polar_capabilities_tuple_from_mask(polar_capability_mask_t mask) {
    mp_obj_t items[10];
    size_t n = 0;
    if (mask & POLAR_CAP_STREAM_HR) {
        items[n++] = mp_obj_new_str("stream:hr", 9);
    }
    if (mask & POLAR_CAP_STREAM_ECG) {
        items[n++] = mp_obj_new_str("stream:ecg", 10);
    }
    if (mask & POLAR_CAP_STREAM_ACC) {
        items[n++] = mp_obj_new_str("stream:acc", 10);
    }
    if (mask & POLAR_CAP_STREAM_PPG) {
        items[n++] = mp_obj_new_str("stream:ppg", 10);
    }
    if (mask & POLAR_CAP_STREAM_PPI) {
        items[n++] = mp_obj_new_str("stream:ppi", 10);
    }
    if (mask & POLAR_CAP_STREAM_GYRO) {
        items[n++] = mp_obj_new_str("stream:gyro", 11);
    }
    if (mask & POLAR_CAP_STREAM_MAG) {
        items[n++] = mp_obj_new_str("stream:mag", 10);
    }
    if (mask & POLAR_CAP_RECORDING) {
        items[n++] = mp_obj_new_str("recording", 9);
    }
    if (mask & POLAR_CAP_PSFTP_READ) {
        items[n++] = mp_obj_new_str("psftp:read", 10);
    }
    if (mask & POLAR_CAP_PSFTP_DELETE) {
        items[n++] = mp_obj_new_str("psftp:delete", 12);
    }
    return mp_obj_new_tuple(n, items);
}

typedef enum {
    POLAR_STREAM_KIND_KEY_INVALID = -1,
    POLAR_STREAM_KIND_KEY_HR = 0,
    POLAR_STREAM_KIND_KEY_ECG,
    POLAR_STREAM_KIND_KEY_ACC,
    POLAR_STREAM_KIND_KEY_PPG,
    POLAR_STREAM_KIND_KEY_PPI,
    POLAR_STREAM_KIND_KEY_GYRO,
    POLAR_STREAM_KIND_KEY_MAG,
} polar_stream_kind_key_t;

static polar_stream_kind_key_t polar_parse_stream_kind_key(mp_obj_t kind_obj) {
    if (!mp_obj_is_str(kind_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("kind must be str"));
    }

    size_t len = 0;
    const char *kind = mp_obj_str_get_data(kind_obj, &len);
    if (len == 2 && memcmp(kind, "hr", 2) == 0) {
        return POLAR_STREAM_KIND_KEY_HR;
    }
    if (len == 3 && memcmp(kind, "ecg", 3) == 0) {
        return POLAR_STREAM_KIND_KEY_ECG;
    }
    if (len == 3 && memcmp(kind, "acc", 3) == 0) {
        return POLAR_STREAM_KIND_KEY_ACC;
    }
    if (len == 3 && memcmp(kind, "ppg", 3) == 0) {
        return POLAR_STREAM_KIND_KEY_PPG;
    }
    if (len == 3 && memcmp(kind, "ppi", 3) == 0) {
        return POLAR_STREAM_KIND_KEY_PPI;
    }
    if (len == 4 && memcmp(kind, "gyro", 4) == 0) {
        return POLAR_STREAM_KIND_KEY_GYRO;
    }
    if (len == 3 && memcmp(kind, "mag", 3) == 0) {
        return POLAR_STREAM_KIND_KEY_MAG;
    }
    return POLAR_STREAM_KIND_KEY_INVALID;
}

static polar_stream_format_t polar_default_stream_format_for_kind(polar_stream_kind_key_t kind) {
    (void)kind;
    return POLAR_STREAM_FORMAT_DECODED;
}

static polar_stream_format_t polar_parse_stream_format_or_default(
    mp_obj_t format_obj,
    polar_stream_format_t default_format) {
    if (format_obj == mp_const_none) {
        return default_format;
    }
    if (!mp_obj_is_str(format_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("format must be str or None"));
    }

    size_t len = 0;
    const char *fmt = mp_obj_str_get_data(format_obj, &len);
    if (len == 3 && memcmp(fmt, "raw", 3) == 0) {
        return POLAR_STREAM_FORMAT_RAW;
    }
    if (len == 7 && memcmp(fmt, "decoded", 7) == 0) {
        return POLAR_STREAM_FORMAT_DECODED;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid format"));
}

static inline void polar_store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
    dst[2] = (uint8_t)((value >> 16) & 0xffu);
    dst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static inline void polar_store_u64_le(uint8_t *dst, uint64_t value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
    dst[2] = (uint8_t)((value >> 16) & 0xffu);
    dst[3] = (uint8_t)((value >> 24) & 0xffu);
    dst[4] = (uint8_t)((value >> 32) & 0xffu);
    dst[5] = (uint8_t)((value >> 40) & 0xffu);
    dst[6] = (uint8_t)((value >> 48) & 0xffu);
    dst[7] = (uint8_t)((value >> 56) & 0xffu);
}

static inline void polar_store_i32_le(uint8_t *dst, int32_t value) {
    polar_store_u32_le(dst, (uint32_t)value);
}

static inline void polar_store_i64_le(uint8_t *dst, int64_t value) {
    polar_store_u64_le(dst, (uint64_t)value);
}

static mp_obj_t polar_build_decoded_chunk_v1(
    uint8_t kind_code,
    uint8_t unit_code,
    uint8_t time_base_code,
    uint8_t flags,
    int64_t t0_ns,
    int32_t dt_ns,
    uint32_t sample_size_bytes,
    const uint8_t *payload,
    size_t payload_len) {
    const size_t header_len = 28;
    const size_t total_len = header_len + payload_len;
    uint8_t *tmp = m_new(uint8_t, total_len);

    tmp[0] = 1; // version
    tmp[1] = kind_code;
    tmp[2] = unit_code;
    tmp[3] = time_base_code;
    tmp[4] = flags;
    tmp[5] = 0;
    tmp[6] = 0;
    tmp[7] = 0;
    polar_store_u32_le(&tmp[8], sample_size_bytes == 0 ? 0u : (uint32_t)(payload_len / sample_size_bytes));
    polar_store_i64_le(&tmp[12], (flags & 0x01u) != 0 ? t0_ns : 0);
    polar_store_i32_le(&tmp[20], (flags & 0x02u) != 0 ? dt_ns : 0);
    polar_store_u32_le(&tmp[24], sample_size_bytes);

    if (payload_len > 0) {
        memcpy(&tmp[28], payload, payload_len);
    }

    mp_obj_t out = mp_obj_new_bytes(tmp, total_len);
    m_del(uint8_t, tmp, total_len);
    return out;
}

static uint8_t polar_hr_public_flags(const polar_sdk_hr_state_t *state) {
    uint8_t flags = 0;
    if (state->last_contact != 0) {
        flags |= 0x01u;
    }
    if ((state->last_flags & 0x04u) != 0) {
        flags |= 0x02u;
    }
    return flags;
}

static mp_obj_t polar_build_decoded_hr_chunk(const polar_sdk_hr_state_t *state) {
    uint8_t payload[12] = {0};
    uint16_t bpm = state->last_bpm;
    payload[0] = (uint8_t)(bpm & 0xffu);
    payload[1] = (uint8_t)((bpm >> 8) & 0xffu);
    payload[2] = polar_hr_public_flags(state);
    payload[3] = state->last_rr_count;
    for (size_t i = 0; i < 4; ++i) {
        uint16_t rr_ms = state->last_rr_ms[i];
        payload[4 + i * 2] = (uint8_t)(rr_ms & 0xffu);
        payload[5 + i * 2] = (uint8_t)((rr_ms >> 8) & 0xffu);
    }

    return polar_build_decoded_chunk_v1(
        0,
        1,
        0,
        0,
        0,
        0,
        12,
        payload,
        sizeof(payload));
}

#if POLAR_SDK_HAS_BTSTACK

static const uint8_t POLAR_SDK_UUID_PMD_SERVICE_BE[16] = {
    0xFB, 0x00, 0x5C, 0x80, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t POLAR_SDK_UUID_PMD_CP_BE[16] = {
    0xFB, 0x00, 0x5C, 0x81, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t POLAR_SDK_UUID_PMD_DATA_BE[16] = {
    0xFB, 0x00, 0x5C, 0x82, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t POLAR_SDK_UUID_PSFTP_MTU_BE[16] = {
    0xFB, 0x00, 0x5C, 0x51, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t POLAR_SDK_UUID_PSFTP_D2H_BE[16] = {
    0xFB, 0x00, 0x5C, 0x52, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t POLAR_SDK_UUID_PSFTP_H2D_BE[16] = {
    0xFB, 0x00, 0x5C, 0x53, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

#define POLAR_UUID16_PSFTP_SERVICE (0xFEEE)

#endif // POLAR_SDK_HAS_BTSTACK

// -----------------------------------------------------------------------------
// Module-local exception types

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_Error,
    MP_QSTR_Error,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &mp_type_Exception
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_TimeoutError,
    MP_QSTR_TimeoutError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_NotConnectedError,
    MP_QSTR_NotConnectedError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_ProtocolError,
    MP_QSTR_ProtocolError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_BufferOverflowError,
    MP_QSTR_BufferOverflowError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_SecurityError,
    MP_QSTR_SecurityError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    polar_type_UnsupportedError,
    MP_QSTR_UnsupportedError,
    MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &polar_type_Error
    );

static NORETURN void polar_raise_exc(const mp_obj_type_t *type, mp_rom_error_text_t msg) {
    nlr_raise(mp_obj_new_exception_msg(type, msg));
}

typedef struct _polar_h10_obj_t polar_h10_obj_t;

static void polar_require_ready(polar_h10_obj_t *self);
static void polar_require_no_active_download_handle(polar_h10_obj_t *self);

// -----------------------------------------------------------------------------
// Device object

typedef struct _polar_h10_obj_t {
    mp_obj_base_t base;
    mp_obj_t addr;        // str|None
    mp_obj_t name_prefix; // str|None

    // Generic stats
    uint32_t connect_calls;
    uint32_t disconnect_calls;
    polar_capability_mask_t required_capabilities_mask;
    uint8_t required_services_mask; // internal discovery mapping
    polar_stream_format_t hr_stream_format;
    polar_stream_format_t ecg_stream_format;
    polar_stream_format_t acc_stream_format;
    uint16_t ecg_sample_rate_hz;
    uint16_t acc_sample_rate_hz;
    uint16_t acc_range;

    polar_sdk_runtime_link_t runtime_link;

#if POLAR_SDK_HAS_BTSTACK
    // Transport ownership and connection context.
    bool connect_intent;
    bool user_disconnect_requested;

    bd_addr_t peer_addr;
    bd_addr_type_t peer_addr_type;

    uint8_t last_att_status;

    uint32_t connect_attempts_total;
    uint32_t reconnect_backoff_events;
    uint32_t connect_success_total;

    uint32_t hci_events_total;
    uint32_t adv_reports_total;
    uint32_t adv_match_total;

    uint16_t att_mtu;
    bool mtu_exchange_pending;
    bool mtu_exchange_done;
    uint32_t mtu_exchange_total;

    uint32_t sm_pairing_complete_total;
    uint8_t sm_last_pairing_status;
    uint8_t sm_last_pairing_reason;

    // Discovered services/chars
    gatt_client_service_t hr_service;
    gatt_client_service_t pmd_service;
    gatt_client_service_t psftp_service;
    bool hr_service_found;
    bool pmd_service_found;
    bool psftp_service_found;

    uint16_t hr_measurement_handle;
    gatt_client_characteristic_t hr_measurement_char;
    bool hr_char_found;

    uint16_t pmd_cp_handle;
    gatt_client_characteristic_t pmd_cp_char;
    bool pmd_cp_char_found;
    uint16_t pmd_data_handle;
    gatt_client_characteristic_t pmd_data_char;
    bool pmd_data_char_found;
    uint16_t psftp_mtu_handle;
    gatt_client_characteristic_t psftp_mtu_char;
    bool psftp_mtu_char_found;
    uint16_t psftp_d2h_handle;
    gatt_client_characteristic_t psftp_d2h_char;
    bool psftp_d2h_char_found;
    uint16_t psftp_h2d_handle;
    gatt_client_characteristic_t psftp_h2d_char;
    bool psftp_h2d_char_found;

    // HR runtime
    gatt_client_notification_t hr_notification;
    bool hr_notification_listening;
    bool hr_enabled;
    bool hr_cfg_pending;
    bool hr_cfg_done;
    uint8_t hr_cfg_att_status;

    uint32_t hr_notifications_total;
    uint32_t hr_indications_total;
    uint32_t hr_value_events_total;
    uint32_t hr_unmatched_value_events_total;
    uint32_t hr_resubscribe_total;
    uint32_t hr_last_resubscribe_ms;
    uint32_t hr_consumed_seq;
    polar_sdk_hr_state_t hr_state;
    uint8_t hr_raw_last[24];
    uint8_t hr_raw_last_len;
    uint32_t hr_raw_seq;
    uint32_t hr_raw_consumed_seq;

    // PMD/ECG runtime
    gatt_client_notification_t pmd_cp_notification;
    gatt_client_notification_t pmd_data_notification;
    bool pmd_cp_notification_listening;
    bool pmd_data_notification_listening;
    bool pmd_cfg_pending;
    bool pmd_cfg_done;
    uint8_t pmd_cfg_att_status;
    bool pmd_write_pending;
    bool pmd_write_done;
    uint8_t pmd_write_att_status;

    bool pmd_cp_response_waiting;
    bool pmd_cp_response_done;
    uint8_t pmd_cp_response_expected_opcode;
    uint8_t pmd_cp_response_expected_type;
    uint8_t pmd_cp_response_status;
    uint8_t pmd_cp_response_more;
    uint8_t pmd_cp_response_params[128];
    uint8_t pmd_cp_response_params_len;

    bool ecg_enabled;
    uint8_t ecg_ring_storage[POLAR_SDK_ECG_RING_BYTES];
    polar_sdk_ecg_ring_t ecg_ring;

    bool imu_enabled;
    uint8_t imu_ring_storage[POLAR_SDK_IMU_RING_BYTES];
    polar_sdk_imu_ring_t imu_ring;

    bool recording_hr_active;
    bool recording_hr_config_known;
    uint8_t recording_hr_interval_s;
    polar_sdk_h10_recording_sample_type_t recording_hr_sample_type;
    char recording_hr_id[POLAR_SDK_H10_RECORDING_ID_MAX_BYTES + 1u];
    uint32_t recording_hr_id_next;
    bool recording_ecg_active;
    bool recording_acc_active;
    uint16_t recording_ecg_sample_rate_hz;
    uint16_t recording_acc_sample_rate_hz;
    uint16_t recording_acc_range;

    uint32_t pmd_cp_notifications_total;
    uint32_t pmd_cp_response_total;

    // PSFTP runtime
    gatt_client_notification_t psftp_mtu_notification;
    gatt_client_notification_t psftp_d2h_notification;
    bool psftp_mtu_notification_listening;
    bool psftp_d2h_notification_listening;
    bool psftp_mtu_enabled;
    bool psftp_d2h_enabled;
    bool psftp_cfg_pending;
    bool psftp_cfg_done;
    uint8_t psftp_cfg_att_status;
    bool psftp_write_pending;
    bool psftp_write_done;
    uint8_t psftp_write_att_status;
    bool psftp_response_waiting;
    bool psftp_response_done;
    polar_sdk_psftp_rx_result_t psftp_response_result;
    polar_sdk_psftp_rx_state_t psftp_rx_state;

    uint32_t psftp_tx_frames_total;
    uint32_t psftp_rx_frames_total;
    uint32_t psftp_rx_seq_errors_total;
    uint32_t psftp_protocol_errors_total;
    uint32_t psftp_overflow_errors_total;
    uint16_t psftp_last_error_code;
    uint8_t psftp_last_att_status;
    uint32_t psftp_last_response_bytes;
    bool psftp_last_security_related_failure;
    bool psftp_bond_clear_done;

    bool download_handle_active;
    uint32_t download_handle_id;
    uint32_t download_handle_next_id;
    uint8_t *download_blob;
    size_t download_blob_len;
    size_t download_blob_off;
    size_t download_blob_alloc;

    polar_sdk_discovery_stage_t discovery_stage;
#endif
} polar_h10_obj_t;

static void polar_require_ready(polar_h10_obj_t *self) {
    if (self->runtime_link.state != POLAR_SDK_STATE_READY ||
        !self->runtime_link.connected ||
        self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    }
}

static void polar_require_no_active_download_handle(polar_h10_obj_t *self) {
#if POLAR_SDK_HAS_BTSTACK
    if (self->download_handle_active) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("download handle already active"));
    }
#else
    (void)self;
#endif
}

#if POLAR_SDK_HAS_BTSTACK

static polar_h10_obj_t *polar_active_h10 = NULL;
static btstack_packet_callback_registration_t polar_hci_event_cb;
static btstack_packet_callback_registration_t polar_sm_event_cb;
static bool polar_hci_event_cb_registered = false;
static bool polar_sm_event_cb_registered = false;
static bool polar_gatt_client_initialized = false;

static inline uint32_t polar_now_ms(void) {
    return mp_hal_ticks_ms();
}

static inline bool polar_elapsed_ms(uint32_t start_ms, uint32_t budget_ms) {
    return (uint32_t)(polar_now_ms() - start_ms) >= budget_ms;
}

static void polar_clear_discovery_cache(polar_h10_obj_t *self) {
    memset(&self->hr_service, 0, sizeof(self->hr_service));
    memset(&self->pmd_service, 0, sizeof(self->pmd_service));
    memset(&self->psftp_service, 0, sizeof(self->psftp_service));

    self->hr_service_found = false;
    self->pmd_service_found = false;
    self->psftp_service_found = false;

    self->hr_measurement_handle = 0;
    memset(&self->hr_measurement_char, 0, sizeof(self->hr_measurement_char));
    self->hr_char_found = false;

    self->pmd_cp_handle = 0;
    memset(&self->pmd_cp_char, 0, sizeof(self->pmd_cp_char));
    self->pmd_cp_char_found = false;
    self->pmd_data_handle = 0;
    memset(&self->pmd_data_char, 0, sizeof(self->pmd_data_char));
    self->pmd_data_char_found = false;

    self->psftp_mtu_handle = 0;
    memset(&self->psftp_mtu_char, 0, sizeof(self->psftp_mtu_char));
    self->psftp_mtu_char_found = false;
    self->psftp_d2h_handle = 0;
    memset(&self->psftp_d2h_char, 0, sizeof(self->psftp_d2h_char));
    self->psftp_d2h_char_found = false;
    self->psftp_h2d_handle = 0;
    memset(&self->psftp_h2d_char, 0, sizeof(self->psftp_h2d_char));
    self->psftp_h2d_char_found = false;

    self->discovery_stage = POLAR_SDK_DISC_STAGE_IDLE;
    self->last_att_status = ATT_ERROR_SUCCESS;
}

static void polar_download_blob_reset(polar_h10_obj_t *self) {
    if (self == NULL) {
        return;
    }
    if (self->download_blob != NULL && self->download_blob_alloc > 0) {
        m_del(uint8_t, self->download_blob, self->download_blob_alloc);
    }
    self->download_blob = NULL;
    self->download_blob_len = 0;
    self->download_blob_off = 0;
    self->download_blob_alloc = 0;
    self->download_handle_active = false;
    self->download_handle_id = 0;

    self->psftp_response_waiting = false;
    self->psftp_response_done = false;
    self->psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    polar_sdk_psftp_rx_reset(&self->psftp_rx_state, NULL, 0);
}

static size_t polar_download_blob_available(const polar_h10_obj_t *self) {
    if (self == NULL) {
        return 0;
    }
    if (self->download_blob_off >= self->psftp_rx_state.length) {
        return 0;
    }
    return self->psftp_rx_state.length - self->download_blob_off;
}

static void polar_download_blob_compact_for_frame(polar_h10_obj_t *self, size_t incoming_frame_len) {
    if (self == NULL || self->download_blob == NULL || self->download_blob_alloc == 0) {
        return;
    }
    if (self->download_blob_off == 0 || self->download_blob_off > self->psftp_rx_state.length) {
        return;
    }

    size_t free_tail = self->download_blob_alloc - self->psftp_rx_state.length;
    if (free_tail >= incoming_frame_len) {
        return;
    }

    size_t remaining = self->psftp_rx_state.length - self->download_blob_off;
    if (remaining > 0) {
        memmove(self->download_blob, self->download_blob + self->download_blob_off, remaining);
    }
    self->download_blob_off = 0;
    self->psftp_rx_state.length = remaining;
}

static void polar_mark_attempt_failed(polar_h10_obj_t *self) {
    polar_sdk_runtime_mark_attempt_failed(&self->runtime_link);
}

static void polar_hci_packet_handler_and_discovery(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void polar_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void polar_discovery_start(polar_h10_obj_t *self);
static uint8_t polar_discovery_cmd_hr_chars(void *ctx);
static uint8_t polar_discovery_cmd_pmd_chars(void *ctx);
static uint8_t polar_discovery_cmd_psftp_chars(void *ctx);
static bool polar_psftp_security_ready(polar_h10_obj_t *self);
static int polar_psftp_prepare_result(polar_h10_obj_t *self);
static bool polar_psftp_reconnect_after_security_failure(polar_h10_obj_t *self, uint32_t timeout_ms);
static void polar_wait_conn_update_settle(polar_h10_obj_t *self, uint32_t timeout_ms);

static void polar_hr_parse_notification(polar_h10_obj_t *self, const uint8_t *value, uint16_t value_len) {
    (void)polar_sdk_hr_parse_measurement(&self->hr_state, value, value_len, polar_now_ms());
}

typedef struct {
    polar_h10_obj_t *self;
    bool *done_flag;
} polar_wait_flag_ctx_t;

static uint32_t polar_wait_flag_now_ms(void *ctx) {
    (void)ctx;
    return polar_now_ms();
}

static void polar_wait_flag_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    mp_event_wait_ms(ms);
}

static bool polar_wait_flag_is_done(void *ctx) {
    polar_wait_flag_ctx_t *w = (polar_wait_flag_ctx_t *)ctx;
    return *w->done_flag;
}

static void polar_btstack_pump_sleep_ms(uint32_t ms) {
    mp_event_wait_ms(ms);
}

static bool polar_wait_flag_is_connected(void *ctx) {
    polar_wait_flag_ctx_t *w = (polar_wait_flag_ctx_t *)ctx;
    return w->self->runtime_link.connected && w->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_wait_for_flag_or_disconnect(polar_h10_obj_t *self, bool *done_flag, uint32_t timeout_ms) {
    polar_wait_flag_ctx_t ctx = {
        .self = self,
        .done_flag = done_flag,
    };
    polar_sdk_wait_ops_t ops = {
        .ctx = &ctx,
        .now_ms = polar_wait_flag_now_ms,
        .sleep_ms = polar_wait_flag_sleep_ms,
        .is_done = polar_wait_flag_is_done,
        .is_connected = polar_wait_flag_is_connected,
    };
    return polar_sdk_wait_until_done_or_disconnect(&ops, timeout_ms, 10);
}

static bool polar_hr_wait_cfg_complete(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->hr_cfg_done, timeout_ms);
}

typedef struct {
    polar_h10_obj_t *self;
} polar_hr_notify_ctx_t;

static bool polar_hr_notify_is_connected_ready(void *ctx) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    return h->self->runtime_link.connected &&
        h->self->runtime_link.state == POLAR_SDK_STATE_READY &&
        h->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_hr_notify_listener_active(void *ctx) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    return h->self->hr_notification_listening;
}

static void polar_hr_notify_start_listener(void *ctx) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    gatt_client_listen_for_characteristic_value_updates(
        &h->self->hr_notification,
        polar_hci_packet_handler_and_discovery,
        h->self->runtime_link.conn_handle,
        &h->self->hr_measurement_char);
    h->self->hr_notification_listening = true;
}

static void polar_hr_notify_stop_listener(void *ctx) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    gatt_client_stop_listening_for_characteristic_value_updates(&h->self->hr_notification);
    h->self->hr_notification_listening = false;
}

static int polar_hr_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    h->self->hr_cfg_pending = true;
    h->self->hr_cfg_done = false;
    h->self->hr_cfg_att_status = ATT_ERROR_SUCCESS;
    uint8_t err = gatt_client_write_client_characteristic_configuration(
        polar_hci_packet_handler_and_discovery,
        h->self->runtime_link.conn_handle,
        &h->self->hr_measurement_char,
        ccc_cfg);
    if (err != ERROR_CODE_SUCCESS) {
        h->self->hr_cfg_pending = false;
        h->self->runtime_link.last_hci_status = err;
        return err;
    }
    return 0;
}

static bool polar_hr_notify_wait_complete(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    polar_hr_notify_ctx_t *h = (polar_hr_notify_ctx_t *)ctx;
    bool done = polar_hr_wait_cfg_complete(h->self, timeout_ms);
    if (out_att_status != NULL) {
        *out_att_status = h->self->hr_cfg_att_status;
    }
    return done;
}

static void polar_raise_for_notify_runtime_result(
    polar_sdk_gatt_notify_runtime_result_t r,
    mp_rom_error_text_t missing_msg,
    mp_rom_error_text_t no_prop_msg,
    mp_rom_error_text_t write_failed_msg,
    mp_rom_error_text_t timeout_msg,
    mp_rom_error_text_t rejected_msg) {
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_OK) {
        return;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    } else if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_MISSING_CHAR) {
        polar_raise_exc(&polar_type_ProtocolError, missing_msg);
    } else if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NO_NOTIFY_PROP) {
        polar_raise_exc(&polar_type_ProtocolError, no_prop_msg);
    } else if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT) {
        polar_raise_exc(&polar_type_TimeoutError, timeout_msg);
    } else if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED) {
        polar_raise_exc(&polar_type_ProtocolError, rejected_msg);
    } else {
        polar_raise_exc(&polar_type_Error, write_failed_msg);
    }
}

static void polar_hr_set_notify(polar_h10_obj_t *self, bool enable) {
    if (enable == self->hr_enabled) {
        return;
    }

    polar_hr_notify_ctx_t ctx = { .self = self };
    polar_sdk_gatt_notify_ops_t ops = {
        .ctx = &ctx,
        .is_connected_ready = polar_hr_notify_is_connected_ready,
        .listener_active = polar_hr_notify_listener_active,
        .start_listener = polar_hr_notify_start_listener,
        .stop_listener = polar_hr_notify_stop_listener,
        .write_ccc = polar_hr_notify_write_ccc,
        .wait_complete = polar_hr_notify_wait_complete,
    };
    polar_sdk_gatt_notify_runtime_args_t args = {
        .ops = &ops,
        .has_value_handle = self->hr_char_found && self->hr_measurement_char.value_handle != 0,
        .enable = enable,
        .properties = self->hr_measurement_char.properties,
        .prop_notify = ATT_PROPERTY_NOTIFY,
        .prop_indicate = ATT_PROPERTY_INDICATE,
        .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
        .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
        .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
        .att_success = ATT_ERROR_SUCCESS,
        .timeout_ms = POLAR_SDK_GATT_OP_TIMEOUT_MS,
        .cfg_pending = &self->hr_cfg_pending,
        .cfg_done = &self->hr_cfg_done,
    };

    polar_raise_for_notify_runtime_result(
        polar_sdk_gatt_notify_runtime_set(&args),
        MP_ERROR_TEXT("HR measurement characteristic missing"),
        MP_ERROR_TEXT("HR characteristic has no notify/indicate property"),
        MP_ERROR_TEXT("failed to write HR CCC"),
        MP_ERROR_TEXT("HR CCC timeout"),
        MP_ERROR_TEXT("HR CCC rejected"));

    self->hr_enabled = enable;
}

static void polar_hr_rearm_best_effort(polar_h10_obj_t *self) {
    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID || !self->hr_enabled) {
        return;
    }

    uint32_t now = polar_now_ms();
    if (self->hr_last_resubscribe_ms != 0 && (uint32_t)(now - self->hr_last_resubscribe_ms) < 1500) {
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        polar_hr_set_notify(self, false);
        polar_hr_set_notify(self, true);
        self->hr_resubscribe_total += 1;
        self->hr_last_resubscribe_ms = now;
        nlr_pop();
    } else {
        self->hr_enabled = false;
        self->hr_cfg_pending = false;
        self->hr_cfg_done = false;
    }
}

static void polar_ecg_ring_reset(polar_h10_obj_t *self) {
    polar_sdk_ecg_ring_reset(&self->ecg_ring);
}

static uint16_t polar_ecg_ring_available(const polar_h10_obj_t *self) {
    return polar_sdk_ecg_ring_available(&self->ecg_ring);
}

static uint16_t polar_ecg_ring_pop_bytes(polar_h10_obj_t *self, uint8_t *out, uint16_t max_len) {
    return polar_sdk_ecg_ring_pop_bytes(&self->ecg_ring, out, max_len);
}

static void polar_imu_ring_reset(polar_h10_obj_t *self) {
    polar_sdk_imu_ring_reset(&self->imu_ring);
}

static uint16_t polar_imu_ring_available(const polar_h10_obj_t *self) {
    return polar_sdk_imu_ring_available(&self->imu_ring);
}

static uint16_t polar_imu_ring_pop_bytes(polar_h10_obj_t *self, uint8_t *out, uint16_t max_len) {
    return polar_sdk_imu_ring_pop_bytes(&self->imu_ring, out, max_len);
}

static bool polar_pmd_wait_cfg_complete(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->pmd_cfg_done, timeout_ms);
}

static bool polar_pmd_wait_write_complete(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->pmd_write_done, timeout_ms);
}

static bool polar_pmd_wait_response(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->pmd_cp_response_done, timeout_ms);
}

static int polar_pmd_write_command_result(polar_h10_obj_t *self, const uint8_t *cmd, uint16_t cmd_len);
static int polar_pmd_set_notify_for_char_result(
    polar_h10_obj_t *self,
    gatt_client_characteristic_t *chr,
    gatt_client_notification_t *notification,
    bool *listening,
    bool enable);
static void polar_raise_for_pmd_notify_status(int status, bool missing_char);
static void polar_pmd_expect_response(polar_h10_obj_t *self, uint8_t opcode, uint8_t type);

static bool polar_pmd_policy_is_connected(void *ctx);
static uint8_t polar_pmd_policy_encryption_key_size(void *ctx);
static void polar_pmd_policy_request_pairing(void *ctx);
static void polar_pmd_policy_sleep_ms(void *ctx, uint32_t ms);

typedef enum {
    POLAR_RECORDING_KIND_KEY_INVALID = -1,
    POLAR_RECORDING_KIND_KEY_HR = 0,
    POLAR_RECORDING_KIND_KEY_ECG,
    POLAR_RECORDING_KIND_KEY_ACC,
} polar_recording_kind_key_t;

static polar_recording_kind_key_t polar_parse_recording_kind_key(mp_obj_t kind_obj) {
    polar_stream_kind_key_t stream_kind = polar_parse_stream_kind_key(kind_obj);
    switch (stream_kind) {
        case POLAR_STREAM_KIND_KEY_HR:
            return POLAR_RECORDING_KIND_KEY_HR;
        case POLAR_STREAM_KIND_KEY_ECG:
            return POLAR_RECORDING_KIND_KEY_ECG;
        case POLAR_STREAM_KIND_KEY_ACC:
            return POLAR_RECORDING_KIND_KEY_ACC;
        default:
            return POLAR_RECORDING_KIND_KEY_INVALID;
    }
}

static bool polar_recording_kind_is_supported_build(polar_recording_kind_key_t kind) {
    switch (kind) {
        case POLAR_RECORDING_KIND_KEY_HR:
            return POLAR_CFG_ENABLE_HR;
        default:
            return false;
    }
}

static bool polar_h10_recording_is_supported(polar_h10_obj_t *self) {
    return self != NULL &&
        POLAR_CFG_ENABLE_PSFTP &&
        POLAR_CFG_ENABLE_HR &&
        self->psftp_mtu_char_found &&
        self->psftp_h2d_char_found &&
        self->psftp_mtu_handle != 0 &&
        self->psftp_h2d_handle != 0;
}

static polar_sdk_h10_recording_sample_type_t polar_parse_h10_recording_sample_type(mp_obj_t obj) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("sample_type must be str"));
    }

    size_t len = 0;
    const char *value = mp_obj_str_get_data(obj, &len);
    if (len == 2 && memcmp(value, "hr", 2) == 0) {
        return POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE;
    }
    if (len == 2 && memcmp(value, "rr", 2) == 0) {
        return POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL;
    }

    mp_raise_ValueError(MP_ERROR_TEXT("invalid sample_type"));
}

static void polar_h10_recording_clear_local_state(polar_h10_obj_t *self) {
    self->recording_hr_active = false;
    self->recording_hr_config_known = false;
    self->recording_hr_interval_s = 1u;
    self->recording_hr_sample_type = POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE;
    self->recording_hr_id[0] = '\0';
}

static void polar_h10_recording_store_id(polar_h10_obj_t *self, const char *recording_id, size_t recording_id_len) {
    if (recording_id == NULL || recording_id_len == 0u) {
        self->recording_hr_id[0] = '\0';
        return;
    }

    if (recording_id_len > POLAR_SDK_H10_RECORDING_ID_MAX_BYTES) {
        recording_id_len = POLAR_SDK_H10_RECORDING_ID_MAX_BYTES;
    }
    memcpy(self->recording_hr_id, recording_id, recording_id_len);
    self->recording_hr_id[recording_id_len] = '\0';
}

static void polar_h10_recording_write_hex_u32(char *dst, uint32_t value) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8u; ++i) {
        unsigned shift = (unsigned)((7u - i) * 4u);
        dst[i] = hex[(value >> shift) & 0x0fu];
    }
}

static void polar_h10_recording_make_auto_id(polar_h10_obj_t *self, char *out, size_t out_capacity) {
    if (out == NULL || out_capacity < 12u) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("recording id buffer too small"));
    }

    if (self->recording_hr_id_next == 0u) {
        self->recording_hr_id_next = 1u;
    }

    memcpy(out, "REC", 3u);
    polar_h10_recording_write_hex_u32(out + 3u, self->recording_hr_id_next);
    out[11] = '\0';
    self->recording_hr_id_next += 1u;
}

static bool polar_h10_recording_local_config_matches(
    const polar_h10_obj_t *self,
    polar_sdk_h10_recording_sample_type_t sample_type,
    uint8_t interval_s) {
    return self->recording_hr_active &&
        self->recording_hr_config_known &&
        self->recording_hr_sample_type == sample_type &&
        self->recording_hr_interval_s == interval_s;
}

static void polar_h10_recording_prepare_or_raise(polar_h10_obj_t *self) {
    if (!POLAR_CFG_ENABLE_PSFTP || !polar_h10_recording_is_supported(self)) {
        return;
    }

    if (self->psftp_mtu_enabled && (self->psftp_d2h_enabled || !self->psftp_d2h_char_found) && polar_psftp_security_ready(self)) {
        return;
    }

    int prepare_status = polar_psftp_prepare_result(self);
    if (prepare_status == POLAR_SDK_PSFTP_OP_OK) {
        self->psftp_last_security_related_failure = false;
        return;
    }

    self->psftp_last_security_related_failure = polar_sdk_psftp_prepare_failure_is_security_related(
        prepare_status,
        polar_psftp_security_ready(self));
    if (self->psftp_last_security_related_failure) {
        polar_raise_exc(&polar_type_SecurityError, MP_ERROR_TEXT("PSFTP security setup failed"));
    }
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("PSFTP channel preparation failed"));
}

static bool polar_wait_for_mtu_exchange(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->mtu_exchange_done, timeout_ms);
}

typedef struct {
    polar_h10_obj_t *self;
} polar_pmd_mtu_ctx_t;

static bool polar_pmd_mtu_is_connected(void *ctx) {
    polar_pmd_mtu_ctx_t *m = (polar_pmd_mtu_ctx_t *)ctx;
    return m->self->runtime_link.connected && m->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static int polar_pmd_mtu_read(void *ctx, uint16_t *out_mtu) {
    polar_pmd_mtu_ctx_t *m = (polar_pmd_mtu_ctx_t *)ctx;

    uint16_t mtu = ATT_DEFAULT_MTU;
    if (gatt_client_get_mtu(m->self->runtime_link.conn_handle, &mtu) != ERROR_CODE_SUCCESS) {
        return -1;
    }

    m->self->att_mtu = mtu;
    if (out_mtu != NULL) {
        *out_mtu = mtu;
    }
    return 0;
}

static int polar_pmd_mtu_request_exchange(void *ctx) {
    polar_pmd_mtu_ctx_t *m = (polar_pmd_mtu_ctx_t *)ctx;
    m->self->mtu_exchange_pending = true;
    m->self->mtu_exchange_done = false;
    gatt_client_send_mtu_negotiation(polar_hci_packet_handler_and_discovery, m->self->runtime_link.conn_handle);
    return 0;
}

static bool polar_pmd_mtu_wait_exchange_complete(void *ctx, uint32_t timeout_ms) {
    polar_pmd_mtu_ctx_t *m = (polar_pmd_mtu_ctx_t *)ctx;
    return polar_wait_for_mtu_exchange(m->self, timeout_ms);
}

static uint16_t polar_pmd_mtu_current(void *ctx) {
    polar_pmd_mtu_ctx_t *m = (polar_pmd_mtu_ctx_t *)ctx;
    return m->self->att_mtu;
}

static bool polar_pmd_ensure_mtu(polar_h10_obj_t *self, uint16_t minimum_mtu, uint32_t timeout_ms) {
    polar_pmd_mtu_ctx_t ctx = { .self = self };
    polar_sdk_gatt_mtu_ops_t ops = {
        .ctx = &ctx,
        .is_connected = polar_pmd_mtu_is_connected,
        .read_mtu = polar_pmd_mtu_read,
        .request_exchange = polar_pmd_mtu_request_exchange,
        .wait_exchange_complete = polar_pmd_mtu_wait_exchange_complete,
        .current_mtu = polar_pmd_mtu_current,
    };

    polar_sdk_gatt_mtu_result_t r = polar_sdk_gatt_mtu_ensure_minimum(
        &ops,
        minimum_mtu,
        timeout_ms,
        &self->att_mtu);

    if (r != POLAR_SDK_GATT_MTU_RESULT_OK) {
        self->mtu_exchange_pending = false;
        if (r == POLAR_SDK_GATT_MTU_RESULT_TIMEOUT) {
            self->mtu_exchange_done = false;
        }
        return false;
    }

    self->mtu_exchange_pending = false;
    return true;
}

typedef struct {
    polar_h10_obj_t *self;
    gatt_client_characteristic_t *chr;
    gatt_client_notification_t *notification;
    bool *listening;
} polar_pmd_notify_ctx_t;

static bool polar_pmd_notify_is_connected_ready(void *ctx) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    return p->self->runtime_link.connected &&
        p->self->runtime_link.state == POLAR_SDK_STATE_READY &&
        p->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_pmd_notify_listener_active(void *ctx) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    return *p->listening;
}

static void polar_pmd_notify_start_listener(void *ctx) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    gatt_client_listen_for_characteristic_value_updates(
        p->notification,
        polar_hci_packet_handler_and_discovery,
        p->self->runtime_link.conn_handle,
        p->chr);
    *p->listening = true;
}

static void polar_pmd_notify_stop_listener(void *ctx) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    gatt_client_stop_listening_for_characteristic_value_updates(p->notification);
    *p->listening = false;
}

static int polar_pmd_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    p->self->pmd_cfg_pending = true;
    p->self->pmd_cfg_done = false;
    p->self->pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    uint8_t err = gatt_client_write_client_characteristic_configuration(
        polar_hci_packet_handler_and_discovery,
        p->self->runtime_link.conn_handle,
        p->chr,
        ccc_cfg);
    if (err != ERROR_CODE_SUCCESS) {
        p->self->pmd_cfg_pending = false;
        p->self->runtime_link.last_hci_status = err;
        return err;
    }
    return 0;
}

static bool polar_pmd_notify_wait_complete(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    polar_pmd_notify_ctx_t *p = (polar_pmd_notify_ctx_t *)ctx;
    bool done = polar_pmd_wait_cfg_complete(p->self, timeout_ms);
    if (out_att_status != NULL) {
        *out_att_status = p->self->pmd_cfg_att_status;
    }
    return done;
}

static int polar_pmd_set_notify_for_char_result(
    polar_h10_obj_t *self,
    gatt_client_characteristic_t *chr,
    gatt_client_notification_t *notification,
    bool *listening,
    bool enable
    ) {
    polar_pmd_notify_ctx_t ctx = {
        .self = self,
        .chr = chr,
        .notification = notification,
        .listening = listening,
    };
    polar_sdk_gatt_notify_ops_t ops = {
        .ctx = &ctx,
        .is_connected_ready = polar_pmd_notify_is_connected_ready,
        .listener_active = polar_pmd_notify_listener_active,
        .start_listener = polar_pmd_notify_start_listener,
        .stop_listener = polar_pmd_notify_stop_listener,
        .write_ccc = polar_pmd_notify_write_ccc,
        .wait_complete = polar_pmd_notify_wait_complete,
    };
    polar_sdk_gatt_notify_runtime_args_t args = {
        .ops = &ops,
        .has_value_handle = chr->value_handle != 0,
        .enable = enable,
        .properties = chr->properties,
        .prop_notify = ATT_PROPERTY_NOTIFY,
        .prop_indicate = ATT_PROPERTY_INDICATE,
        .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
        .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
        .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
        .att_success = ATT_ERROR_SUCCESS,
        .timeout_ms = POLAR_SDK_GATT_OP_TIMEOUT_MS,
        .cfg_pending = &self->pmd_cfg_pending,
        .cfg_done = &self->pmd_cfg_done,
    };

    polar_sdk_gatt_notify_runtime_result_t r = polar_sdk_gatt_notify_runtime_set(&args);
    return polar_sdk_pmd_map_notify_result(
        r,
        self->pmd_cfg_att_status,
        POLAR_SDK_PMD_OP_OK,
        POLAR_SDK_PMD_OP_NOT_CONNECTED,
        POLAR_SDK_PMD_OP_TIMEOUT,
        POLAR_SDK_PMD_OP_TRANSPORT);
}

static void polar_raise_for_pmd_notify_status(int status, bool missing_char) {
    if (status == POLAR_SDK_PMD_OP_OK) {
        return;
    }
    if (status == POLAR_SDK_PMD_OP_NOT_CONNECTED) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    } else if (status == POLAR_SDK_PMD_OP_TIMEOUT) {
        polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD CCC timeout"));
    } else if (status > 0) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD CCC rejected"));
    } else if (missing_char) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD characteristic missing"));
    } else {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("failed to write PMD CCC"));
    }
}

static void polar_pmd_set_notify_for_char(
    polar_h10_obj_t *self,
    gatt_client_characteristic_t *chr,
    gatt_client_notification_t *notification,
    bool *listening,
    bool enable
    ) {
    int status = polar_pmd_set_notify_for_char_result(self, chr, notification, listening, enable);
    polar_raise_for_pmd_notify_status(status, chr->value_handle == 0);
}

static void polar_pmd_expect_response(polar_h10_obj_t *self, uint8_t opcode, uint8_t type) {
    self->pmd_cp_response_expected_opcode = opcode;
    self->pmd_cp_response_expected_type = type;
    self->pmd_cp_response_status = 0xff;
    self->pmd_cp_response_more = 0;
    self->pmd_cp_response_params_len = 0;
    self->pmd_cp_response_done = false;
    self->pmd_cp_response_waiting = true;
}

typedef struct {
    polar_h10_obj_t *self;
} polar_pmd_write_ctx_t;

static bool polar_pmd_write_is_connected(void *ctx) {
    polar_pmd_write_ctx_t *w = (polar_pmd_write_ctx_t *)ctx;
    return w->self->runtime_link.connected && w->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static int polar_pmd_write_value_cb(void *ctx, const uint8_t *data, uint16_t len) {
    polar_pmd_write_ctx_t *w = (polar_pmd_write_ctx_t *)ctx;
    w->self->pmd_write_pending = true;
    w->self->pmd_write_done = false;
    w->self->pmd_write_att_status = ATT_ERROR_SUCCESS;

    uint8_t err = gatt_client_write_value_of_characteristic(
        polar_hci_packet_handler_and_discovery,
        w->self->runtime_link.conn_handle,
        w->self->pmd_cp_handle,
        len,
        (uint8_t *)data);
    if (err != ERROR_CODE_SUCCESS) {
        w->self->pmd_write_pending = false;
        w->self->runtime_link.last_hci_status = err;
        return err;
    }
    return 0;
}

static bool polar_pmd_write_wait_cb(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    polar_pmd_write_ctx_t *w = (polar_pmd_write_ctx_t *)ctx;
    bool done = polar_pmd_wait_write_complete(w->self, timeout_ms);
    if (out_att_status) {
        *out_att_status = w->self->pmd_write_att_status;
    }
    return done;
}

static int polar_pmd_write_command_result(polar_h10_obj_t *self, const uint8_t *cmd, uint16_t cmd_len) {
    polar_pmd_write_ctx_t ctx = { .self = self };
    polar_sdk_gatt_write_ops_t ops = {
        .ctx = &ctx,
        .is_connected = polar_pmd_write_is_connected,
        .write_value = polar_pmd_write_value_cb,
        .wait_complete = polar_pmd_write_wait_cb,
    };

    uint8_t att_status = ATT_ERROR_SUCCESS;
    polar_sdk_gatt_write_result_t r = polar_sdk_gatt_write_with_wait(
        &ops,
        cmd,
        cmd_len,
        ATT_ERROR_SUCCESS,
        POLAR_SDK_GATT_OP_TIMEOUT_MS,
        &att_status);

    self->pmd_write_att_status = att_status;
    if (r == POLAR_SDK_GATT_WRITE_NOT_CONNECTED) {
        return POLAR_SDK_PMD_OP_NOT_CONNECTED;
    }
    if (r == POLAR_SDK_GATT_WRITE_TIMEOUT) {
        self->pmd_write_pending = false;
        self->pmd_write_done = false;
        return POLAR_SDK_PMD_OP_TIMEOUT;
    }
    if (r == POLAR_SDK_GATT_WRITE_ATT_REJECTED) {
        return self->pmd_write_att_status;
    }
    if (r == POLAR_SDK_GATT_WRITE_FAILED) {
        return POLAR_SDK_PMD_OP_TRANSPORT;
    }
    return POLAR_SDK_PMD_OP_OK;
}

static void polar_pmd_write_command(polar_h10_obj_t *self, const uint8_t *cmd, uint16_t cmd_len) {
    int r = polar_pmd_write_command_result(self, cmd, cmd_len);
    if (r == POLAR_SDK_PMD_OP_OK) {
        return;
    }
    if (r == POLAR_SDK_PMD_OP_NOT_CONNECTED) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    } else if (r == POLAR_SDK_PMD_OP_TIMEOUT) {
        polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD write timeout"));
    } else if (r > 0) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD write rejected"));
    } else {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("failed to write PMD command"));
    }
}

static void polar_pmd_parse_cp_notification(polar_h10_obj_t *self, const uint8_t *value, uint16_t value_len) {
    self->pmd_cp_notifications_total += 1;

    polar_sdk_pmd_cp_response_t response;
    if (!polar_sdk_pmd_parse_cp_response(value, value_len, &response)) {
        return;
    }

    self->pmd_cp_response_total += 1;

    if (self->pmd_cp_response_waiting &&
        response.opcode == self->pmd_cp_response_expected_opcode &&
        (self->pmd_cp_response_expected_type == 0xffu || response.measurement_type == self->pmd_cp_response_expected_type)) {
        self->pmd_cp_response_status = response.status;
        self->pmd_cp_response_more = response.has_more ? response.more : 0;
        self->pmd_cp_response_params_len = 0;
        if (response.status == POLAR_SDK_PMD_RESPONSE_SUCCESS && value_len > 5u) {
            size_t params_len = (size_t)value_len - 5u;
            if (params_len > sizeof(self->pmd_cp_response_params)) {
                params_len = sizeof(self->pmd_cp_response_params);
            }
            memcpy(self->pmd_cp_response_params, value + 5u, params_len);
            self->pmd_cp_response_params_len = (uint8_t)params_len;
        }
        self->pmd_cp_response_done = true;
        self->pmd_cp_response_waiting = false;
    }
}

static void polar_pmd_parse_data(polar_h10_obj_t *self, const uint8_t *value, uint16_t value_len) {
    polar_sdk_ecg_parse_pmd_notification(&self->ecg_ring, POLAR_SDK_PMD_MEAS_ECG, value, value_len);
    polar_sdk_imu_parse_pmd_notification(&self->imu_ring, POLAR_SDK_PMD_MEAS_ACC, value, value_len);
}

static bool polar_psftp_wait_cfg_complete(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->psftp_cfg_done, timeout_ms);
}

static bool polar_psftp_wait_write_complete(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->psftp_write_done, timeout_ms);
}

static bool polar_psftp_wait_response(polar_h10_obj_t *self, uint32_t timeout_ms) {
    return polar_wait_for_flag_or_disconnect(self, &self->psftp_response_done, timeout_ms);
}

static uint16_t polar_psftp_frame_capacity(const polar_h10_obj_t *self) {
    uint16_t att_mtu = self->att_mtu;
    if (att_mtu < ATT_DEFAULT_MTU) {
        att_mtu = ATT_DEFAULT_MTU;
    }
    if (att_mtu <= 3u) {
        return 20u;
    }
    return (uint16_t)(att_mtu - 3u);
}

static bool polar_psftp_security_ready(polar_h10_obj_t *self) {
    if (!self->runtime_link.connected ||
        self->runtime_link.state != POLAR_SDK_STATE_READY ||
        self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }
    return gap_encryption_key_size(self->runtime_link.conn_handle) > 0;
}

static bool polar_psftp_security_is_connected_cb(const void *ctx) {
    const polar_h10_obj_t *self = (const polar_h10_obj_t *)ctx;
    return self->runtime_link.connected && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_psftp_security_is_secure_cb(const void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_psftp_security_ready(self);
}

static void polar_psftp_security_request_pairing_cb(const void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    sm_request_pairing(self->runtime_link.conn_handle);
}

static void polar_psftp_security_sleep_ms_cb(const void *ctx, uint32_t ms) {
    (void)ctx;
    polar_btstack_pump_sleep_ms(ms);
}

static bool polar_psftp_security_is_secure_prepare_cb(void *ctx) {
    return polar_psftp_security_is_secure_cb((const void *)ctx);
}

static void polar_clear_local_bond_for_addr_once(
    polar_h10_obj_t *self,
    bd_addr_type_t addr_type,
    const bd_addr_t addr) {
    if (self->psftp_bond_clear_done) {
        return;
    }

    (void)addr_type;
    (void)addr;
    self->psftp_bond_clear_done = true;
}

static void polar_psftp_clear_local_bond_once(polar_h10_obj_t *self) {
    if (self->psftp_bond_clear_done) {
        return;
    }
    if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    polar_clear_local_bond_for_addr_once(self, self->peer_addr_type, self->peer_addr);
}

static bool polar_psftp_request_pairing_and_wait(polar_h10_obj_t *self) {
    if (polar_psftp_security_ready(self)) {
        return true;
    }

    if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }

    polar_sdk_btstack_sm_apply_default_auth_policy();
    polar_psftp_clear_local_bond_once(self);

    polar_sdk_security_policy_t policy = {
        .rounds = 1,
        .wait_ms_per_round = POLAR_SDK_PMD_SECURITY_WAIT_MS,
        .request_gap_ms = 120,
        .poll_ms = 20,
    };
    polar_sdk_security_ops_t ops = {
        .ctx = self,
        .is_connected = polar_psftp_security_is_connected_cb,
        .is_secure = polar_psftp_security_is_secure_cb,
        .request_pairing = polar_psftp_security_request_pairing_cb,
        .sleep_ms = polar_psftp_security_sleep_ms_cb,
    };

    bool dropped_stale_bond = false;

    for (size_t round = 0; round < POLAR_SDK_PMD_SECURITY_ROUNDS; ++round) {
        if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
            return false;
        }

        uint32_t pair_before = self->sm_pairing_complete_total;
        polar_sdk_security_result_t r = polar_sdk_security_request_with_retry(&policy, &ops);
        if (r == POLAR_SDK_SECURITY_RESULT_OK) {
            return true;
        }
        if (r == POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED) {
            return false;
        }

        bool pairing_failed = self->sm_pairing_complete_total != pair_before && self->sm_last_pairing_status != 0;
        if (pairing_failed && !dropped_stale_bond && self->peer_addr_type != BD_ADDR_TYPE_UNKNOWN) {
            gap_delete_bonding(self->peer_addr_type, self->peer_addr);
            dropped_stale_bond = true;
        }
    }

    return polar_psftp_security_ready(self);
}

typedef struct {
    polar_h10_obj_t *self;
    gatt_client_characteristic_t *chr;
    gatt_client_notification_t *notification;
    bool *listening;
} polar_psftp_notify_ctx_t;

static bool polar_psftp_notify_is_connected_ready(void *ctx) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    return p->self->runtime_link.connected &&
        p->self->runtime_link.state == POLAR_SDK_STATE_READY &&
        p->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_psftp_notify_listener_active(void *ctx) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    return *p->listening;
}

static void polar_psftp_notify_start_listener(void *ctx) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    gatt_client_listen_for_characteristic_value_updates(
        p->notification,
        polar_hci_packet_handler_and_discovery,
        p->self->runtime_link.conn_handle,
        p->chr);
    *p->listening = true;
}

static void polar_psftp_notify_stop_listener(void *ctx) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    gatt_client_stop_listening_for_characteristic_value_updates(p->notification);
    *p->listening = false;
}

static int polar_psftp_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    p->self->psftp_cfg_pending = true;
    p->self->psftp_cfg_done = false;
    p->self->psftp_cfg_att_status = ATT_ERROR_SUCCESS;

    uint8_t err = gatt_client_write_client_characteristic_configuration(
        polar_hci_packet_handler_and_discovery,
        p->self->runtime_link.conn_handle,
        p->chr,
        ccc_cfg);
    if (err != ERROR_CODE_SUCCESS) {
        p->self->psftp_cfg_pending = false;
        p->self->runtime_link.last_hci_status = err;
        return err;
    }
    return 0;
}

static bool polar_psftp_notify_wait_complete(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    polar_psftp_notify_ctx_t *p = (polar_psftp_notify_ctx_t *)ctx;
    bool done = polar_psftp_wait_cfg_complete(p->self, timeout_ms);
    if (out_att_status != NULL) {
        *out_att_status = p->self->psftp_cfg_att_status;
    }
    return done;
}

static int polar_psftp_set_notify_for_char_result(
    polar_h10_obj_t *self,
    gatt_client_characteristic_t *chr,
    gatt_client_notification_t *notification,
    bool *listening,
    bool enable) {
    polar_psftp_notify_ctx_t ctx = {
        .self = self,
        .chr = chr,
        .notification = notification,
        .listening = listening,
    };
    polar_sdk_gatt_notify_ops_t ops = {
        .ctx = &ctx,
        .is_connected_ready = polar_psftp_notify_is_connected_ready,
        .listener_active = polar_psftp_notify_listener_active,
        .start_listener = polar_psftp_notify_start_listener,
        .stop_listener = polar_psftp_notify_stop_listener,
        .write_ccc = polar_psftp_notify_write_ccc,
        .wait_complete = polar_psftp_notify_wait_complete,
    };
    polar_sdk_gatt_notify_runtime_args_t args = {
        .ops = &ops,
        .has_value_handle = chr->value_handle != 0,
        .enable = enable,
        .properties = chr->properties,
        .prop_notify = ATT_PROPERTY_NOTIFY,
        .prop_indicate = ATT_PROPERTY_INDICATE,
        .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
        .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
        .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
        .att_success = ATT_ERROR_SUCCESS,
        .timeout_ms = POLAR_SDK_PSFTP_GATT_OP_TIMEOUT_MS,
        .cfg_pending = &self->psftp_cfg_pending,
        .cfg_done = &self->psftp_cfg_done,
    };

    polar_sdk_gatt_notify_runtime_result_t r = polar_sdk_gatt_notify_runtime_set(&args);
    self->psftp_last_att_status = self->psftp_cfg_att_status;
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_OK) {
        return POLAR_SDK_PSFTP_OP_OK;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_OP_NOT_CONNECTED;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT) {
        return POLAR_SDK_PSFTP_OP_TIMEOUT;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_MISSING_CHAR) {
        return POLAR_SDK_PSFTP_OP_MISSING_CHAR;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NO_NOTIFY_PROP) {
        return POLAR_SDK_PSFTP_OP_NO_NOTIFY_PROP;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED) {
        return self->psftp_cfg_att_status;
    }
    return POLAR_SDK_PSFTP_OP_TRANSPORT;
}

static bool polar_psftp_prepare_is_connected_ready(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->runtime_link.connected &&
        self->runtime_link.state == POLAR_SDK_STATE_READY &&
        self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool polar_psftp_prepare_has_required_characteristics(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->psftp_mtu_char_found &&
        self->psftp_h2d_char_found &&
        self->psftp_mtu_handle != 0 &&
        self->psftp_h2d_handle != 0;
}

static polar_sdk_security_result_t polar_psftp_prepare_ensure_security(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (polar_psftp_request_pairing_and_wait(self)) {
        return POLAR_SDK_SECURITY_RESULT_OK;
    }
    if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
    }
    return POLAR_SDK_SECURITY_RESULT_TIMEOUT;
}

static bool polar_psftp_prepare_mtu_notify_enabled(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->psftp_mtu_enabled;
}

static int polar_psftp_prepare_enable_mtu_notify(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    int status = polar_psftp_set_notify_for_char_result(
        self,
        &self->psftp_mtu_char,
        &self->psftp_mtu_notification,
        &self->psftp_mtu_notification_listening,
        true);
    if (status == POLAR_SDK_PSFTP_OP_OK) {
        self->psftp_mtu_enabled = true;
    }
    return status;
}

static bool polar_psftp_prepare_d2h_notify_supported(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->psftp_d2h_char_found && self->psftp_d2h_handle != 0;
}

static bool polar_psftp_prepare_d2h_notify_enabled(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->psftp_d2h_enabled;
}

static int polar_psftp_prepare_enable_d2h_notify(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    int status = polar_psftp_set_notify_for_char_result(
        self,
        &self->psftp_d2h_char,
        &self->psftp_d2h_notification,
        &self->psftp_d2h_notification_listening,
        true);
    if (status == POLAR_SDK_PSFTP_OP_OK) {
        self->psftp_d2h_enabled = true;
    }
    return status;
}

static int polar_psftp_prepare_result(polar_h10_obj_t *self) {
    if (!polar_psftp_prepare_is_connected_ready(self)) {
        return POLAR_SDK_PSFTP_OP_NOT_CONNECTED;
    }

    // Align with probe behavior: give post-connect parameter update time to
    // settle before first security-sensitive PSFTP preparation step.
    if (self->runtime_link.conn_update_pending) {
        polar_wait_conn_update_settle(self, 1200);
    }

    // First PSFTP operation after connect can race immediately into pairing on
    // some sessions. Hold a one-time pre-security settle window so callers
    // don't need ad-hoc sleeps after connect().
    if (!self->psftp_mtu_enabled && !self->psftp_d2h_enabled && !polar_psftp_security_ready(self)) {
        mp_event_wait_ms(1200);
    }

    polar_sdk_psftp_prepare_policy_t policy = {
        .retry_security_on_att = true,
        .strict_d2h_enable = false,
    };
    polar_sdk_psftp_prepare_ops_t ops = {
        .ctx = self,
        .is_connected_ready = polar_psftp_prepare_is_connected_ready,
        .has_required_characteristics = polar_psftp_prepare_has_required_characteristics,
        .security_ready = polar_psftp_security_is_secure_prepare_cb,
        .ensure_security = polar_psftp_prepare_ensure_security,
        .mtu_notify_enabled = polar_psftp_prepare_mtu_notify_enabled,
        .enable_mtu_notify = polar_psftp_prepare_enable_mtu_notify,
        .d2h_notify_supported = polar_psftp_prepare_d2h_notify_supported,
        .d2h_notify_enabled = polar_psftp_prepare_d2h_notify_enabled,
        .enable_d2h_notify = polar_psftp_prepare_enable_d2h_notify,
    };

    return polar_sdk_psftp_prepare_channels(&policy, &ops);
}

typedef struct {
    polar_h10_obj_t *self;
} polar_psftp_write_ctx_t;

static bool polar_psftp_write_is_connected(void *ctx) {
    polar_psftp_write_ctx_t *w = (polar_psftp_write_ctx_t *)ctx;
    return w->self->runtime_link.connected && w->self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static int polar_psftp_write_value_cb(void *ctx, const uint8_t *data, uint16_t len) {
    polar_psftp_write_ctx_t *w = (polar_psftp_write_ctx_t *)ctx;
    w->self->psftp_write_pending = true;
    w->self->psftp_write_done = false;
    w->self->psftp_write_att_status = ATT_ERROR_SUCCESS;

    uint8_t err = gatt_client_write_value_of_characteristic(
        polar_hci_packet_handler_and_discovery,
        w->self->runtime_link.conn_handle,
        w->self->psftp_mtu_handle,
        len,
        (uint8_t *)data);
    if (err != ERROR_CODE_SUCCESS) {
        w->self->psftp_write_pending = false;
        w->self->runtime_link.last_hci_status = err;
        return err;
    }
    return 0;
}

static bool polar_psftp_write_wait_cb(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    polar_psftp_write_ctx_t *w = (polar_psftp_write_ctx_t *)ctx;
    bool done = polar_psftp_wait_write_complete(w->self, timeout_ms);
    if (out_att_status != 0) {
        *out_att_status = w->self->psftp_write_att_status;
    }
    return done;
}

static int polar_psftp_write_frame_result(polar_h10_obj_t *self, const uint8_t *frame, uint16_t frame_len) {
    polar_psftp_write_ctx_t ctx = { .self = self };
    polar_sdk_gatt_write_ops_t ops = {
        .ctx = &ctx,
        .is_connected = polar_psftp_write_is_connected,
        .write_value = polar_psftp_write_value_cb,
        .wait_complete = polar_psftp_write_wait_cb,
    };

    uint8_t att_status = ATT_ERROR_SUCCESS;
    polar_sdk_gatt_write_result_t r = polar_sdk_gatt_write_with_wait(
        &ops,
        frame,
        frame_len,
        ATT_ERROR_SUCCESS,
        POLAR_SDK_GATT_OP_TIMEOUT_MS,
        &att_status);

    self->psftp_write_att_status = att_status;
    self->psftp_last_att_status = att_status;

    if (r == POLAR_SDK_GATT_WRITE_OK) {
        return POLAR_SDK_PSFTP_OP_OK;
    }
    if (r == POLAR_SDK_GATT_WRITE_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_OP_NOT_CONNECTED;
    }
    if (r == POLAR_SDK_GATT_WRITE_TIMEOUT) {
        self->psftp_write_pending = false;
        self->psftp_write_done = false;
        return POLAR_SDK_PSFTP_OP_TIMEOUT;
    }
    if (r == POLAR_SDK_GATT_WRITE_ATT_REJECTED) {
        return self->psftp_write_att_status;
    }
    return POLAR_SDK_PSFTP_OP_TRANSPORT;
}

static int polar_psftp_get_prepare_channels_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_psftp_prepare_result(self);
}

static uint16_t polar_psftp_get_frame_capacity_cb(void *ctx) {
    const polar_h10_obj_t *self = (const polar_h10_obj_t *)ctx;
    return polar_psftp_frame_capacity(self);
}

static int polar_psftp_get_write_frame_cb(void *ctx, const uint8_t *frame, uint16_t frame_len) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_psftp_write_frame_result(self, frame, frame_len);
}

static void polar_psftp_get_on_tx_frame_ok_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->psftp_tx_frames_total += 1;
}

static void polar_psftp_get_begin_response_cb(void *ctx, uint8_t *response, size_t response_capacity) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->psftp_last_error_code = 0;
    self->psftp_last_response_bytes = 0;
    self->psftp_response_waiting = true;
    self->psftp_response_done = false;
    self->psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    polar_sdk_psftp_rx_reset(&self->psftp_rx_state, response, response_capacity);
}

static bool polar_psftp_get_wait_response_cb(void *ctx, uint32_t timeout_ms) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    bool done = polar_psftp_wait_response(self, timeout_ms);
    if (!done) {
        self->psftp_response_waiting = false;
        self->psftp_response_done = false;
    }
    return done;
}

static polar_sdk_psftp_rx_result_t polar_psftp_get_response_result_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->psftp_response_result;
}

static const polar_sdk_psftp_rx_state_t *polar_psftp_get_rx_state_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return &self->psftp_rx_state;
}

static bool polar_psftp_get_is_remote_success_cb(void *ctx, uint16_t error_code) {
    (void)ctx;
#if POLAR_CFG_ENABLE_PSFTP
    return error_code == protocol_PbPFtpError_OPERATION_SUCCEEDED;
#else
    (void)error_code;
    return false;
#endif
}

static polar_sdk_psftp_trans_result_t polar_psftp_map_prepare_status_to_result(int prepare_status) {
    if (prepare_status == POLAR_SDK_PSFTP_OP_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED;
    }
    if (prepare_status == POLAR_SDK_PSFTP_OP_MISSING_CHAR || prepare_status == POLAR_SDK_PSFTP_OP_NO_NOTIFY_PROP) {
        return POLAR_SDK_PSFTP_TRANS_MISSING_CHARACTERISTICS;
    }
    if (prepare_status == POLAR_SDK_PSFTP_OP_TIMEOUT) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_TIMEOUT;
    }
    if (prepare_status == POLAR_SDK_PSFTP_OP_TRANSPORT) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
    }
    if (prepare_status > 0) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_ATT_REJECTED;
    }
    return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
}

static polar_sdk_psftp_trans_result_t polar_psftp_map_write_status_to_result(int write_status) {
    if (write_status == POLAR_SDK_PSFTP_OP_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED;
    }
    if (write_status == POLAR_SDK_PSFTP_OP_TIMEOUT) {
        return POLAR_SDK_PSFTP_TRANS_WRITE_TIMEOUT;
    }
    if (write_status == POLAR_SDK_PSFTP_OP_TRANSPORT) {
        return POLAR_SDK_PSFTP_TRANS_WRITE_FAILED;
    }
    if (write_status > 0) {
        return POLAR_SDK_PSFTP_TRANS_WRITE_ATT_REJECTED;
    }
    return POLAR_SDK_PSFTP_TRANS_WRITE_FAILED;
}

static polar_sdk_psftp_trans_result_t polar_psftp_begin_get_stream(
    polar_h10_obj_t *self,
    const char *path,
    size_t path_len) {
    if (self == NULL || path == NULL || self->download_blob == NULL || self->download_blob_alloc == 0) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    int prepare_status = polar_psftp_prepare_result(self);
    if (prepare_status != POLAR_SDK_PSFTP_OP_OK) {
        if (prepare_status > 0) {
            self->psftp_last_att_status = (uint8_t)prepare_status;
        }
        return polar_psftp_map_prepare_status_to_result(prepare_status);
    }

    uint8_t proto_payload[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES];
    size_t proto_payload_len = 0;
    if (!polar_sdk_psftp_encode_get_operation(path, path_len, proto_payload, sizeof(proto_payload), &proto_payload_len)) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    uint8_t request_stream[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES + 2u];
    size_t request_len = polar_sdk_psftp_build_rfc60_request(
        proto_payload,
        proto_payload_len,
        request_stream,
        sizeof(request_stream));
    if (request_len == 0) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    uint16_t frame_capacity = polar_psftp_frame_capacity(self);
    uint8_t frame[520];
    if (frame_capacity == 0 || frame_capacity > sizeof(frame)) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    self->psftp_last_error_code = 0;
    self->psftp_last_response_bytes = 0;
    self->psftp_response_waiting = true;
    self->psftp_response_done = false;
    self->psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    self->download_blob_off = 0;
    polar_sdk_psftp_rx_reset(&self->psftp_rx_state, self->download_blob, self->download_blob_alloc);

    polar_sdk_psftp_tx_state_t tx;
    polar_sdk_psftp_tx_init(&tx, request_stream, request_len);

    while (polar_sdk_psftp_tx_has_more(&tx)) {
        bool is_last = false;
        size_t frame_len = polar_sdk_psftp_tx_build_next_frame(
            &tx,
            frame_capacity,
            frame,
            sizeof(frame),
            &is_last);
        (void)is_last;

        if (frame_len == 0 || frame_len > UINT16_MAX) {
            self->psftp_response_waiting = false;
            self->psftp_response_done = false;
            return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
        }

        int write_status = polar_psftp_write_frame_result(self, frame, (uint16_t)frame_len);
        if (write_status != POLAR_SDK_PSFTP_OP_OK) {
            if (write_status > 0) {
                self->psftp_last_att_status = (uint8_t)write_status;
            }
            self->psftp_response_waiting = false;
            self->psftp_response_done = false;
            return polar_psftp_map_write_status_to_result(write_status);
        }

        self->psftp_tx_frames_total += 1;
    }

    return POLAR_SDK_PSFTP_TRANS_OK;
}

static polar_sdk_psftp_trans_result_t polar_psftp_execute_get(
    polar_h10_obj_t *self,
    const char *path,
    size_t path_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len) {
    if (out_response_len != NULL) {
        *out_response_len = 0;
    }

    polar_sdk_psftp_get_ops_t ops = {
        .ctx = self,
        .prepare_channels = polar_psftp_get_prepare_channels_cb,
        .frame_capacity = polar_psftp_get_frame_capacity_cb,
        .write_frame = polar_psftp_get_write_frame_cb,
        .on_tx_frame_ok = polar_psftp_get_on_tx_frame_ok_cb,
        .begin_response = polar_psftp_get_begin_response_cb,
        .wait_response = polar_psftp_get_wait_response_cb,
        .response_result = polar_psftp_get_response_result_cb,
        .rx_state = polar_psftp_get_rx_state_cb,
        .is_remote_success = polar_psftp_get_is_remote_success_cb,
    };

    self->psftp_last_security_related_failure = false;
    bool retried_after_reconnect = false;
    for (;;) {
        int prepare_status = POLAR_SDK_PSFTP_OP_OK;
        int write_status = POLAR_SDK_PSFTP_OP_OK;
        uint16_t error_code = 0;

        polar_sdk_psftp_trans_result_t result = polar_sdk_psftp_execute_get_operation(
            &ops,
            path,
            path_len,
            response,
            response_capacity,
            timeout_ms,
            out_response_len,
            &error_code,
            &prepare_status,
            &write_status);

        self->psftp_last_response_bytes = (uint32_t)self->psftp_rx_state.length;

        if (result == POLAR_SDK_PSFTP_TRANS_OK) {
            self->psftp_response_waiting = false;
            self->psftp_last_security_related_failure = false;
            return result;
        }

        if (prepare_status > 0 || write_status > 0) {
            self->psftp_last_att_status = (uint8_t)(write_status > 0 ? write_status : prepare_status);
        }
        self->psftp_last_error_code = error_code;

        bool security_related = polar_sdk_psftp_prepare_failure_is_security_related(
            prepare_status,
            polar_psftp_security_ready(self));
        self->psftp_last_security_related_failure = security_related;

        if (!retried_after_reconnect && security_related &&
            polar_psftp_reconnect_after_security_failure(self, POLAR_SDK_PSFTP_RECONNECT_TIMEOUT_MS)) {
            retried_after_reconnect = true;
            continue;
        }

        self->psftp_response_waiting = false;
        return result;
    }
}

static polar_sdk_psftp_trans_result_t polar_psftp_execute_proto(
    polar_h10_obj_t *self,
    const uint8_t *proto_payload,
    size_t proto_payload_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len) {
    if (out_response_len != NULL) {
        *out_response_len = 0;
    }

    polar_sdk_psftp_get_ops_t ops = {
        .ctx = self,
        .prepare_channels = polar_psftp_get_prepare_channels_cb,
        .frame_capacity = polar_psftp_get_frame_capacity_cb,
        .write_frame = polar_psftp_get_write_frame_cb,
        .on_tx_frame_ok = polar_psftp_get_on_tx_frame_ok_cb,
        .begin_response = polar_psftp_get_begin_response_cb,
        .wait_response = polar_psftp_get_wait_response_cb,
        .response_result = polar_psftp_get_response_result_cb,
        .rx_state = polar_psftp_get_rx_state_cb,
        .is_remote_success = polar_psftp_get_is_remote_success_cb,
    };

    self->psftp_last_security_related_failure = false;
    bool retried_after_reconnect = false;
    for (;;) {
        int prepare_status = POLAR_SDK_PSFTP_OP_OK;
        int write_status = POLAR_SDK_PSFTP_OP_OK;
        uint16_t error_code = 0;

        polar_sdk_psftp_trans_result_t result = polar_sdk_psftp_execute_proto_operation(
            &ops,
            proto_payload,
            proto_payload_len,
            response,
            response_capacity,
            timeout_ms,
            out_response_len,
            &error_code,
            &prepare_status,
            &write_status);

        self->psftp_last_response_bytes = (uint32_t)self->psftp_rx_state.length;

        if (result == POLAR_SDK_PSFTP_TRANS_OK) {
            self->psftp_response_waiting = false;
            self->psftp_last_security_related_failure = false;
            return result;
        }

        if (prepare_status > 0 || write_status > 0) {
            self->psftp_last_att_status = (uint8_t)(write_status > 0 ? write_status : prepare_status);
        }
        self->psftp_last_error_code = error_code;

        bool security_related = polar_sdk_psftp_prepare_failure_is_security_related(
            prepare_status,
            polar_psftp_security_ready(self));
        self->psftp_last_security_related_failure = security_related;

        if (!retried_after_reconnect && security_related &&
            polar_psftp_reconnect_after_security_failure(self, POLAR_SDK_PSFTP_RECONNECT_TIMEOUT_MS)) {
            retried_after_reconnect = true;
            continue;
        }

        self->psftp_response_waiting = false;
        return result;
    }
}

static polar_sdk_psftp_trans_result_t polar_psftp_execute_query(
    polar_h10_obj_t *self,
    uint16_t query_id,
    const uint8_t *query_payload,
    size_t query_payload_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len) {
    if (out_response_len != NULL) {
        *out_response_len = 0;
    }

    polar_sdk_psftp_get_ops_t ops = {
        .ctx = self,
        .prepare_channels = polar_psftp_get_prepare_channels_cb,
        .frame_capacity = polar_psftp_get_frame_capacity_cb,
        .write_frame = polar_psftp_get_write_frame_cb,
        .on_tx_frame_ok = polar_psftp_get_on_tx_frame_ok_cb,
        .begin_response = polar_psftp_get_begin_response_cb,
        .wait_response = polar_psftp_get_wait_response_cb,
        .response_result = polar_psftp_get_response_result_cb,
        .rx_state = polar_psftp_get_rx_state_cb,
        .is_remote_success = polar_psftp_get_is_remote_success_cb,
    };

    self->psftp_last_security_related_failure = false;
    bool retried_after_reconnect = false;
    for (;;) {
        int prepare_status = POLAR_SDK_PSFTP_OP_OK;
        int write_status = POLAR_SDK_PSFTP_OP_OK;
        uint16_t error_code = 0;

        polar_sdk_psftp_trans_result_t result = polar_sdk_psftp_execute_query_operation(
            &ops,
            query_id,
            query_payload,
            query_payload_len,
            response,
            response_capacity,
            timeout_ms,
            out_response_len,
            &error_code,
            &prepare_status,
            &write_status);

        self->psftp_last_response_bytes = (uint32_t)self->psftp_rx_state.length;

        if (result == POLAR_SDK_PSFTP_TRANS_OK) {
            self->psftp_response_waiting = false;
            self->psftp_last_security_related_failure = false;
            return result;
        }

        if (prepare_status > 0 || write_status > 0) {
            self->psftp_last_att_status = (uint8_t)(write_status > 0 ? write_status : prepare_status);
        }
        self->psftp_last_error_code = error_code;

        bool security_related = polar_sdk_psftp_prepare_failure_is_security_related(
            prepare_status,
            polar_psftp_security_ready(self));
        self->psftp_last_security_related_failure = security_related;

        if (!retried_after_reconnect && security_related &&
            polar_psftp_reconnect_after_security_failure(self, POLAR_SDK_PSFTP_RECONNECT_TIMEOUT_MS)) {
            retried_after_reconnect = true;
            continue;
        }

        self->psftp_response_waiting = false;
        return result;
    }
}

static polar_sdk_psftp_trans_result_t polar_psftp_execute_remove(
    polar_h10_obj_t *self,
    const char *path,
    size_t path_len,
    uint32_t timeout_ms) {
    uint8_t proto_payload[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES];
    size_t proto_payload_len = 0;
    if (!polar_sdk_psftp_encode_remove_operation(path, path_len, proto_payload, sizeof(proto_payload), &proto_payload_len)) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    uint8_t response[64] = {0};
    size_t response_len = 0;
    return polar_psftp_execute_proto(
        self,
        proto_payload,
        proto_payload_len,
        response,
        sizeof(response),
        timeout_ms,
        &response_len);
}

static NORETURN void polar_raise_psftp_remote_error(uint16_t error_code) {
#if POLAR_CFG_ENABLE_PSFTP
    if (error_code == protocol_PbPFtpError_TIMEOUT) {
        mp_raise_msg_varg(&polar_type_TimeoutError, MP_ERROR_TEXT("PSFTP error code %u"), (unsigned)error_code);
    }
    if (error_code == protocol_PbPFtpError_NO_SUCH_FILE_OR_DIRECTORY ||
        error_code == protocol_PbPFtpError_SYSTEM_BUSY ||
        error_code == protocol_PbPFtpError_TRY_AGAIN) {
        mp_raise_msg_varg(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP error code %u"), (unsigned)error_code);
    }
#endif
    mp_raise_msg_varg(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP error code %u"), (unsigned)error_code);
}

static NORETURN void polar_raise_for_psftp_result(polar_h10_obj_t *self, polar_sdk_psftp_trans_result_t result) {
    if (result == POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    }
    if (self->psftp_last_security_related_failure) {
        polar_raise_exc(&polar_type_SecurityError, MP_ERROR_TEXT("PSFTP security setup failed"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_MISSING_CHARACTERISTICS) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP characteristics missing"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_NOTIFY_TIMEOUT ||
        result == POLAR_SDK_PSFTP_TRANS_WRITE_TIMEOUT ||
        result == POLAR_SDK_PSFTP_TRANS_RESPONSE_TIMEOUT) {
        polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PSFTP timeout"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_NOTIFY_ATT_REJECTED || result == POLAR_SDK_PSFTP_TRANS_WRITE_ATT_REJECTED) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP ATT rejected"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_OVERFLOW) {
        polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("PSFTP response overflow"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_SEQUENCE_ERROR) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP sequence error"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_PROTOCOL_ERROR) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP protocol error"));
    }
    if (result == POLAR_SDK_PSFTP_TRANS_REMOTE_ERROR) {
        polar_raise_psftp_remote_error(self->psftp_last_error_code);
    }

    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("PSFTP transport failure"));
}

static void polar_h10_recording_query_status_or_raise(polar_h10_obj_t *self) {
    uint8_t response[96];
    size_t response_len = 0;
    polar_sdk_psftp_trans_result_t result = polar_psftp_execute_query(
        self,
        POLAR_SDK_PSFTP_QUERY_REQUEST_RECORDING_STATUS,
        NULL,
        0,
        response,
        sizeof(response),
        POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS,
        &response_len);
    if (result != POLAR_SDK_PSFTP_TRANS_OK) {
        polar_raise_for_psftp_result(self, result);
    }

    bool recording_on = false;
    size_t recording_id_len = 0;
    char recording_id[POLAR_SDK_H10_RECORDING_ID_MAX_BYTES + 1u];
    if (!polar_sdk_psftp_decode_h10_recording_status_result(
            response,
            response_len,
            &recording_on,
            recording_id,
            sizeof(recording_id),
            &recording_id_len)) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("failed to decode recording status"));
    }

    if (!recording_on) {
        polar_h10_recording_clear_local_state(self);
        return;
    }

    bool keep_local_config = self->recording_hr_active &&
        self->recording_hr_config_known &&
        strlen(self->recording_hr_id) == recording_id_len &&
        memcmp(self->recording_hr_id, recording_id, recording_id_len) == 0;
    self->recording_hr_active = true;
    self->recording_hr_config_known = keep_local_config;
    polar_h10_recording_store_id(self, recording_id, recording_id_len);
}

static NORETURN void polar_raise_for_psftp_rx_result(polar_h10_obj_t *self, polar_sdk_psftp_rx_result_t result) {
    if (result == POLAR_SDK_PSFTP_RX_ERROR_FRAME) {
        polar_raise_psftp_remote_error(self->psftp_last_error_code);
    }
    if (result == POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP sequence error"));
    }
    if (result == POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PSFTP protocol error"));
    }
    if (result == POLAR_SDK_PSFTP_RX_OVERFLOW) {
        polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("PSFTP response overflow"));
    }

    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("PSFTP transport failure"));
}

static void polar_session_stop_all_listeners(polar_h10_obj_t *self) {
    if (self->hr_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->hr_notification);
        self->hr_notification_listening = false;
    }
    if (self->pmd_cp_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->pmd_cp_notification);
        self->pmd_cp_notification_listening = false;
    }
    if (self->pmd_data_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->pmd_data_notification);
        self->pmd_data_notification_listening = false;
    }
    if (self->psftp_mtu_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->psftp_mtu_notification);
        self->psftp_mtu_notification_listening = false;
    }
    if (self->psftp_d2h_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->psftp_d2h_notification);
        self->psftp_d2h_notification_listening = false;
    }
}

static void polar_session_reset_feature_state(polar_h10_obj_t *self) {
    self->hr_enabled = false;
    self->hr_cfg_pending = false;
    self->hr_cfg_done = false;
    self->hr_cfg_att_status = ATT_ERROR_SUCCESS;
    polar_sdk_hr_reset(&self->hr_state);
    self->hr_consumed_seq = 0;

    self->pmd_cfg_pending = false;
    self->pmd_cfg_done = false;
    self->pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    self->pmd_write_pending = false;
    self->pmd_write_done = false;
    self->pmd_write_att_status = ATT_ERROR_SUCCESS;
    self->pmd_cp_response_waiting = false;
    self->pmd_cp_response_done = false;
    self->pmd_cp_response_expected_opcode = 0;
    self->pmd_cp_response_expected_type = 0;
    self->pmd_cp_response_status = 0xff;
    self->pmd_cp_response_more = 0;
    self->pmd_cp_response_params_len = 0;
    self->hr_enabled = false;
    self->hr_raw_last_len = 0;
    self->hr_raw_consumed_seq = self->hr_raw_seq;
    self->ecg_enabled = false;
    self->imu_enabled = false;
    self->recording_hr_active = false;
    self->recording_hr_config_known = false;
    self->recording_hr_interval_s = 1u;
    self->recording_hr_sample_type = POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE;
    self->recording_hr_id[0] = '\0';
    self->recording_ecg_active = false;
    self->recording_acc_active = false;
    polar_ecg_ring_reset(self);
    polar_imu_ring_reset(self);

    polar_download_blob_reset(self);

    self->psftp_mtu_enabled = false;
    self->psftp_d2h_enabled = false;
    self->psftp_cfg_pending = false;
    self->psftp_cfg_done = false;
    self->psftp_cfg_att_status = ATT_ERROR_SUCCESS;
    self->psftp_write_pending = false;
    self->psftp_write_done = false;
    self->psftp_write_att_status = ATT_ERROR_SUCCESS;
    self->psftp_response_waiting = false;
    self->psftp_response_done = false;
    self->psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    self->psftp_last_att_status = ATT_ERROR_SUCCESS;
    self->psftp_last_error_code = 0;
    self->psftp_last_response_bytes = 0;
    self->psftp_last_security_related_failure = false;
    polar_sdk_psftp_rx_reset(&self->psftp_rx_state, NULL, 0);

    self->runtime_link.conn_update_pending = false;
    self->mtu_exchange_pending = false;
    self->mtu_exchange_done = false;
    self->att_mtu = ATT_DEFAULT_MTU;
}

static void polar_link_on_connected_ready(void *ctx, const polar_sdk_link_event_t *event) {
    (void)event;
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;

    polar_session_reset_feature_state(self);

    uint8_t conn_update_err = gap_update_connection_parameters(
        self->runtime_link.conn_handle,
        POLAR_SDK_POST_CONN_INTERVAL_MIN,
        POLAR_SDK_POST_CONN_INTERVAL_MAX,
        POLAR_SDK_POST_CONN_LATENCY,
        POLAR_SDK_POST_CONN_SUPERVISION_TIMEOUT_10MS);
    self->runtime_link.conn_update_pending = (conn_update_err == ERROR_CODE_SUCCESS);
    if (conn_update_err != ERROR_CODE_SUCCESS) {
        self->runtime_link.last_hci_status = conn_update_err;
    }

    polar_discovery_start(self);
}

static void polar_link_on_disconnected(void *ctx, const polar_sdk_link_event_t *event) {
    (void)event;
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_session_stop_all_listeners(self);
    polar_session_reset_feature_state(self);
}

static bool polar_adv_runtime_is_scanning(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->runtime_link.state == POLAR_SDK_STATE_SCANNING;
}

static void polar_adv_runtime_on_report(void *ctx, const polar_sdk_btstack_adv_report_t *report) {
    (void)report;
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->adv_reports_total += 1;
}

static void polar_adv_runtime_on_match(void *ctx, const polar_sdk_btstack_adv_report_t *report) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->adv_match_total += 1;
    memcpy(self->peer_addr, report->addr, sizeof(bd_addr_t));
    self->peer_addr_type = report->addr_type;
    polar_clear_local_bond_for_addr_once(self, report->addr_type, report->addr);
}

static int polar_adv_runtime_stop_scan(void *ctx) {
    (void)ctx;
    gap_stop_scan();
    return ERROR_CODE_SUCCESS;
}

static int polar_adv_runtime_connect(void *ctx, const uint8_t *addr, uint8_t addr_type) {
    (void)ctx;
    bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    return gap_connect(a, addr_type);
}

static void polar_adv_runtime_on_connect_error(void *ctx, int status) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->runtime_link.last_hci_status = (uint8_t)status;
    polar_mark_attempt_failed(self);
}

static void polar_hci_dispatch_on_adv_report(void *ctx, const polar_sdk_btstack_adv_report_t *adv_report) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;

    polar_sdk_btstack_scan_filter_t filter = {
        .use_addr = false,
        .addr = {0},
        .use_name_prefix = false,
        .name_prefix = 0,
        .name_prefix_len = 0,
        .use_name_contains_pair = false,
        .name_contains_a = 0,
        .name_contains_b = 0,
    };

    if (self->addr != mp_const_none) {
        const char *addr_str = mp_obj_str_get_str(self->addr);
        bd_addr_t target_addr;
        if (sscanf_bd_addr(addr_str, target_addr) != 0) {
            filter.use_addr = true;
            memcpy(filter.addr, target_addr, sizeof(bd_addr_t));
        }
    } else if (self->name_prefix != mp_const_none) {
        size_t prefix_len = 0;
        const uint8_t *prefix = (const uint8_t *)mp_obj_str_get_data(self->name_prefix, &prefix_len);
        filter.use_name_prefix = true;
        filter.name_prefix = prefix;
        filter.name_prefix_len = prefix_len;
    }

    polar_sdk_btstack_adv_runtime_ops_t ops = {
        .ctx = self,
        .is_scanning = polar_adv_runtime_is_scanning,
        .on_report = polar_adv_runtime_on_report,
        .on_match = polar_adv_runtime_on_match,
        .stop_scan = polar_adv_runtime_stop_scan,
        .connect = polar_adv_runtime_connect,
        .on_connect_error = polar_adv_runtime_on_connect_error,
    };
    (void)polar_sdk_btstack_adv_runtime_on_report(
        &self->runtime_link,
        &filter,
        adv_report,
        ERROR_CODE_SUCCESS,
        &ops);
}

static void polar_hci_dispatch_on_link_event(void *ctx, const polar_sdk_link_event_t *link_event) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_sdk_runtime_context_link_ops_t ops = {
        .ctx = self,
        .on_connected_ready = polar_link_on_connected_ready,
        .on_disconnected = polar_link_on_disconnected,
        .on_conn_update_complete = NULL,
    };
    (void)polar_sdk_runtime_context_handle_link_event(
        &self->runtime_link,
        HCI_CON_HANDLE_INVALID,
        link_event,
        self->user_disconnect_requested,
        self->connect_intent,
        &ops);
}

static void polar_hci_dispatch_on_sm_event(void *ctx, const polar_sdk_sm_event_t *sm_event) {
    (void)ctx;
    (void)sm_event;
}

static void polar_hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_h10_obj_t *self = polar_active_h10;
    if (self == NULL) {
        return;
    }

    self->hci_events_total += 1;

    polar_sdk_btstack_dispatch_ops_t ops = {
        .ctx = self,
        .on_adv_report = polar_hci_dispatch_on_adv_report,
        .on_link_event = polar_hci_dispatch_on_link_event,
        .on_sm_event = polar_hci_dispatch_on_sm_event,
    };
    (void)polar_sdk_btstack_dispatch_event(packet_type, packet, &ops);
}

static void polar_sm_on_just_works_request(void *ctx, uint16_t handle) {
    (void)ctx;
    sm_just_works_confirm(handle);
}

static void polar_sm_on_numeric_comparison_request(void *ctx, uint16_t handle) {
    (void)ctx;
    sm_numeric_comparison_confirm(handle);
}

static void polar_sm_on_authorization_request(void *ctx, uint16_t handle) {
    (void)ctx;
    // Current embedded policy is permissive: auto-grant authorization requests.
    sm_authorization_grant(handle);
}

static void polar_sm_on_pairing_complete(void *ctx, const polar_sdk_sm_event_t *event) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->sm_pairing_complete_total += 1;
    self->sm_last_pairing_status = event->status;
    self->sm_last_pairing_reason = event->reason;
}

static void polar_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_h10_obj_t *self = polar_active_h10;
    if (self == NULL) {
        return;
    }

    polar_sdk_sm_event_t sm_event;
    if (!polar_sdk_btstack_decode_sm_event(packet_type, packet, &sm_event)) {
        return;
    }

    polar_sdk_sm_control_ops_t ops = {
        .ctx = self,
        .on_just_works_request = polar_sm_on_just_works_request,
        .on_numeric_comparison_request = polar_sm_on_numeric_comparison_request,
        .on_authorization_request = polar_sm_on_authorization_request,
        .on_pairing_complete = polar_sm_on_pairing_complete,
    };
    (void)polar_sdk_sm_control_apply(
        &sm_event,
        self->runtime_link.conn_handle,
        HCI_CON_HANDLE_INVALID,
        &ops);
}

static polar_sdk_discovery_snapshot_t polar_discovery_snapshot_from_obj(polar_h10_obj_t *self) {
    polar_sdk_discovery_snapshot_t snapshot = {
        .stage = self->discovery_stage,
        .required_services_mask = self->required_services_mask,
        .hr_service_found = self->hr_service_found,
        .pmd_service_found = self->pmd_service_found,
        .psftp_service_found = self->psftp_service_found,
        .hr_measurement_handle = self->hr_measurement_handle,
        .pmd_cp_handle = self->pmd_cp_handle,
        .pmd_data_handle = self->pmd_data_handle,
        .psftp_mtu_handle = self->psftp_mtu_handle,
        .psftp_d2h_handle = self->psftp_d2h_handle,
        .psftp_h2d_handle = self->psftp_h2d_handle,
    };
    return snapshot;
}

static void polar_discovery_runtime_set_stage(void *ctx, polar_sdk_discovery_stage_t stage) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->discovery_stage = stage;
}

static void polar_discovery_runtime_mark_ready(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->runtime_link.state = POLAR_SDK_STATE_READY;
    self->connect_success_total += 1;
}

static void polar_discovery_runtime_mark_att_fail(void *ctx, uint8_t att_status) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->last_att_status = att_status;
    polar_mark_attempt_failed(self);
}

static void polar_discovery_runtime_mark_hci_fail(void *ctx, uint8_t hci_status) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->runtime_link.last_hci_status = hci_status;
    polar_mark_attempt_failed(self);
}

static void polar_discovery_advance(polar_h10_obj_t *self, uint8_t att_status) {
    polar_sdk_discovery_snapshot_t snapshot = polar_discovery_snapshot_from_obj(self);
    polar_sdk_discovery_runtime_ops_t ops = {
        .ctx = self,
        .set_stage = polar_discovery_runtime_set_stage,
        .mark_ready = polar_discovery_runtime_mark_ready,
        .mark_att_fail = polar_discovery_runtime_mark_att_fail,
        .mark_hci_fail = polar_discovery_runtime_mark_hci_fail,
        .discover_hr_chars = polar_discovery_cmd_hr_chars,
        .discover_pmd_chars = polar_discovery_cmd_pmd_chars,
        .discover_psftp_chars = polar_discovery_cmd_psftp_chars,
    };
    polar_sdk_discovery_runtime_on_query_complete(
        &snapshot,
        POLAR_SDK_SERVICE_HR,
        POLAR_SDK_SERVICE_ECG,
        POLAR_SDK_SERVICE_PSFTP,
        att_status,
        ATT_ERROR_ATTRIBUTE_NOT_FOUND,
        ERROR_CODE_SUCCESS,
        &ops);
}

static uint8_t polar_discovery_cmd_hr_chars(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return gatt_client_discover_characteristics_for_service(
        polar_hci_packet_handler_and_discovery,
        self->runtime_link.conn_handle,
        &self->hr_service);
}

static uint8_t polar_discovery_cmd_pmd_chars(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return gatt_client_discover_characteristics_for_service(
        polar_hci_packet_handler_and_discovery,
        self->runtime_link.conn_handle,
        &self->pmd_service);
}

static uint8_t polar_discovery_cmd_psftp_chars(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return gatt_client_discover_characteristics_for_service(
        polar_hci_packet_handler_and_discovery,
        self->runtime_link.conn_handle,
        &self->psftp_service);
}

static void polar_discovery_handle_service_result(polar_h10_obj_t *self, const gatt_client_service_t *svc) {
    polar_sdk_disc_service_kind_t kind = polar_sdk_btstack_classify_service(
        svc->uuid16,
        svc->uuid128,
        ORG_BLUETOOTH_SERVICE_HEART_RATE,
        POLAR_UUID16_PSFTP_SERVICE,
        POLAR_SDK_UUID_PMD_SERVICE_BE);

    if (kind == POLAR_SDK_DISC_SERVICE_HR) {
        self->hr_service = *svc;
    } else if (kind == POLAR_SDK_DISC_SERVICE_PMD) {
        self->pmd_service = *svc;
    } else if (kind == POLAR_SDK_DISC_SERVICE_PSFTP) {
        self->psftp_service = *svc;
    }

    polar_sdk_discovery_apply_service_kind(
        kind,
        &self->hr_service_found,
        &self->pmd_service_found,
        &self->psftp_service_found);
}

static void polar_discovery_handle_char_result(polar_h10_obj_t *self, const gatt_client_characteristic_t *chr) {
    polar_sdk_disc_char_kind_t kind = polar_sdk_btstack_classify_char(
        self->discovery_stage,
        chr->uuid16,
        chr->uuid128,
        ORG_BLUETOOTH_CHARACTERISTIC_HEART_RATE_MEASUREMENT,
        POLAR_SDK_UUID_PMD_CP_BE,
        POLAR_SDK_UUID_PMD_DATA_BE,
        POLAR_SDK_UUID_PSFTP_MTU_BE,
        POLAR_SDK_UUID_PSFTP_D2H_BE,
        POLAR_SDK_UUID_PSFTP_H2D_BE);

    if (kind == POLAR_SDK_DISC_CHAR_HR_MEAS) {
        self->hr_measurement_char = *chr;
    } else if (kind == POLAR_SDK_DISC_CHAR_PMD_CP) {
        self->pmd_cp_char = *chr;
    } else if (kind == POLAR_SDK_DISC_CHAR_PMD_DATA) {
        self->pmd_data_char = *chr;
    } else if (kind == POLAR_SDK_DISC_CHAR_PSFTP_MTU) {
        self->psftp_mtu_char = *chr;
        self->psftp_mtu_char_found = true;
    } else if (kind == POLAR_SDK_DISC_CHAR_PSFTP_D2H) {
        self->psftp_d2h_char = *chr;
        self->psftp_d2h_char_found = true;
    } else if (kind == POLAR_SDK_DISC_CHAR_PSFTP_H2D) {
        self->psftp_h2d_char = *chr;
        self->psftp_h2d_char_found = true;
    }

    polar_sdk_discovery_apply_char_kind(
        kind,
        chr->value_handle,
        &self->hr_char_found,
        &self->hr_measurement_handle,
        &self->pmd_cp_char_found,
        &self->pmd_cp_handle,
        &self->pmd_data_char_found,
        &self->pmd_data_handle,
        &self->psftp_mtu_handle,
        &self->psftp_d2h_handle,
        &self->psftp_h2d_handle);
}

static bool polar_handle_gatt_route_event(polar_h10_obj_t *self, const polar_sdk_btstack_gatt_route_result_t *route) {
    if (route->kind == POLAR_SDK_GATT_ROUTE_MTU_EVENT) {
        if (self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID || route->mtu.handle == self->runtime_link.conn_handle) {
            self->att_mtu = route->mtu.mtu;
            self->mtu_exchange_done = true;
            self->mtu_exchange_pending = false;
            self->mtu_exchange_total += 1;
        }
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_HR_VALUE) {
        self->hr_value_events_total += 1;
        if (route->value.notification) {
            self->hr_notifications_total += 1;
        } else {
            self->hr_indications_total += 1;
        }

        size_t raw_len = route->value.value_len;
        if (raw_len > sizeof(self->hr_raw_last)) {
            raw_len = sizeof(self->hr_raw_last);
        }
        if (raw_len > 0) {
            memcpy(self->hr_raw_last, route->value.value, raw_len);
        }
        self->hr_raw_last_len = (uint8_t)raw_len;
        self->hr_raw_seq += 1;

        polar_hr_parse_notification(self, route->value.value, route->value.value_len);
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_PMD_CP_VALUE) {
        polar_pmd_parse_cp_notification(self, route->value.value, route->value.value_len);
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_PMD_DATA_VALUE) {
        polar_pmd_parse_data(self, route->value.value, route->value.value_len);
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_PSFTP_MTU_VALUE) {
        self->psftp_rx_frames_total += 1;

        if (self->psftp_response_waiting) {
            if (self->download_handle_active) {
                polar_download_blob_compact_for_frame(self, route->value.value_len);
            }

            polar_sdk_psftp_rx_result_t r = polar_sdk_psftp_rx_feed_frame(
                &self->psftp_rx_state,
                route->value.value,
                route->value.value_len);
            self->psftp_response_result = r;

            if (self->download_handle_active &&
                (r == POLAR_SDK_PSFTP_RX_MORE || r == POLAR_SDK_PSFTP_RX_COMPLETE) &&
                route->value.value_len > 1u) {
                self->psftp_last_response_bytes += (uint32_t)(route->value.value_len - 1u);
            }

            if (r == POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR) {
                self->psftp_rx_seq_errors_total += 1;
                self->psftp_last_error_code = POLAR_SDK_PSFTP_PFTP_AIR_PACKET_LOST_ERROR;
                self->psftp_response_done = true;
                self->psftp_response_waiting = false;
                return true;
            }
            if (r == POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR) {
                self->psftp_protocol_errors_total += 1;
                self->psftp_response_done = true;
                self->psftp_response_waiting = false;
                return true;
            }
            if (r == POLAR_SDK_PSFTP_RX_OVERFLOW) {
                self->psftp_overflow_errors_total += 1;
                self->psftp_response_done = true;
                self->psftp_response_waiting = false;
                return true;
            }
            if (r == POLAR_SDK_PSFTP_RX_ERROR_FRAME) {
                self->psftp_last_error_code = self->psftp_rx_state.error_code;
                self->psftp_response_done = true;
                self->psftp_response_waiting = false;
                return true;
            }
            if (r == POLAR_SDK_PSFTP_RX_COMPLETE) {
                self->psftp_response_done = true;
                self->psftp_response_waiting = false;
                return true;
            }
        }
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_PSFTP_D2H_VALUE) {
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_HR_UNMATCHED_VALUE) {
        self->hr_unmatched_value_events_total += 1;
        return true;
    }

    if (route->kind == POLAR_SDK_GATT_ROUTE_QUERY_COMPLETE) {
        polar_sdk_gatt_query_slot_t slots[] = {
            {
                .pending = &self->hr_cfg_pending,
                .done = &self->hr_cfg_done,
                .att_status = &self->hr_cfg_att_status,
                .update_last_att_status = false,
            },
            {
                .pending = &self->pmd_cfg_pending,
                .done = &self->pmd_cfg_done,
                .att_status = &self->pmd_cfg_att_status,
                .update_last_att_status = true,
            },
            {
                .pending = &self->pmd_write_pending,
                .done = &self->pmd_write_done,
                .att_status = &self->pmd_write_att_status,
                .update_last_att_status = true,
            },
            {
                .pending = &self->psftp_cfg_pending,
                .done = &self->psftp_cfg_done,
                .att_status = &self->psftp_cfg_att_status,
                .update_last_att_status = true,
            },
            {
                .pending = &self->psftp_write_pending,
                .done = &self->psftp_write_done,
                .att_status = &self->psftp_write_att_status,
                .update_last_att_status = true,
            },
        };
        if (polar_sdk_gatt_apply_query_complete(
                route->query_complete_att_status,
                slots,
                MP_ARRAY_SIZE(slots),
                &self->last_att_status)) {
            return true;
        }

        if (self->runtime_link.state == POLAR_SDK_STATE_DISCOVERING && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID) {
            polar_discovery_advance(self, route->query_complete_att_status);
        }
        return true;
    }

    return false;
}

static void polar_handle_discovery_decode_apply(polar_h10_obj_t *self, uint8_t packet_type, uint8_t *packet) {
    if (self->runtime_link.state != POLAR_SDK_STATE_DISCOVERING || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    polar_sdk_discovery_btstack_result_t disc_result;
    if (!polar_sdk_discovery_btstack_decode_result(packet_type, packet, self->discovery_stage, &disc_result)) {
        return;
    }

    if (disc_result.kind == POLAR_SDK_DISCOVERY_BTSTACK_SERVICE_RESULT) {
        polar_discovery_handle_service_result(self, &disc_result.service);
    } else if (disc_result.kind == POLAR_SDK_DISCOVERY_BTSTACK_CHAR_RESULT) {
        polar_discovery_handle_char_result(self, &disc_result.characteristic);
    }
}

static void polar_hci_packet_handler_and_discovery(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    // Main HCI path.
    polar_hci_packet_handler(packet_type, channel, packet, size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_h10_obj_t *self = polar_active_h10;
    if (self == NULL) {
        return;
    }

    polar_sdk_btstack_gatt_route_state_t route_state = {
        .conn_handle = self->runtime_link.conn_handle,
        .connected = self->runtime_link.connected,
        .hr_value_handle = self->hr_measurement_handle,
        .hr_enabled = self->hr_enabled,
        .pmd_cp_value_handle = self->pmd_cp_handle,
        .pmd_cp_listening = self->pmd_cp_notification_listening,
        .pmd_data_value_handle = self->pmd_data_handle,
        .ecg_enabled = self->ecg_enabled,
        .psftp_mtu_value_handle = self->psftp_mtu_handle,
        .psftp_mtu_listening = self->psftp_mtu_notification_listening,
        .psftp_d2h_value_handle = self->psftp_d2h_handle,
        .psftp_d2h_listening = self->psftp_d2h_notification_listening,
    };

    polar_sdk_btstack_gatt_route_result_t route;
    if (!polar_sdk_btstack_route_gatt_event(packet_type, packet, &route_state, &route)) {
        polar_handle_discovery_decode_apply(self, packet_type, packet);
        return;
    }

    if (!polar_handle_gatt_route_event(self, &route)) {
        polar_handle_discovery_decode_apply(self, packet_type, packet);
    }
}

static void polar_discovery_start(polar_h10_obj_t *self) {
    polar_clear_discovery_cache(self);
    self->discovery_stage = POLAR_SDK_DISC_STAGE_SERVICES;

    uint8_t err = gatt_client_discover_primary_services(polar_hci_packet_handler_and_discovery, self->runtime_link.conn_handle);
    if (err != ERROR_CODE_SUCCESS) {
        self->runtime_link.last_hci_status = err;
        polar_mark_attempt_failed(self);
    }
}

static bool polar_wait_until_ready_or_fail(polar_h10_obj_t *self, uint32_t budget_ms) {
    uint32_t start_ms = polar_now_ms();
    while (!polar_elapsed_ms(start_ms, budget_ms)) {
        if (self->runtime_link.state == POLAR_SDK_STATE_READY) {
            return true;
        }
        if (self->runtime_link.attempt_failed) {
            return false;
        }
        mp_event_wait_ms(10);
    }
    return self->runtime_link.state == POLAR_SDK_STATE_READY;
}

static void polar_wait_conn_update_settle(polar_h10_obj_t *self, uint32_t timeout_ms) {
    uint32_t start_ms = polar_now_ms();
    while (self->runtime_link.conn_update_pending && !polar_elapsed_ms(start_ms, timeout_ms)) {
        if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
            break;
        }
        mp_event_wait_ms(10);
    }
}

static uint32_t polar_cleanup_now_ms(void *ctx) {
    (void)ctx;
    return polar_now_ms();
}

static void polar_cleanup_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    mp_event_wait_ms(ms);
}

static int polar_cleanup_stop_scan(void *ctx) {
    (void)ctx;
    gap_stop_scan();
    return ERROR_CODE_SUCCESS;
}

static int polar_cleanup_cancel_connect(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    uint8_t err = gap_connect_cancel();
    if (err != ERROR_CODE_SUCCESS) {
        self->runtime_link.last_hci_status = err;
    }
    return err;
}

static int polar_cleanup_disconnect(void *ctx, uint16_t conn_handle) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    uint8_t err = gap_disconnect(conn_handle);
    if (err != ERROR_CODE_SUCCESS) {
        self->runtime_link.last_hci_status = err;
    }
    return err;
}

static bool polar_cleanup_is_link_down(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return !self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID;
}

static void polar_cleanup_stop_all_listeners_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_session_stop_all_listeners(self);
}

static void polar_cleanup_reset_session_state_cb(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_session_reset_feature_state(self);
}

static void polar_cleanup_after_attempt(polar_h10_obj_t *self) {
    polar_sdk_runtime_context_cleanup_ops_t ops = {
        .ctx = self,
        .now_ms = polar_cleanup_now_ms,
        .sleep_ms = polar_cleanup_sleep_ms,
        .stop_scan = polar_cleanup_stop_scan,
        .cancel_connect = polar_cleanup_cancel_connect,
        .disconnect = polar_cleanup_disconnect,
        .is_link_down = polar_cleanup_is_link_down,
        .stop_all_listeners = polar_cleanup_stop_all_listeners_cb,
        .reset_session_state = polar_cleanup_reset_session_state_cb,
    };
    polar_sdk_runtime_context_cleanup_attempt(
        &self->runtime_link,
        HCI_CON_HANDLE_INVALID,
        POLAR_SDK_DISCONNECT_WAIT_MS,
        POLAR_SDK_RUNTIME_STATE_RECOVERING,
        &ops);
}

static void polar_ensure_ble_ready(void) {
    // Ensure the high-level bluetooth.BLE singleton exists so modbluetooth ringbuf
    // callbacks always have a valid object, even though we don't use Python IRQs here.
    if (MP_STATE_VM(bluetooth) == MP_OBJ_NULL) {
        mp_obj_t bt_module = mp_import_name(MP_QSTR_bluetooth, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t ble_type = mp_load_attr(bt_module, MP_QSTR_BLE);
        (void)mp_call_function_0(ble_type);
    }

    bool bluetooth_just_activated = false;
    if (!mp_bluetooth_is_active()) {
        int err = mp_bluetooth_init();
        if (err != 0) {
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("failed to initialize Bluetooth stack"));
        }
        bluetooth_just_activated = true;
    }

    // First operations right after stack activation can be timing-sensitive.
    // Apply a one-time settle window per activation so callers don't need
    // startup sleeps in scripts.
    if (bluetooth_just_activated) {
        mp_event_wait_ms(1200);
    }

    polar_apply_btstack_host_identity_keys();

    // Keep Security Manager defaults aligned with probe/examples.
    // Apply after BLE stack is active so all subsequent pairing requests follow
    // the same bonding + secure-connection policy.
    polar_sdk_btstack_sm_apply_default_auth_policy();

    // Ensure GATT client is initialized for our direct btstack usage.
    // On MicroPython BTstack ports with GATT client enabled, mp_bluetooth_init()
    // performs gatt_client_init(). We keep that integration contract and avoid a
    // second explicit init here.
    #if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT
    polar_gatt_client_initialized = true;
    #else
    if (!polar_gatt_client_initialized) {
        gatt_client_init();
        polar_gatt_client_initialized = true;
    }
    #endif
    // Align direct GATT-client sequencing with the working pico-sdk probe.
    // The probe keeps BTstack's default automatic MTU exchange enabled, and
    // the on-air comparison showed that the probe reaches an early ATT MTU
    // exchange phase that the MPY path was skipping. Re-enable auto MTU
    // negotiation here after MicroPython's init path disables it so discovery
    // and the first secured PSFTP operation follow the probe more closely.
    gatt_client_mtu_enable_auto_negotiation(true);
}

static void polar_register_hci_handler_refresh(void) {
    // Refresh registration each connect attempt to survive soft-reset / stack
    // reinit sequences where our C static state may outlive btstack registrations.
    if (polar_hci_event_cb_registered) {
        hci_remove_event_handler(&polar_hci_event_cb);
        polar_hci_event_cb_registered = false;
    }
    if (polar_sm_event_cb_registered) {
        sm_remove_event_handler(&polar_sm_event_cb);
        polar_sm_event_cb_registered = false;
    }

    polar_hci_event_cb.callback = &polar_hci_packet_handler_and_discovery;
    hci_add_event_handler(&polar_hci_event_cb);
    polar_hci_event_cb_registered = true;

    polar_sm_event_cb.callback = &polar_sm_packet_handler;
    sm_add_event_handler(&polar_sm_event_cb);
    polar_sm_event_cb_registered = true;
}

static uint32_t polar_transport_now_ms(void *ctx) {
    (void)ctx;
    return polar_now_ms();
}

static void polar_transport_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    mp_event_wait_ms((mp_uint_t)ms);
}

static bool polar_transport_start_attempt(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    self->runtime_link.attempt_failed = false;
    self->runtime_link.conn_update_pending = false;
    self->runtime_link.last_hci_status = ERROR_CODE_SUCCESS;
    self->last_att_status = ATT_ERROR_SUCCESS;
    self->psftp_bond_clear_done = false;
    self->runtime_link.state = POLAR_SDK_STATE_SCANNING;

    if (self->addr != mp_const_none) {
        bd_addr_t target_addr;
        const char *addr_str = mp_obj_str_get_str(self->addr);
        if (sscanf_bd_addr(addr_str, target_addr) != 0) {
            polar_clear_local_bond_for_addr_once(self, BD_ADDR_TYPE_UNKNOWN, target_addr);
        }
    }

    gap_set_scan_params(1, POLAR_SDK_SCAN_INTERVAL_UNITS, POLAR_SDK_SCAN_WINDOW_UNITS, 0);
    gap_start_scan();
    return true;
}

static polar_sdk_transport_attempt_result_t polar_transport_wait_attempt(void *ctx, uint32_t attempt_budget_ms) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (polar_wait_until_ready_or_fail(self, attempt_budget_ms)) {
        return POLAR_SDK_TRANSPORT_ATTEMPT_READY;
    }
    if (self->runtime_link.attempt_failed) {
        return POLAR_SDK_TRANSPORT_ATTEMPT_FAILED;
    }
    return POLAR_SDK_TRANSPORT_ATTEMPT_TIMEOUT;
}

static void polar_transport_cleanup_after_attempt(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_cleanup_after_attempt(self);
    self->runtime_link.state = POLAR_SDK_STATE_RECOVERING;
}

static void polar_transport_on_connect_ready(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    // Give post-connect parameter update a short chance to complete,
    // so HR streaming starts on settled link parameters.
    polar_wait_conn_update_settle(self, 1200);
}

static polar_sdk_transport_connect_result_t polar_transport_connect_with_timeout(
    polar_h10_obj_t *self,
    uint32_t timeout_ms) {
    self->connect_intent = true;
    self->user_disconnect_requested = false;

    polar_sdk_connect_policy_t connect_policy = {
        .timeout_ms = timeout_ms,
        .attempt_slice_ms = POLAR_SDK_CONNECT_ATTEMPT_SLICE_MS,
    };
    polar_sdk_transport_connect_ops_t transport_ops = {
        .ctx = self,
        .now_ms = polar_transport_now_ms,
        .sleep_ms = polar_transport_sleep_ms,
        .start_attempt = polar_transport_start_attempt,
        .wait_attempt = polar_transport_wait_attempt,
        .cleanup_after_attempt = polar_transport_cleanup_after_attempt,
        .on_connect_ready = polar_transport_on_connect_ready,
    };
    polar_sdk_transport_connect_stats_t transport_stats;

    polar_sdk_transport_connect_result_t connect_result = polar_sdk_transport_connect_blocking(
        &connect_policy,
        &transport_ops,
        &transport_stats);

    self->connect_attempts_total += transport_stats.attempts_total;
    self->reconnect_backoff_events += transport_stats.backoff_events_total;
    self->connect_intent = false;

    return connect_result;
}

static bool polar_psftp_reconnect_after_security_failure(polar_h10_obj_t *self, uint32_t timeout_ms) {
    if (self == NULL) {
        return false;
    }
    if (polar_active_h10 != NULL && polar_active_h10 != self) {
        return false;
    }

    polar_ensure_ble_ready();
    polar_register_hci_handler_refresh();

    self->user_disconnect_requested = true;
    self->connect_intent = false;
    polar_cleanup_after_attempt(self);
    self->runtime_link.connected = false;
    self->runtime_link.conn_handle = HCI_CON_HANDLE_INVALID;
    self->runtime_link.conn_update_pending = false;
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;

    if (polar_active_h10 == NULL) {
        polar_active_h10 = self;
    }

    polar_capability_mask_t saved_caps_mask = self->required_capabilities_mask;
    uint8_t saved_services_mask = self->required_services_mask;
    self->required_capabilities_mask |= POLAR_CAP_PSFTP_READ;
    self->required_services_mask |= POLAR_SDK_SERVICE_PSFTP;

    polar_sdk_transport_connect_result_t connect_result = polar_transport_connect_with_timeout(self, timeout_ms);
    self->required_capabilities_mask = saved_caps_mask;
    self->required_services_mask = saved_services_mask;
    if (connect_result == POLAR_SDK_TRANSPORT_CONNECT_OK) {
        return true;
    }

    self->runtime_link.conn_update_pending = false;
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;
    if (polar_active_h10 == self) {
        polar_active_h10 = NULL;
    }
    return false;
}

#endif // POLAR_SDK_HAS_BTSTACK

static mp_obj_t polar_h10_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum {
        ARG_addr,
        ARG_name_prefix,
        ARG_required_capabilities,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_name_prefix, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_Polar) } },
        { MP_QSTR_required_capabilities, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_empty_tuple } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    if (parsed_args[ARG_addr].u_obj != mp_const_none && !mp_obj_is_str(parsed_args[ARG_addr].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("addr must be str or None"));
    }
    if (parsed_args[ARG_name_prefix].u_obj != mp_const_none && !mp_obj_is_str(parsed_args[ARG_name_prefix].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("name_prefix must be str or None"));
    }

    polar_capability_mask_t required_caps = 0;
    uint8_t required_services_mask = 0;
    if (!polar_required_capabilities_parse(parsed_args[ARG_required_capabilities].u_obj, &required_caps, &required_services_mask)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid required_capabilities"));
    }

    polar_h10_obj_t *self = mp_obj_malloc(polar_h10_obj_t, type);
    self->addr = parsed_args[ARG_addr].u_obj;
    self->name_prefix = parsed_args[ARG_name_prefix].u_obj;
    self->required_capabilities_mask = required_caps;
    self->required_services_mask = required_services_mask;
    self->hr_stream_format = POLAR_STREAM_FORMAT_DECODED;
    self->ecg_stream_format = POLAR_STREAM_FORMAT_DECODED;
    self->acc_stream_format = POLAR_STREAM_FORMAT_DECODED;
    self->ecg_sample_rate_hz = 130;
    self->acc_sample_rate_hz = 50;
    self->acc_range = POLAR_SDK_IMU_DEFAULT_RANGE_G;
    polar_sdk_runtime_link_init(&self->runtime_link, HCI_CON_HANDLE_INVALID);
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;
    self->runtime_link.connected = false;
    self->connect_calls = 0;
    self->disconnect_calls = 0;

#if POLAR_SDK_HAS_BTSTACK
    self->connect_intent = false;
    self->user_disconnect_requested = false;
    self->runtime_link.conn_handle = HCI_CON_HANDLE_INVALID;
    memset(self->peer_addr, 0, sizeof(self->peer_addr));
    self->peer_addr_type = BD_ADDR_TYPE_UNKNOWN;

    self->runtime_link.attempt_failed = false;
    self->runtime_link.last_hci_status = ERROR_CODE_SUCCESS;
    self->last_att_status = ATT_ERROR_SUCCESS;
    self->runtime_link.last_disconnect_status = ERROR_CODE_SUCCESS;
    self->runtime_link.last_disconnect_reason = 0;

    self->connect_attempts_total = 0;
    self->reconnect_backoff_events = 0;
    self->connect_success_total = 0;

    self->hci_events_total = 0;
    self->adv_reports_total = 0;
    self->adv_match_total = 0;
    self->runtime_link.conn_complete_total = 0;
    self->runtime_link.conn_update_complete_total = 0;

    self->att_mtu = ATT_DEFAULT_MTU;
    self->mtu_exchange_pending = false;
    self->mtu_exchange_done = false;
    self->mtu_exchange_total = 0;

    self->runtime_link.last_conn_interval_units = 0;
    self->runtime_link.last_conn_latency = 0;
    self->runtime_link.last_conn_supervision_timeout_10ms = 0;
    self->runtime_link.last_conn_update_status = ERROR_CODE_SUCCESS;
    self->runtime_link.conn_update_pending = false;

    self->runtime_link.disconnect_events_total = 0;
    self->runtime_link.disconnect_reason_0x08_total = 0;
    self->runtime_link.disconnect_reason_0x3e_total = 0;
    self->runtime_link.disconnect_reason_other_total = 0;

    self->sm_pairing_complete_total = 0;
    self->sm_last_pairing_status = 0;
    self->sm_last_pairing_reason = 0;

    memset(&self->hr_notification, 0, sizeof(self->hr_notification));
    self->hr_notification_listening = false;
    self->hr_enabled = false;
    self->hr_cfg_pending = false;
    self->hr_cfg_done = false;
    self->hr_cfg_att_status = ATT_ERROR_SUCCESS;
    self->hr_notifications_total = 0;
    self->hr_indications_total = 0;
    self->hr_value_events_total = 0;
    self->hr_unmatched_value_events_total = 0;
    self->hr_resubscribe_total = 0;
    self->hr_last_resubscribe_ms = 0;
    self->hr_consumed_seq = 0;
    polar_sdk_hr_reset(&self->hr_state);
    memset(self->hr_raw_last, 0, sizeof(self->hr_raw_last));
    self->hr_raw_last_len = 0;
    self->hr_raw_seq = 0;
    self->hr_raw_consumed_seq = 0;

    memset(&self->pmd_cp_notification, 0, sizeof(self->pmd_cp_notification));
    memset(&self->pmd_data_notification, 0, sizeof(self->pmd_data_notification));
    self->pmd_cp_notification_listening = false;
    self->pmd_data_notification_listening = false;
    self->pmd_cfg_pending = false;
    self->pmd_cfg_done = false;
    self->pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    self->pmd_write_pending = false;
    self->pmd_write_done = false;
    self->pmd_write_att_status = ATT_ERROR_SUCCESS;
    self->pmd_cp_response_waiting = false;
    self->pmd_cp_response_done = false;
    self->pmd_cp_response_expected_opcode = 0;
    self->pmd_cp_response_expected_type = 0;
    self->pmd_cp_response_status = 0xff;
    self->pmd_cp_response_more = 0;
    self->pmd_cp_response_params_len = 0;

    self->ecg_enabled = false;
    polar_sdk_ecg_ring_init(&self->ecg_ring, self->ecg_ring_storage, POLAR_SDK_ECG_RING_BYTES);

    self->imu_enabled = false;
    polar_sdk_imu_ring_init(&self->imu_ring, self->imu_ring_storage, POLAR_SDK_IMU_RING_BYTES);
    self->recording_hr_active = false;
    self->recording_hr_config_known = false;
    self->recording_hr_interval_s = 1u;
    self->recording_hr_sample_type = POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE;
    self->recording_hr_id[0] = '\0';
    self->recording_hr_id_next = 1u;
    self->recording_ecg_active = false;
    self->recording_acc_active = false;
    self->recording_ecg_sample_rate_hz = 130;
    self->recording_acc_sample_rate_hz = 50;
    self->recording_acc_range = POLAR_SDK_IMU_DEFAULT_RANGE_G;

    self->pmd_cp_notifications_total = 0;
    self->pmd_cp_response_total = 0;

    memset(&self->psftp_mtu_notification, 0, sizeof(self->psftp_mtu_notification));
    memset(&self->psftp_d2h_notification, 0, sizeof(self->psftp_d2h_notification));
    self->psftp_mtu_notification_listening = false;
    self->psftp_d2h_notification_listening = false;
    self->psftp_mtu_enabled = false;
    self->psftp_d2h_enabled = false;
    self->psftp_cfg_pending = false;
    self->psftp_cfg_done = false;
    self->psftp_cfg_att_status = ATT_ERROR_SUCCESS;
    self->psftp_write_pending = false;
    self->psftp_write_done = false;
    self->psftp_write_att_status = ATT_ERROR_SUCCESS;
    self->psftp_response_waiting = false;
    self->psftp_response_done = false;
    self->psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    polar_sdk_psftp_rx_reset(&self->psftp_rx_state, NULL, 0);

    self->psftp_tx_frames_total = 0;
    self->psftp_rx_frames_total = 0;
    self->psftp_rx_seq_errors_total = 0;
    self->psftp_protocol_errors_total = 0;
    self->psftp_overflow_errors_total = 0;
    self->psftp_last_error_code = 0;
    self->psftp_last_att_status = ATT_ERROR_SUCCESS;
    self->psftp_last_response_bytes = 0;
    self->psftp_last_security_related_failure = false;
    self->psftp_bond_clear_done = false;

    self->download_handle_active = false;
    self->download_handle_id = 0;
    self->download_handle_next_id = 1;
    self->download_blob = NULL;
    self->download_blob_len = 0;
    self->download_blob_off = 0;
    self->download_blob_alloc = 0;

    polar_clear_discovery_cache(self);
#endif

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t polar_h10_state(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_str(polar_state_strs[self->runtime_link.state], strlen(polar_state_strs[self->runtime_link.state]));
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_state_obj, polar_h10_state);

static mp_obj_t polar_h10_is_connected(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->runtime_link.state == POLAR_SDK_STATE_READY);
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_is_connected_obj, polar_h10_is_connected);

static mp_obj_t polar_h10_set_required_capabilities(mp_obj_t self_in, mp_obj_t caps_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_capability_mask_t caps_mask = 0;
    uint8_t services_mask = 0;
    if (!polar_required_capabilities_parse(caps_in, &caps_mask, &services_mask)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid required_capabilities"));
    }

    self->required_capabilities_mask = caps_mask;
    self->required_services_mask = services_mask;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_set_required_capabilities_obj, polar_h10_set_required_capabilities);

static mp_obj_t polar_h10_required_capabilities(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return polar_capabilities_tuple_from_mask(self->required_capabilities_mask);
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_required_capabilities_obj, polar_h10_required_capabilities);

static mp_obj_t polar_stream_kind_capabilities_dict(
    bool supported,
    bool has_raw,
    bool has_decoded,
    const char *default_format,
    const char *units,
    mp_obj_t decoded_sample_size,
    mp_obj_t config) {
    mp_obj_t dict = mp_obj_new_dict(config == MP_OBJ_NULL ? 5 : 6);
    mp_obj_dict_store(dict, mp_obj_new_str("supported", 9), mp_obj_new_bool(supported));

    mp_obj_t formats = mp_obj_new_list(0, NULL);
    if (has_raw) {
        mp_obj_list_append(formats, mp_obj_new_str("raw", 3));
    }
    if (has_decoded) {
        mp_obj_list_append(formats, mp_obj_new_str("decoded", 7));
    }
    mp_obj_dict_store(dict, mp_obj_new_str("formats", 7), formats);
    mp_obj_dict_store(dict, mp_obj_new_str("default_format", 14), mp_obj_new_str(default_format, strlen(default_format)));
    mp_obj_dict_store(dict, mp_obj_new_str("units", 5), mp_obj_new_str(units, strlen(units)));
    mp_obj_dict_store(dict, mp_obj_new_str("decoded_sample_size_bytes", 25), decoded_sample_size);
    if (config != MP_OBJ_NULL) {
        mp_obj_dict_store(dict, mp_obj_new_str("config", 6), config);
    }
    return dict;
}

static mp_obj_t polar_config_desc_range(mp_int_t min_v, mp_int_t max_v, mp_int_t step_v) {
    mp_obj_t dict = mp_obj_new_dict(4);
    mp_obj_dict_store(dict, mp_obj_new_str("type", 4), mp_obj_new_str("range", 5));
    mp_obj_dict_store(dict, mp_obj_new_str("min", 3), mp_obj_new_int(min_v));
    mp_obj_dict_store(dict, mp_obj_new_str("max", 3), mp_obj_new_int(max_v));
    mp_obj_dict_store(dict, mp_obj_new_str("step", 4), mp_obj_new_int(step_v));
    return dict;
}

static mp_obj_t polar_config_desc_enum(const char *const *choices, size_t choice_count) {
    mp_obj_t dict = mp_obj_new_dict(2);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < choice_count; ++i) {
        mp_obj_list_append(list, mp_obj_new_str(choices[i], strlen(choices[i])));
    }
    mp_obj_dict_store(dict, mp_obj_new_str("type", 4), mp_obj_new_str("enum", 4));
    mp_obj_dict_store(dict, mp_obj_new_str("choices", 7), list);
    return dict;
}

static mp_obj_t polar_capability_bool_dict4(bool control, bool status, bool list, bool delete_) {
    mp_obj_t dict = mp_obj_new_dict(4);
    mp_obj_dict_store(dict, mp_obj_new_str("control", 7), mp_obj_new_bool(control));
    mp_obj_dict_store(dict, mp_obj_new_str("status", 6), mp_obj_new_bool(status));
    mp_obj_dict_store(dict, mp_obj_new_str("list", 4), mp_obj_new_bool(list));
    mp_obj_dict_store(dict, mp_obj_new_str("delete", 6), mp_obj_new_bool(delete_));
    return dict;
}

static mp_obj_t polar_h10_capabilities(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);

    mp_obj_t root = mp_obj_new_dict(6);
    mp_obj_dict_store(root, mp_obj_new_str("schema_version", 14), mp_obj_new_int(0));

    mp_obj_t device = mp_obj_new_dict(5);
    if (self->addr == mp_const_none) {
        mp_obj_dict_store(device, mp_obj_new_str("id", 2), mp_const_none);
        mp_obj_dict_store(device, mp_obj_new_str("address", 7), mp_const_none);
    } else {
        mp_obj_dict_store(device, mp_obj_new_str("id", 2), self->addr);
        mp_obj_dict_store(device, mp_obj_new_str("address", 7), self->addr);
    }
    mp_obj_dict_store(device, mp_obj_new_str("model", 5), mp_obj_new_str("unknown", 7));
    mp_obj_dict_store(device, mp_obj_new_str("family", 6), mp_obj_new_str("polar", 5));
    mp_obj_dict_store(device, mp_obj_new_str("firmware", 8), mp_const_none);
    mp_obj_dict_store(root, mp_obj_new_str("device", 6), device);

    mp_obj_t kinds = mp_obj_new_list(0, NULL);
    mp_obj_t by_kind = mp_obj_new_dict(3);
    if (POLAR_CFG_ENABLE_HR) {
        mp_obj_list_append(kinds, mp_obj_new_str("hr", 2));
        mp_obj_dict_store(
            by_kind,
            mp_obj_new_str("hr", 2),
            polar_stream_kind_capabilities_dict(true, true, true, "decoded", "bpm", mp_obj_new_int(12), MP_OBJ_NULL));
    }
    if (POLAR_CFG_ENABLE_ECG) {
        mp_obj_t ecg_cfg = mp_obj_new_dict(1);
        mp_obj_dict_store(ecg_cfg, mp_obj_new_str("sample_rate_hz", 14), polar_config_desc_range(1, 65535, 1));
        mp_obj_list_append(kinds, mp_obj_new_str("ecg", 3));
        mp_obj_dict_store(
            by_kind,
            mp_obj_new_str("ecg", 3),
            polar_stream_kind_capabilities_dict(true, false, true, "decoded", "uV", mp_obj_new_int(4), ecg_cfg));

        mp_obj_t acc_cfg = mp_obj_new_dict(2);
        mp_obj_dict_store(acc_cfg, mp_obj_new_str("sample_rate_hz", 14), polar_config_desc_range(1, 65535, 1));
        mp_obj_dict_store(acc_cfg, mp_obj_new_str("range", 5), polar_config_desc_range(1, 65535, 1));
        mp_obj_list_append(kinds, mp_obj_new_str("acc", 3));
        mp_obj_dict_store(
            by_kind,
            mp_obj_new_str("acc", 3),
            polar_stream_kind_capabilities_dict(true, false, true, "decoded", "mg", mp_obj_new_int(6), acc_cfg));
    }

    mp_obj_t streams = mp_obj_new_dict(3);
    mp_obj_dict_store(streams, mp_obj_new_str("kinds", 5), kinds);
    mp_obj_dict_store(streams, mp_obj_new_str("max_parallel_pmd_streams", 24), mp_obj_new_int(POLAR_CFG_ENABLE_ECG ? 2 : 0));
    mp_obj_dict_store(streams, mp_obj_new_str("by_kind", 7), by_kind);
    mp_obj_dict_store(root, mp_obj_new_str("streams", 7), streams);

    bool recording_supported = polar_h10_recording_is_supported(self);
    mp_obj_t recording = mp_obj_new_dict(5);
    mp_obj_dict_store(recording, mp_obj_new_str("supported", 9), mp_obj_new_bool(recording_supported));
    mp_obj_t recording_kinds = mp_obj_new_list(0, NULL);
    mp_obj_t recording_by_kind = mp_obj_new_dict(1);
    if (recording_supported) {
        static const char *const h10_recording_sample_type_choices[] = { "hr", "rr" };
        mp_obj_t hr_desc = mp_obj_new_dict(2);
        mp_obj_t hr_cfg = mp_obj_new_dict(2);
        mp_obj_dict_store(hr_cfg, mp_obj_new_str("sample_type", 11), polar_config_desc_enum(h10_recording_sample_type_choices, 2));
        mp_obj_dict_store(hr_cfg, mp_obj_new_str("interval_s", 10), polar_config_desc_range(1, 5, 4));
        mp_obj_dict_store(hr_desc, mp_obj_new_str("units", 5), mp_const_none);
        mp_obj_dict_store(hr_desc, mp_obj_new_str("config", 6), hr_cfg);
        mp_obj_list_append(recording_kinds, mp_obj_new_str("hr", 2));
        mp_obj_dict_store(recording_by_kind, mp_obj_new_str("hr", 2), hr_desc);
    }
    mp_obj_dict_store(recording, mp_obj_new_str("kinds", 5), recording_kinds);
    mp_obj_dict_store(recording, mp_obj_new_str("max_parallel_kinds", 18), mp_obj_new_int(recording_supported ? 1 : 0));
    mp_obj_dict_store(recording, mp_obj_new_str("features", 8), polar_capability_bool_dict4(recording_supported, recording_supported, false, false));
    mp_obj_dict_store(recording, mp_obj_new_str("by_kind", 7), recording_by_kind);
    mp_obj_dict_store(root, mp_obj_new_str("recording", 9), recording);

    mp_obj_t psftp = mp_obj_new_dict(4);
    mp_obj_dict_store(psftp, mp_obj_new_str("read", 4), mp_obj_new_bool(POLAR_CFG_ENABLE_PSFTP));
    mp_obj_dict_store(psftp, mp_obj_new_str("delete", 6), mp_obj_new_bool(POLAR_CFG_ENABLE_PSFTP));
    mp_obj_dict_store(psftp, mp_obj_new_str("chunked_download", 16), mp_obj_new_bool(POLAR_CFG_ENABLE_PSFTP));
    mp_obj_dict_store(psftp, mp_obj_new_str("max_chunk_bytes", 15), mp_obj_new_int_from_uint(POLAR_SDK_PSFTP_MAX_DOWNLOAD_BYTES));
    mp_obj_dict_store(root, mp_obj_new_str("psftp", 5), psftp);

    mp_obj_t security = mp_obj_new_dict(3);
    bool bonded = false;
    uint8_t key_size = 0;
#if POLAR_SDK_HAS_BTSTACK
    if (self->runtime_link.connected && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID) {
        bonded = gap_bonded(self->runtime_link.conn_handle);
        key_size = gap_encryption_key_size(self->runtime_link.conn_handle);
    }
#endif
    mp_obj_dict_store(security, mp_obj_new_str("bonded", 6), mp_obj_new_bool(bonded));
    mp_obj_dict_store(security, mp_obj_new_str("encryption_key_size", 19), mp_obj_new_int_from_uint(key_size));
    mp_obj_dict_store(security, mp_obj_new_str("pairing_method", 14), mp_const_none);
    mp_obj_dict_store(root, mp_obj_new_str("security", 8), security);

    return root;
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_capabilities_obj, polar_h10_capabilities);

#if POLAR_SDK_HAS_BTSTACK
static uint32_t polar_btstack_cfg_feature_mask(void) {
    uint32_t mask = 0;
    #if defined(ENABLE_LE_CENTRAL)
    mask |= (1u << 0);
    #endif
    #if defined(ENABLE_LE_PERIPHERAL)
    mask |= (1u << 1);
    #endif
    #if defined(ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL)
    mask |= (1u << 2);
    #endif
    #if defined(ENABLE_LOG_INFO)
    mask |= (1u << 3);
    #endif
    #if defined(ENABLE_LOG_ERROR)
    mask |= (1u << 4);
    #endif
    #if defined(ENABLE_LOG_DEBUG)
    mask |= (1u << 5);
    #endif
    return mask;
}

static mp_obj_t polar_btstack_config_snapshot_dict(void) {
    mp_obj_t dict = mp_obj_new_dict(24);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_feature_mask), mp_obj_new_int_from_uint(polar_btstack_cfg_feature_mask()));

    #if defined(ENABLE_LE_CENTRAL)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_le_central), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_le_central), mp_const_false);
    #endif

    #if defined(ENABLE_LE_PERIPHERAL)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_le_peripheral), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_le_peripheral), mp_const_false);
    #endif

    #if defined(ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_hci_controller_to_host_flow_control), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_hci_controller_to_host_flow_control), mp_const_false);
    #endif

    #if defined(ENABLE_LOG_INFO)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_info), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_info), mp_const_false);
    #endif

    #if defined(ENABLE_LOG_ERROR)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_error), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_error), mp_const_false);
    #endif

    #if defined(ENABLE_LOG_DEBUG)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_debug), mp_const_true);
    #else
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_enable_log_debug), mp_const_false);
    #endif

    #if defined(HCI_ACL_PAYLOAD_SIZE)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_hci_acl_payload_size), mp_obj_new_int_from_uint((uint32_t)HCI_ACL_PAYLOAD_SIZE));
    #endif
    #if defined(MAX_NR_HCI_CONNECTIONS)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_hci_connections), mp_obj_new_int_from_uint((uint32_t)MAX_NR_HCI_CONNECTIONS));
    #endif
    #if defined(MAX_NR_GATT_CLIENTS)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_gatt_clients), mp_obj_new_int_from_uint((uint32_t)MAX_NR_GATT_CLIENTS));
    #endif
    #if defined(MAX_NR_L2CAP_CHANNELS)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_l2cap_channels), mp_obj_new_int_from_uint((uint32_t)MAX_NR_L2CAP_CHANNELS));
    #endif
    #if defined(MAX_NR_L2CAP_SERVICES)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_l2cap_services), mp_obj_new_int_from_uint((uint32_t)MAX_NR_L2CAP_SERVICES));
    #endif
    #if defined(MAX_NR_SM_LOOKUP_ENTRIES)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_sm_lookup_entries), mp_obj_new_int_from_uint((uint32_t)MAX_NR_SM_LOOKUP_ENTRIES));
    #endif
    #if defined(MAX_NR_WHITELIST_ENTRIES)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_whitelist_entries), mp_obj_new_int_from_uint((uint32_t)MAX_NR_WHITELIST_ENTRIES));
    #endif
    #if defined(MAX_NR_LE_DEVICE_DB_ENTRIES)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_nr_le_device_db_entries), mp_obj_new_int_from_uint((uint32_t)MAX_NR_LE_DEVICE_DB_ENTRIES));
    #endif
    #if defined(MAX_ATT_DB_SIZE)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_max_att_db_size), mp_obj_new_int_from_uint((uint32_t)MAX_ATT_DB_SIZE));
    #endif
    #if defined(HCI_RESET_RESEND_TIMEOUT_MS)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_cfg_hci_reset_resend_timeout_ms), mp_obj_new_int_from_uint((uint32_t)HCI_RESET_RESEND_TIMEOUT_MS));
    #endif

    return dict;
}

#endif

static mp_obj_t polar_h10_stats(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_obj_t dict = mp_obj_new_dict(96);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_state), mp_obj_new_str(polar_state_strs[self->runtime_link.state], strlen(polar_state_strs[self->runtime_link.state])));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connected), mp_obj_new_bool(self->runtime_link.connected));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connect_calls), mp_obj_new_int_from_uint(self->connect_calls));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_disconnect_calls), mp_obj_new_int_from_uint(self->disconnect_calls));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_hr), mp_obj_new_bool(POLAR_CFG_ENABLE_HR));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_ecg), mp_obj_new_bool(POLAR_CFG_ENABLE_ECG));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_psftp), mp_obj_new_bool(POLAR_CFG_ENABLE_PSFTP));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_required_capabilities), polar_capabilities_tuple_from_mask(self->required_capabilities_mask));

#if POLAR_SDK_HAS_BTSTACK
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_config), polar_btstack_config_snapshot_dict());
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_polar_conn_handle), mp_obj_new_int_from_uint(self->runtime_link.conn_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connect_attempts_total), mp_obj_new_int_from_uint(self->connect_attempts_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connect_success_total), mp_obj_new_int_from_uint(self->connect_success_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_reconnect_backoff_events), mp_obj_new_int_from_uint(self->reconnect_backoff_events));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hci_events_total), mp_obj_new_int_from_uint(self->hci_events_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_adv_reports_total), mp_obj_new_int_from_uint(self->adv_reports_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_adv_match_total), mp_obj_new_int_from_uint(self->adv_match_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_conn_complete_total), mp_obj_new_int_from_uint(self->runtime_link.conn_complete_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_conn_update_complete_total), mp_obj_new_int_from_uint(self->runtime_link.conn_update_complete_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_att_mtu), mp_obj_new_int_from_uint(self->att_mtu));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mtu_exchange_pending), mp_obj_new_bool(self->mtu_exchange_pending));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mtu_exchange_total), mp_obj_new_int_from_uint(self->mtu_exchange_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_conn_interval_units), mp_obj_new_int_from_uint(self->runtime_link.last_conn_interval_units));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_conn_latency), mp_obj_new_int_from_uint(self->runtime_link.last_conn_latency));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_conn_supervision_timeout_10ms), mp_obj_new_int_from_uint(self->runtime_link.last_conn_supervision_timeout_10ms));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_conn_update_status), mp_obj_new_int_from_uint(self->runtime_link.last_conn_update_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_conn_update_pending), mp_obj_new_bool(self->runtime_link.conn_update_pending));

    uint8_t conn_encryption_key_size = 0;
    bool conn_bonded = false;
    if (self->runtime_link.connected && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID) {
        conn_encryption_key_size = gap_encryption_key_size(self->runtime_link.conn_handle);
        conn_bonded = gap_bonded(self->runtime_link.conn_handle);
    }
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_conn_encryption_key_size), mp_obj_new_int_from_uint(conn_encryption_key_size));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_conn_bonded), mp_obj_new_bool(conn_bonded));

    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_disconnect_events_total), mp_obj_new_int_from_uint(self->runtime_link.disconnect_events_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_disconnect_status), mp_obj_new_int_from_uint(self->runtime_link.last_disconnect_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_disconnect_reason), mp_obj_new_int_from_uint(self->runtime_link.last_disconnect_reason));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_disconnect_reason_0x08_total), mp_obj_new_int_from_uint(self->runtime_link.disconnect_reason_0x08_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_disconnect_reason_0x3e_total), mp_obj_new_int_from_uint(self->runtime_link.disconnect_reason_0x3e_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_disconnect_reason_other_total), mp_obj_new_int_from_uint(self->runtime_link.disconnect_reason_other_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_sm_pairing_complete_total), mp_obj_new_int_from_uint(self->sm_pairing_complete_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_sm_last_pairing_status), mp_obj_new_int_from_uint(self->sm_last_pairing_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_sm_last_pairing_reason), mp_obj_new_int_from_uint(self->sm_last_pairing_reason));

    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_hci_status), mp_obj_new_int_from_uint(self->runtime_link.last_hci_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_att_status), mp_obj_new_int_from_uint(self->last_att_status));

    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_measurement_handle), mp_obj_new_int_from_uint(self->hr_measurement_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_enabled), mp_obj_new_bool(self->hr_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_notifications_total), mp_obj_new_int_from_uint(self->hr_notifications_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_indications_total), mp_obj_new_int_from_uint(self->hr_indications_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_value_events_total), mp_obj_new_int_from_uint(self->hr_value_events_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_unmatched_value_events_total), mp_obj_new_int_from_uint(self->hr_unmatched_value_events_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_parse_errors_total), mp_obj_new_int_from_uint(self->hr_state.parse_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_resubscribe_total), mp_obj_new_int_from_uint(self->hr_resubscribe_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_last_resubscribe_ms), mp_obj_new_int_from_uint(self->hr_last_resubscribe_ms));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_last_bpm), mp_obj_new_int_from_uint(self->hr_state.last_bpm));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_last_rr_count), mp_obj_new_int_from_uint(self->hr_state.last_rr_count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hr_last_flags), mp_obj_new_int_from_uint(self->hr_state.last_flags));

    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cp_handle), mp_obj_new_int_from_uint(self->pmd_cp_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_data_handle), mp_obj_new_int_from_uint(self->pmd_data_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cp_properties), mp_obj_new_int_from_uint(self->pmd_cp_char.properties));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_data_properties), mp_obj_new_int_from_uint(self->pmd_data_char.properties));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cfg_att_status), mp_obj_new_int_from_uint(self->pmd_cfg_att_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_write_att_status), mp_obj_new_int_from_uint(self->pmd_write_att_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cp_response_status), mp_obj_new_int_from_uint(self->pmd_cp_response_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_enabled), mp_obj_new_bool(self->ecg_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_available_bytes), mp_obj_new_int_from_uint(self->ecg_ring.count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_notifications_total), mp_obj_new_int_from_uint(self->ecg_ring.notifications_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_frames_total), mp_obj_new_int_from_uint(self->ecg_ring.frames_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_samples_total), mp_obj_new_int_from_uint(self->ecg_ring.samples_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_parse_errors_total), mp_obj_new_int_from_uint(self->ecg_ring.parse_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_drop_bytes_total), mp_obj_new_int_from_uint(self->ecg_ring.drop_bytes_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecg_ring_high_water), mp_obj_new_int_from_uint(self->ecg_ring.ring_high_water));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_enabled), mp_obj_new_bool(self->imu_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_available_bytes), mp_obj_new_int_from_uint(self->imu_ring.count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_notifications_total), mp_obj_new_int_from_uint(self->imu_ring.notifications_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_frames_total), mp_obj_new_int_from_uint(self->imu_ring.frames_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_samples_total), mp_obj_new_int_from_uint(self->imu_ring.samples_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_parse_errors_total), mp_obj_new_int_from_uint(self->imu_ring.parse_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_drop_bytes_total), mp_obj_new_int_from_uint(self->imu_ring.drop_bytes_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_acc_ring_high_water), mp_obj_new_int_from_uint(self->imu_ring.ring_high_water));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cp_notifications_total), mp_obj_new_int_from_uint(self->pmd_cp_notifications_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pmd_cp_response_total), mp_obj_new_int_from_uint(self->pmd_cp_response_total));

    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_mtu_handle), mp_obj_new_int_from_uint(self->psftp_mtu_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_d2h_handle), mp_obj_new_int_from_uint(self->psftp_d2h_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_h2d_handle), mp_obj_new_int_from_uint(self->psftp_h2d_handle));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_mtu_properties), mp_obj_new_int_from_uint(self->psftp_mtu_char.properties));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_d2h_properties), mp_obj_new_int_from_uint(self->psftp_d2h_char.properties));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_h2d_properties), mp_obj_new_int_from_uint(self->psftp_h2d_char.properties));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_mtu_enabled), mp_obj_new_bool(self->psftp_mtu_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_d2h_enabled), mp_obj_new_bool(self->psftp_d2h_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_tx_frames_total), mp_obj_new_int_from_uint(self->psftp_tx_frames_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_rx_frames_total), mp_obj_new_int_from_uint(self->psftp_rx_frames_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_rx_seq_errors_total), mp_obj_new_int_from_uint(self->psftp_rx_seq_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_protocol_errors_total), mp_obj_new_int_from_uint(self->psftp_protocol_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_overflow_errors_total), mp_obj_new_int_from_uint(self->psftp_overflow_errors_total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_last_error_code), mp_obj_new_int_from_uint(self->psftp_last_error_code));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_last_att_status), mp_obj_new_int_from_uint(self->psftp_last_att_status));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_psftp_last_response_bytes), mp_obj_new_int_from_uint(self->psftp_last_response_bytes));
#endif

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_stats_obj, polar_h10_stats);

static mp_obj_t polar_h10_connect(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_timeout_ms,
        ARG_required_capabilities,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout_ms, MP_ARG_INT, { .u_int = 10000 } },
        { MP_QSTR_required_capabilities, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    self->connect_calls += 1;

#if !POLAR_SDK_HAS_BTSTACK
    (void)parsed_args;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack central mode is not available in this build"));
#else
    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be > 0"));
    }

    polar_capability_mask_t connect_caps_mask = self->required_capabilities_mask;
    uint8_t connect_services_mask = self->required_services_mask;
    if (parsed_args[ARG_required_capabilities].u_obj != mp_const_none) {
        (void)polar_required_capabilities_parse(parsed_args[ARG_required_capabilities].u_obj, &connect_caps_mask, &connect_services_mask);
    }
    if (connect_services_mask == 0) {
        connect_services_mask = polar_build_default_services_mask();
    }

    if (self->runtime_link.connected && self->runtime_link.state == POLAR_SDK_STATE_READY) {
        return mp_const_none;
    }

    if (polar_active_h10 != NULL && polar_active_h10 != self) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("another polar.Device transport is active"));
    }

    // Validate addr format up front if provided.
    if (self->addr != mp_const_none) {
        bd_addr_t addr_tmp;
        const char *addr_str = mp_obj_str_get_str(self->addr);
        if (sscanf_bd_addr(addr_str, addr_tmp) == 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid addr format"));
        }
    }

    polar_ensure_ble_ready();
    polar_register_hci_handler_refresh();

    polar_capability_mask_t saved_caps_mask = self->required_capabilities_mask;
    uint8_t saved_services_mask = self->required_services_mask;
    self->required_capabilities_mask = connect_caps_mask;
    self->required_services_mask = connect_services_mask;

    polar_active_h10 = self;

    polar_sdk_transport_connect_result_t connect_result = polar_transport_connect_with_timeout(
        self,
        (uint32_t)timeout_ms);

    if (connect_result == POLAR_SDK_TRANSPORT_CONNECT_OK) {
        self->required_capabilities_mask = saved_caps_mask;
        self->required_services_mask = saved_services_mask;
        return mp_const_none;
    }

    self->required_capabilities_mask = saved_caps_mask;
    self->required_services_mask = saved_services_mask;
    self->runtime_link.conn_update_pending = false;
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;
    if (polar_active_h10 == self) {
        polar_active_h10 = NULL;
    }

    if (connect_result == POLAR_SDK_TRANSPORT_CONNECT_TIMEOUT) {
        polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("connect timeout"));
    }
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("connect failed"));
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_connect_obj, 1, polar_h10_connect);

static mp_obj_t polar_h10_disconnect(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 10000 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    self->disconnect_calls += 1;

#if !POLAR_SDK_HAS_BTSTACK
    (void)parsed_args;
    self->runtime_link.connected = false;
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;
    return mp_const_none;
#else
    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be > 0"));
    }

    self->user_disconnect_requested = true;
    self->connect_intent = false;

    if (self->runtime_link.state == POLAR_SDK_STATE_SCANNING) {
        gap_stop_scan();
    }
    if (self->runtime_link.state == POLAR_SDK_STATE_CONNECTING) {
        (void)gap_connect_cancel();
    }

    if (self->runtime_link.connected && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID) {
        uint8_t err = gap_disconnect(self->runtime_link.conn_handle);
        if (err != ERROR_CODE_SUCCESS) {
            self->runtime_link.last_hci_status = err;
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("gap_disconnect failed"));
        }

        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (!self->runtime_link.connected || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                break;
            }
            mp_event_wait_ms(10);
        }
    }

    if (self->hr_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->hr_notification);
        self->hr_notification_listening = false;
    }
    if (self->pmd_cp_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->pmd_cp_notification);
        self->pmd_cp_notification_listening = false;
    }
    if (self->pmd_data_notification_listening) {
        gatt_client_stop_listening_for_characteristic_value_updates(&self->pmd_data_notification);
        self->pmd_data_notification_listening = false;
    }

    polar_download_blob_reset(self);

    self->hr_enabled = false;
    self->hr_raw_last_len = 0;
    self->hr_raw_consumed_seq = self->hr_raw_seq;
    self->ecg_enabled = false;
    self->imu_enabled = false;
    self->runtime_link.connected = false;
    self->runtime_link.conn_handle = HCI_CON_HANDLE_INVALID;
    self->runtime_link.conn_update_pending = false;
    self->runtime_link.state = POLAR_SDK_STATE_IDLE;

    if (polar_active_h10 == self) {
        polar_active_h10 = NULL;
    }

    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_disconnect_obj, 1, polar_h10_disconnect);

static mp_obj_t polar_h10_start_hr(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_HR) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("HR feature disabled at build time"));
    }
    polar_require_no_active_download_handle(self);
    if (self->hr_enabled) {
        if (self->hr_stream_format == POLAR_STREAM_FORMAT_DECODED) {
            return mp_const_none;
        }
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("HR stream already active with different format"));
    }
    self->hr_stream_format = POLAR_STREAM_FORMAT_DECODED;
    polar_hr_set_notify(self, true);
    self->hr_consumed_seq = self->hr_state.sample_seq;
    self->hr_raw_consumed_seq = self->hr_raw_seq;
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_start_hr_obj, polar_h10_start_hr);

static mp_obj_t polar_h10_stop_hr(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_const_none;
#else
    if (!POLAR_CFG_ENABLE_HR) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("HR feature disabled at build time"));
    }
    polar_require_no_active_download_handle(self);
    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        self->hr_enabled = false;
        self->hr_cfg_pending = false;
        self->hr_cfg_done = false;
        self->hr_raw_last_len = 0;
        self->hr_consumed_seq = self->hr_state.sample_seq;
        self->hr_raw_consumed_seq = self->hr_raw_seq;
        return mp_const_none;
    }

    // Best-effort disable: if CCC write fails/times out, still clear local state
    // so callers can continue cleanup paths.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        polar_hr_set_notify(self, false);
        nlr_pop();
    } else {
        self->hr_enabled = false;
        self->hr_cfg_pending = false;
        self->hr_cfg_done = false;
    }
    self->hr_raw_last_len = 0;
    self->hr_consumed_seq = self->hr_state.sample_seq;
    self->hr_raw_consumed_seq = self->hr_raw_seq;
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_stop_hr_obj, polar_h10_stop_hr);

static mp_obj_t polar_h10_read_hr(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_const_none;
#else
    if (!POLAR_CFG_ENABLE_HR) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("HR feature disabled at build time"));
    }
    polar_require_ready(self);
    if (!self->hr_enabled || self->hr_stream_format != POLAR_STREAM_FORMAT_DECODED) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("decoded HR stream is not active"));
    }

    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be >= 0"));
    }

    if (self->hr_consumed_seq >= self->hr_state.sample_seq && timeout_ms > 0) {
        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            }
            if (self->hr_consumed_seq < self->hr_state.sample_seq) {
                break;
            }
            mp_event_wait_ms(10);
        }
    }

    if (self->hr_consumed_seq >= self->hr_state.sample_seq && timeout_ms > 0) {
        // Recovery path: if we waited and still saw no sample while connected,
        // try re-arming HR CCC once.
        polar_hr_rearm_best_effort(self);

        if (self->runtime_link.connected && self->runtime_link.state == POLAR_SDK_STATE_READY && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID && self->hr_enabled) {
            uint32_t start_ms = polar_now_ms();
            uint32_t rearm_wait_ms = (uint32_t)timeout_ms;
            if (rearm_wait_ms > 1200) {
                rearm_wait_ms = 1200;
            }
            while (!polar_elapsed_ms(start_ms, rearm_wait_ms)) {
                if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                    polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
                }
                if (self->hr_consumed_seq < self->hr_state.sample_seq) {
                    break;
                }
                mp_event_wait_ms(10);
            }
        }
    }

    if (self->hr_consumed_seq >= self->hr_state.sample_seq) {
        return mp_const_none;
    }

    self->hr_consumed_seq = self->hr_state.sample_seq;

    mp_obj_t out[9] = {
        mp_obj_new_str("unknown", 7),
        mp_const_none,
        mp_obj_new_int_from_uint(self->hr_state.last_bpm),
        mp_obj_new_int_from_uint(polar_hr_public_flags(&self->hr_state)),
        mp_obj_new_int_from_uint(self->hr_state.last_rr_count),
        mp_obj_new_int_from_uint(self->hr_state.last_rr_ms[0]),
        mp_obj_new_int_from_uint(self->hr_state.last_rr_ms[1]),
        mp_obj_new_int_from_uint(self->hr_state.last_rr_ms[2]),
        mp_obj_new_int_from_uint(self->hr_state.last_rr_ms[3]),
    };
    return mp_obj_new_tuple(9, out);
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_read_hr_obj, 1, polar_h10_read_hr);

static bool polar_pmd_policy_is_connected(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return self->runtime_link.connected && self->runtime_link.state == POLAR_SDK_STATE_READY && self->runtime_link.conn_handle != HCI_CON_HANDLE_INVALID;
}

static uint8_t polar_pmd_policy_encryption_key_size(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (!polar_pmd_policy_is_connected(ctx)) {
        return 0;
    }
    return gap_encryption_key_size(self->runtime_link.conn_handle);
}

static void polar_pmd_policy_request_pairing(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (!polar_pmd_policy_is_connected(ctx)) {
        return;
    }
    polar_sdk_btstack_sm_apply_default_auth_policy();
    sm_request_pairing(self->runtime_link.conn_handle);
}

static void polar_pmd_policy_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    mp_event_wait_ms((mp_uint_t)ms);
}

static int polar_pmd_policy_enable_cp_notify(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_pmd_set_notify_for_char_result(
        self,
        &self->pmd_cp_char,
        &self->pmd_cp_notification,
        &self->pmd_cp_notification_listening,
        true);
}

static int polar_pmd_policy_enable_data_notify(void *ctx) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_pmd_set_notify_for_char_result(
        self,
        &self->pmd_data_char,
        &self->pmd_data_notification,
        &self->pmd_data_notification_listening,
        true);
}

static int polar_pmd_policy_enable_notifications(void *ctx) {
    int last_att_status = 0;
    polar_sdk_pmd_notify_pair_ops_t ops = {
        .ctx = ctx,
        .is_connected = polar_pmd_policy_is_connected,
        .enable_cp_notify = polar_pmd_policy_enable_cp_notify,
        .enable_data_notify = polar_pmd_policy_enable_data_notify,
    };
    int r = polar_sdk_pmd_enable_notify_pair(
        &ops,
        POLAR_SDK_PMD_OP_OK,
        POLAR_SDK_PMD_OP_NOT_CONNECTED,
        POLAR_SDK_PMD_OP_TRANSPORT,
        &last_att_status);
    if (r == POLAR_SDK_PMD_OP_TRANSPORT && last_att_status > 0) {
        return last_att_status;
    }
    return r;
}

static int polar_pmd_policy_ensure_minimum_mtu(void *ctx, uint16_t minimum_mtu) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    if (!polar_pmd_policy_is_connected(ctx)) {
        return POLAR_SDK_PMD_OP_NOT_CONNECTED;
    }
    if (polar_pmd_ensure_mtu(self, minimum_mtu, POLAR_SDK_GATT_OP_TIMEOUT_MS)) {
        return POLAR_SDK_PMD_OP_OK;
    }
    if (!polar_pmd_policy_is_connected(ctx)) {
        return POLAR_SDK_PMD_OP_NOT_CONNECTED;
    }
    return POLAR_SDK_PMD_OP_TIMEOUT;
}

static void polar_pmd_policy_expect_response_cb(void *ctx, uint8_t opcode, uint8_t measurement_type) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    polar_pmd_expect_response(self, opcode, measurement_type);
}

static int polar_pmd_policy_write_command_cb(void *ctx, const uint8_t *cmd, uint16_t cmd_len) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    return polar_pmd_write_command_result(self, cmd, cmd_len);
}

static bool polar_pmd_policy_wait_response_cb(void *ctx, uint32_t timeout_ms, uint8_t *out_response_status) {
    polar_h10_obj_t *self = (polar_h10_obj_t *)ctx;
    bool done = polar_pmd_wait_response(self, timeout_ms);
    if (done && out_response_status != NULL) {
        *out_response_status = self->pmd_cp_response_status;
    }
    return done;
}

static int polar_pmd_policy_start_measurement_and_wait_response(
    void *ctx,
    const uint8_t *start_cmd,
    size_t start_cmd_len,
    uint8_t *out_status) {
    if (start_cmd == NULL || start_cmd_len < 2) {
        return POLAR_SDK_PMD_OP_TRANSPORT;
    }

    uint8_t measurement_type = start_cmd[1];
    polar_sdk_pmd_start_cmd_ops_t ops = {
        .ctx = ctx,
        .is_connected = polar_pmd_policy_is_connected,
        .expect_response = polar_pmd_policy_expect_response_cb,
        .write_command = polar_pmd_policy_write_command_cb,
        .wait_response = polar_pmd_policy_wait_response_cb,
    };
    return polar_sdk_pmd_start_command_and_wait(
        &ops,
        start_cmd,
        start_cmd_len,
        POLAR_SDK_PMD_OP_REQUEST_MEASUREMENT_START,
        measurement_type,
        POLAR_SDK_GATT_OP_TIMEOUT_MS,
        POLAR_SDK_PMD_OP_OK,
        POLAR_SDK_PMD_OP_NOT_CONNECTED,
        POLAR_SDK_PMD_OP_TIMEOUT,
        POLAR_SDK_PMD_OP_TRANSPORT,
        out_status);
}

static mp_obj_t polar_h10_start_ecg(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_sample_rate,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 130 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ECG feature disabled at build time"));
    }
    polar_require_no_active_download_handle(self);
    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    }
    if (!self->pmd_cp_char_found || !self->pmd_data_char_found || self->pmd_cp_handle == 0 || self->pmd_data_handle == 0) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD characteristics missing"));
    }

    mp_int_t sample_rate = parsed_args[ARG_sample_rate].u_int;
    if (sample_rate <= 0 || sample_rate > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ECG sample_rate"));
    }
    if (self->ecg_enabled) {
        if (self->ecg_stream_format == POLAR_STREAM_FORMAT_DECODED && self->ecg_sample_rate_hz == (uint16_t)sample_rate) {
            return mp_const_none;
        }
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("ECG stream already active with different config"));
    }

    polar_sdk_pmd_start_policy_t policy = {
        .ccc_attempts = POLAR_SDK_PMD_CCC_ATTEMPTS,
        .security_rounds_per_attempt = POLAR_SDK_PMD_SECURITY_ROUNDS,
        .security_wait_ms = POLAR_SDK_PMD_SECURITY_WAIT_MS,
        .minimum_mtu = POLAR_SDK_PMD_MIN_MTU,
        .sample_rate = (uint16_t)sample_rate,
        .include_resolution = true,
        .resolution = 14,
        .include_range = false,
        .range = 0,
    };
    polar_sdk_pmd_start_ops_t ops = {
        .ctx = self,
        .is_connected = polar_pmd_policy_is_connected,
        .encryption_key_size = polar_pmd_policy_encryption_key_size,
        .request_pairing = polar_pmd_policy_request_pairing,
        .sleep_ms = polar_pmd_policy_sleep_ms,
        .enable_notifications = polar_pmd_policy_enable_notifications,
        .ensure_minimum_mtu = polar_pmd_policy_ensure_minimum_mtu,
        .start_ecg_and_wait_response = polar_pmd_policy_start_measurement_and_wait_response,
    };

    uint8_t start_response_status = 0xff;
    int last_ccc_att_status = 0;
    polar_sdk_pmd_start_result_t start_result = polar_sdk_pmd_start_ecg_with_policy(
        &policy,
        &ops,
        &start_response_status,
        &last_ccc_att_status);

    if (last_ccc_att_status > 0) {
        self->pmd_cfg_att_status = (uint8_t)last_ccc_att_status;
        self->last_att_status = (uint8_t)last_ccc_att_status;
    }
    self->pmd_cp_response_status = start_response_status;

    switch (start_result) {
        case POLAR_SDK_PMD_START_RESULT_OK:
            break;
        case POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED:
            polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT:
            polar_raise_exc(&polar_type_SecurityError, MP_ERROR_TEXT("failed to establish secure link for PMD"));
            break;
        case POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT:
            polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD CCC timeout"));
            break;
        case POLAR_SDK_PMD_START_RESULT_CCC_REJECTED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD CCC rejected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_MTU_FAILED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD requires larger ATT MTU"));
            break;
        case POLAR_SDK_PMD_START_RESULT_START_TIMEOUT:
            polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD start response timeout"));
            break;
        case POLAR_SDK_PMD_START_RESULT_START_REJECTED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD start ECG rejected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR:
        default:
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("PMD start transport failure"));
            break;
    }

    self->ecg_enabled = true;
    self->ecg_sample_rate_hz = (uint16_t)sample_rate;
    polar_ecg_ring_reset(self);
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_start_ecg_obj, 1, polar_h10_start_ecg);

static void polar_pmd_disable_notifications_best_effort(polar_h10_obj_t *self) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        polar_pmd_set_notify_for_char(
            self,
            &self->pmd_data_char,
            &self->pmd_data_notification,
            &self->pmd_data_notification_listening,
            false
            );
        nlr_pop();
    } else {
        self->pmd_data_notification_listening = false;
        self->pmd_cfg_pending = false;
        self->pmd_cfg_done = false;
    }

    if (nlr_push(&nlr) == 0) {
        polar_pmd_set_notify_for_char(
            self,
            &self->pmd_cp_char,
            &self->pmd_cp_notification,
            &self->pmd_cp_notification_listening,
            false
            );
        nlr_pop();
    } else {
        self->pmd_cp_notification_listening = false;
        self->pmd_cfg_pending = false;
        self->pmd_cfg_done = false;
    }
}

static mp_obj_t polar_h10_stop_ecg(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_const_none;
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ECG feature disabled at build time"));
    }

    polar_require_no_active_download_handle(self);

    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        self->ecg_enabled = false;
        return mp_const_none;
    }

    if (self->ecg_enabled) {
        uint8_t stop_cmd[2] = {
            POLAR_SDK_PMD_OP_STOP_MEASUREMENT,
            POLAR_SDK_PMD_MEAS_ECG,
        };

        polar_pmd_expect_response(self, POLAR_SDK_PMD_OP_STOP_MEASUREMENT, POLAR_SDK_PMD_MEAS_ECG);
        polar_pmd_write_command(self, stop_cmd, (uint16_t)sizeof(stop_cmd));

        if (polar_pmd_wait_response(self, POLAR_SDK_GATT_OP_TIMEOUT_MS)) {
            if (!polar_sdk_pmd_response_status_ok(self->pmd_cp_response_status)) {
                polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD stop ECG rejected"));
            }
        }
    }

    self->ecg_enabled = false;

    if (!self->imu_enabled) {
        polar_pmd_disable_notifications_best_effort(self);
    }

    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_stop_ecg_obj, polar_h10_stop_ecg);

static mp_obj_t polar_h10_read_ecg(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_max_bytes,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1024 } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_obj_new_bytes((const uint8_t *)"", 0);
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ECG feature disabled at build time"));
    }
    polar_require_ready(self);
    if (!self->ecg_enabled) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("ECG stream is not active"));
    }

    mp_int_t max_bytes = parsed_args[ARG_max_bytes].u_int;
    if (max_bytes <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be > 0"));
    }
    if (max_bytes > POLAR_SDK_ECG_RING_BYTES) {
        max_bytes = POLAR_SDK_ECG_RING_BYTES;
    }
    max_bytes &= ~0x03;
    if (max_bytes <= 0) {
        return mp_obj_new_bytes((const uint8_t *)"", 0);
    }

    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be >= 0"));
    }

    if (polar_ecg_ring_available(self) == 0 && timeout_ms > 0 && self->ecg_enabled) {
        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            }
            if (polar_ecg_ring_available(self) > 0) {
                break;
            }
            mp_event_wait_ms(10);
        }
    }

    uint16_t n = polar_ecg_ring_available(self);
    if (n == 0) {
        return mp_obj_new_bytes((const uint8_t *)"", 0);
    }
    if (n > (uint16_t)max_bytes) {
        n = (uint16_t)max_bytes;
    }
    n &= ~0x03;
    if (n == 0) {
        return mp_obj_new_bytes((const uint8_t *)"", 0);
    }

    uint8_t *tmp = m_new(uint8_t, n);
    uint16_t popped = polar_ecg_ring_pop_bytes(self, tmp, n);
    mp_obj_t out = mp_obj_new_bytes(tmp, popped);
    m_del(uint8_t, tmp, n);
    return out;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_read_ecg_obj, 1, polar_h10_read_ecg);

static mp_obj_t polar_h10_start_imu(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_sample_rate,
        ARG_range,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 50 } },
        { MP_QSTR_range, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = POLAR_SDK_IMU_DEFAULT_RANGE_G } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ACC feature disabled at build time"));
    }
    polar_require_no_active_download_handle(self);
    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
    }
    if (!self->pmd_cp_char_found || !self->pmd_data_char_found || self->pmd_cp_handle == 0 || self->pmd_data_handle == 0) {
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD characteristics missing"));
    }

    mp_int_t sample_rate = parsed_args[ARG_sample_rate].u_int;
    if (sample_rate <= 0 || sample_rate > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ACC sample_rate"));
    }

    mp_int_t range = parsed_args[ARG_range].u_int;
    if (range <= 0 || range > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ACC range"));
    }
    if (self->imu_enabled) {
        if (self->acc_stream_format == POLAR_STREAM_FORMAT_DECODED &&
            self->acc_sample_rate_hz == (uint16_t)sample_rate &&
            self->acc_range == (uint16_t)range) {
            return mp_const_none;
        }
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("ACC stream already active with different config"));
    }

    polar_sdk_pmd_start_policy_t policy = {
        .ccc_attempts = POLAR_SDK_PMD_CCC_ATTEMPTS,
        .security_rounds_per_attempt = POLAR_SDK_PMD_SECURITY_ROUNDS,
        .security_wait_ms = POLAR_SDK_PMD_SECURITY_WAIT_MS,
        .minimum_mtu = POLAR_SDK_PMD_MIN_MTU,
        .sample_rate = (uint16_t)sample_rate,
        .include_resolution = true,
        .resolution = POLAR_SDK_IMU_DEFAULT_RESOLUTION,
        .include_range = true,
        .range = (uint16_t)range,
    };
    polar_sdk_pmd_start_ops_t ops = {
        .ctx = self,
        .is_connected = polar_pmd_policy_is_connected,
        .encryption_key_size = polar_pmd_policy_encryption_key_size,
        .request_pairing = polar_pmd_policy_request_pairing,
        .sleep_ms = polar_pmd_policy_sleep_ms,
        .enable_notifications = polar_pmd_policy_enable_notifications,
        .ensure_minimum_mtu = polar_pmd_policy_ensure_minimum_mtu,
        .start_ecg_and_wait_response = polar_pmd_policy_start_measurement_and_wait_response,
    };

    uint8_t start_response_status = 0xff;
    int last_ccc_att_status = 0;
    polar_sdk_pmd_start_result_t start_result = polar_sdk_pmd_start_acc_with_policy(
        &policy,
        &ops,
        &start_response_status,
        &last_ccc_att_status);

    if (last_ccc_att_status > 0) {
        self->pmd_cfg_att_status = (uint8_t)last_ccc_att_status;
        self->last_att_status = (uint8_t)last_ccc_att_status;
    }
    self->pmd_cp_response_status = start_response_status;

    switch (start_result) {
        case POLAR_SDK_PMD_START_RESULT_OK:
            break;
        case POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED:
            polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT:
            polar_raise_exc(&polar_type_SecurityError, MP_ERROR_TEXT("failed to establish secure link for PMD"));
            break;
        case POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT:
            polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD CCC timeout"));
            break;
        case POLAR_SDK_PMD_START_RESULT_CCC_REJECTED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD CCC rejected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_MTU_FAILED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD requires larger ATT MTU"));
            break;
        case POLAR_SDK_PMD_START_RESULT_START_TIMEOUT:
            polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("PMD start response timeout"));
            break;
        case POLAR_SDK_PMD_START_RESULT_START_REJECTED:
            polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD start ACC rejected"));
            break;
        case POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR:
        default:
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("PMD start transport failure"));
            break;
    }

    self->imu_enabled = true;
    self->acc_sample_rate_hz = (uint16_t)sample_rate;
    self->acc_range = (uint16_t)range;
    polar_imu_ring_reset(self);
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_start_imu_obj, 1, polar_h10_start_imu);

static mp_obj_t polar_h10_stop_imu(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_const_none;
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ACC feature disabled at build time"));
    }

    polar_require_no_active_download_handle(self);

    if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
        self->imu_enabled = false;
        return mp_const_none;
    }

    if (self->imu_enabled) {
        uint8_t stop_cmd[2] = {
            POLAR_SDK_PMD_OP_STOP_MEASUREMENT,
            POLAR_SDK_PMD_MEAS_ACC,
        };

        polar_pmd_expect_response(self, POLAR_SDK_PMD_OP_STOP_MEASUREMENT, POLAR_SDK_PMD_MEAS_ACC);
        polar_pmd_write_command(self, stop_cmd, (uint16_t)sizeof(stop_cmd));

        if (polar_pmd_wait_response(self, POLAR_SDK_GATT_OP_TIMEOUT_MS)) {
            if (!polar_sdk_pmd_response_status_ok(self->pmd_cp_response_status)) {
                polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("PMD stop ACC rejected"));
            }
        }
    }

    self->imu_enabled = false;

    if (!self->ecg_enabled) {
        polar_pmd_disable_notifications_best_effort(self);
    }

    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_stop_imu_obj, polar_h10_stop_imu);

static mp_obj_t polar_h10_read_imu(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_max_bytes,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1024 } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    return mp_obj_new_bytes((const uint8_t *)"", 0);
#else
    if (!POLAR_CFG_ENABLE_ECG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ACC feature disabled at build time"));
    }
    polar_require_ready(self);
    if (!self->imu_enabled) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("ACC stream is not active"));
    }

    mp_int_t max_bytes = parsed_args[ARG_max_bytes].u_int;
    if (max_bytes <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be > 0"));
    }
    if (max_bytes > POLAR_SDK_IMU_RING_BYTES) {
        max_bytes = POLAR_SDK_IMU_RING_BYTES;
    }
    max_bytes -= (max_bytes % 6);
    if (max_bytes <= 0) {
        return mp_obj_new_bytes((const uint8_t *)"", 0);
    }

    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be >= 0"));
    }

    if (polar_imu_ring_available(self) == 0 && timeout_ms > 0 && self->imu_enabled) {
        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                break;
            }
            if (polar_imu_ring_available(self) > 0) {
                break;
            }
            mp_event_wait_ms(5);
        }
    }

    uint16_t n = polar_imu_ring_available(self);
    if (n > (uint16_t)max_bytes) {
        n = (uint16_t)max_bytes;
    }
    n -= (uint16_t)(n % 6u);
    if (n == 0) {
        return mp_obj_new_bytes((const uint8_t *)"", 0);
    }

    uint8_t *tmp = m_new(uint8_t, n);
    uint16_t popped = polar_imu_ring_pop_bytes(self, tmp, n);
    mp_obj_t out = mp_obj_new_bytes(tmp, popped);
    m_del(uint8_t, tmp, n);
    return out;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_read_imu_obj, 1, polar_h10_read_imu);

static mp_obj_t polar_h10_list_dir(mp_obj_t self_in, mp_obj_t path_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);

#if !POLAR_SDK_HAS_BTSTACK
    (void)path_in;
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_PSFTP) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("PSFTP feature disabled at build time"));
    }
    polar_require_ready(self);
    if (self->download_handle_active) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("download handle already active"));
    }
    if (!mp_obj_is_str(path_in)) {
        mp_raise_TypeError(MP_ERROR_TEXT("path must be str"));
    }

    size_t path_len = 0;
    const char *path = mp_obj_str_get_data(path_in, &path_len);
    if (path_len == 0 || path[0] != '/') {
        mp_raise_ValueError(MP_ERROR_TEXT("path must be absolute"));
    }

    bool add_trailing_slash = path[path_len - 1] != '/';
    size_t normalized_len = path_len + (add_trailing_slash ? 1u : 0u);
    if (normalized_len > POLAR_SDK_PSFTP_MAX_PATH_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("path too long"));
    }

    char normalized_path[POLAR_SDK_PSFTP_MAX_PATH_BYTES + 1u];
    memcpy(normalized_path, path, path_len);
    if (add_trailing_slash) {
        normalized_path[path_len] = '/';
    }
    normalized_path[normalized_len] = '\0';

    uint8_t *response = m_new(uint8_t, POLAR_SDK_PSFTP_MAX_DIR_RESPONSE_BYTES);
    size_t response_len = 0;
    polar_sdk_psftp_trans_result_t r = polar_psftp_execute_get(
        self,
        normalized_path,
        normalized_len,
        response,
        POLAR_SDK_PSFTP_MAX_DIR_RESPONSE_BYTES,
        POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS,
        &response_len);
    if (r != POLAR_SDK_PSFTP_TRANS_OK) {
        m_del(uint8_t, response, POLAR_SDK_PSFTP_MAX_DIR_RESPONSE_BYTES);
        polar_raise_for_psftp_result(self, r);
    }

    polar_sdk_psftp_dir_entry_t *entries = m_new(
        polar_sdk_psftp_dir_entry_t,
        POLAR_SDK_PSFTP_MAX_DIR_ENTRIES);
    size_t entry_count = 0;
    polar_sdk_psftp_dir_decode_result_t decode_result = polar_sdk_psftp_decode_directory(
        response,
        response_len,
        entries,
        POLAR_SDK_PSFTP_MAX_DIR_ENTRIES,
        &entry_count);

    m_del(uint8_t, response, POLAR_SDK_PSFTP_MAX_DIR_RESPONSE_BYTES);

    if (decode_result == POLAR_SDK_PSFTP_DIR_DECODE_TOO_MANY_ENTRIES) {
        m_del(polar_sdk_psftp_dir_entry_t, entries, POLAR_SDK_PSFTP_MAX_DIR_ENTRIES);
        polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("PSFTP directory entry overflow"));
    }
    if (decode_result != POLAR_SDK_PSFTP_DIR_DECODE_OK) {
        m_del(polar_sdk_psftp_dir_entry_t, entries, POLAR_SDK_PSFTP_MAX_DIR_ENTRIES);
        polar_raise_exc(&polar_type_ProtocolError, MP_ERROR_TEXT("failed to decode PSFTP directory"));
    }

    mp_obj_t out_list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < entry_count; ++i) {
        size_t name_len = strlen(entries[i].name);
        bool is_dir = false;
        if (name_len > 0 && entries[i].name[name_len - 1] == '/') {
            is_dir = true;
            name_len -= 1;
        }

        mp_obj_t entry = mp_obj_new_dict(3);
        mp_obj_dict_store(entry, mp_obj_new_str("name", 4), mp_obj_new_str(entries[i].name, name_len));
        mp_obj_dict_store(entry, mp_obj_new_str("size", 4), mp_obj_new_int_from_ull(entries[i].size));
        mp_obj_dict_store(entry, mp_obj_new_str("is_dir", 6), mp_obj_new_bool(is_dir));
        mp_obj_list_append(out_list, entry);
    }

    m_del(polar_sdk_psftp_dir_entry_t, entries, POLAR_SDK_PSFTP_MAX_DIR_ENTRIES);
    return out_list;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_list_dir_obj, polar_h10_list_dir);

static mp_obj_t polar_h10_download(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_path,
        ARG_max_bytes,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = POLAR_SDK_PSFTP_DEFAULT_DOWNLOAD_MAX_BYTES } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_PSFTP) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("PSFTP feature disabled at build time"));
    }
    polar_require_ready(self);
    if (self->download_handle_active) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("download handle already active"));
    }
    if (!mp_obj_is_str(parsed_args[ARG_path].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("path must be str"));
    }

    mp_int_t max_bytes = parsed_args[ARG_max_bytes].u_int;
    if (max_bytes <= 0 || max_bytes > POLAR_SDK_PSFTP_MAX_DOWNLOAD_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout_ms"));
    }

    size_t path_len = 0;
    const char *path = mp_obj_str_get_data(parsed_args[ARG_path].u_obj, &path_len);
    if (path_len == 0 || path[0] != '/' || path_len > POLAR_SDK_PSFTP_MAX_PATH_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid path"));
    }

    uint8_t *response = m_new(uint8_t, (size_t)max_bytes);
    size_t response_len = 0;
    polar_sdk_psftp_trans_result_t r = polar_psftp_execute_get(
        self,
        path,
        path_len,
        response,
        (size_t)max_bytes,
        (uint32_t)timeout_ms,
        &response_len);
    if (r != POLAR_SDK_PSFTP_TRANS_OK) {
        m_del(uint8_t, response, (size_t)max_bytes);
        polar_raise_for_psftp_result(self, r);
    }

    mp_obj_t out = mp_obj_new_bytes(response, response_len);
    m_del(uint8_t, response, (size_t)max_bytes);
    return out;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_download_obj, 2, polar_h10_download);

static mp_obj_t polar_h10_download_open(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_path,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);

#if !POLAR_SDK_HAS_BTSTACK
    (void)self;
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_PSFTP) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("PSFTP feature disabled at build time"));
    }
    polar_require_ready(self);
    if (self->download_handle_active) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("download handle already active"));
    }
    if (!mp_obj_is_str(parsed_args[ARG_path].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("path must be str"));
    }

    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout_ms"));
    }

    size_t path_len = 0;
    const char *path = mp_obj_str_get_data(parsed_args[ARG_path].u_obj, &path_len);
    if (path_len == 0 || path[0] != '/' || path_len > POLAR_SDK_PSFTP_MAX_PATH_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid path"));
    }

    polar_download_blob_reset(self);

    self->download_blob = m_new(uint8_t, POLAR_SDK_PSFTP_MAX_DOWNLOAD_BYTES);
    self->download_blob_alloc = POLAR_SDK_PSFTP_MAX_DOWNLOAD_BYTES;
    self->download_blob_len = 0;
    self->download_blob_off = 0;
    self->download_handle_active = true;

    polar_sdk_psftp_trans_result_t r = polar_psftp_begin_get_stream(self, path, path_len);
    if (r != POLAR_SDK_PSFTP_TRANS_OK) {
        polar_download_blob_reset(self);
        polar_raise_for_psftp_result(self, r);
    }

    if (timeout_ms > 0) {
        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (polar_download_blob_available(self) > 0 || self->psftp_response_done) {
                break;
            }
            if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                polar_download_blob_reset(self);
                polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            }
            mp_event_wait_ms(10);
        }

        if (self->psftp_response_done && polar_download_blob_available(self) == 0 && self->psftp_response_result != POLAR_SDK_PSFTP_RX_COMPLETE) {
            polar_sdk_psftp_rx_result_t rx_result = self->psftp_response_result;
            polar_download_blob_reset(self);
            polar_raise_for_psftp_rx_result(self, rx_result);
        }
    }

    if (self->download_handle_next_id == 0) {
        self->download_handle_next_id = 1;
    }
    self->download_handle_id = self->download_handle_next_id;
    self->download_handle_next_id += 1;

    return mp_obj_new_int_from_uint(self->download_handle_id);
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_download_open_obj, 2, polar_h10_download_open);

static mp_obj_t polar_h10_download_read(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_handle,
        ARG_buf,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handle, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_buf, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t handle = parsed_args[ARG_handle].u_int;
    mp_int_t timeout_ms = parsed_args[ARG_timeout_ms].u_int;
    if (timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout_ms"));
    }
    if (handle <= 0 || !self->download_handle_active || (uint32_t)handle != self->download_handle_id) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid handle"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(parsed_args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_WRITE);
    if (bufinfo.len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must not be empty"));
    }

    size_t available = polar_download_blob_available(self);
    if (available == 0 && timeout_ms > 0 && !self->psftp_response_done) {
        uint32_t start_ms = polar_now_ms();
        while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
            if (polar_download_blob_available(self) > 0 || self->psftp_response_done) {
                break;
            }
            if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
                polar_download_blob_reset(self);
                polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
            }
            mp_event_wait_ms(10);
        }
        available = polar_download_blob_available(self);
    }

    if (available == 0 && self->psftp_response_done) {
        polar_sdk_psftp_rx_result_t result = self->psftp_response_result;
        if (result == POLAR_SDK_PSFTP_RX_COMPLETE) {
            polar_download_blob_reset(self);
            return mp_obj_new_int(0);
        }

        polar_download_blob_reset(self);
        polar_raise_for_psftp_rx_result(self, result);
    }

    if (available == 0) {
        if (!self->runtime_link.connected || self->runtime_link.state != POLAR_SDK_STATE_READY || self->runtime_link.conn_handle == HCI_CON_HANDLE_INVALID) {
            polar_download_blob_reset(self);
            polar_raise_exc(&polar_type_NotConnectedError, MP_ERROR_TEXT("not connected"));
        }
        polar_raise_exc(&polar_type_TimeoutError, MP_ERROR_TEXT("download read timeout"));
    }

    size_t n = bufinfo.len;
    if (n > available) {
        n = available;
    }
    memcpy(bufinfo.buf, self->download_blob + self->download_blob_off, n);
    self->download_blob_off += n;

    if (self->download_blob_off == self->psftp_rx_state.length) {
        self->download_blob_off = 0;
        self->psftp_rx_state.length = 0;
    }

    return mp_obj_new_int_from_uint((mp_uint_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_download_read_obj, 3, polar_h10_download_read);

static mp_obj_t polar_h10_download_close(mp_obj_t self_in, mp_obj_t handle_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t handle = mp_obj_get_int(handle_in);
    if (handle <= 0 || !self->download_handle_active || (uint32_t)handle != self->download_handle_id) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid handle"));
    }

    polar_download_blob_reset(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_download_close_obj, polar_h10_download_close);

static mp_obj_t polar_h10_recording_start(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_kind,
        ARG_sample_type,
        ARG_interval_s,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kind, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_sample_type, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_interval_s, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);

    polar_recording_kind_key_t kind = polar_parse_recording_kind_key(parsed_args[ARG_kind].u_obj);
    if (kind == POLAR_RECORDING_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid recording kind"));
    }
    if (!polar_recording_kind_is_supported_build(kind)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording kind unsupported"));
    }
    if (!polar_h10_recording_is_supported(self)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording control unsupported"));
    }

    polar_h10_recording_prepare_or_raise(self);

    polar_sdk_h10_recording_sample_type_t sample_type =
        parsed_args[ARG_sample_type].u_obj == mp_const_none
            ? POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE
            : polar_parse_h10_recording_sample_type(parsed_args[ARG_sample_type].u_obj);
    mp_int_t interval_value = parsed_args[ARG_interval_s].u_int;
    if (interval_value != 1 && interval_value != 5) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid interval_s"));
    }
    if (sample_type == POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL && interval_value != 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("interval_s is unsupported for RR recording"));
    }
    uint8_t interval_s = (uint8_t)interval_value;

    polar_h10_recording_query_status_or_raise(self);
    if (self->recording_hr_active) {
        if (!self->recording_hr_config_known) {
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("recording already active with unknown config"));
        }
        if (!polar_h10_recording_local_config_matches(self, sample_type, interval_s)) {
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("recording already active with different or unknown config"));
        }
        return mp_const_none;
    }

    char recording_id[POLAR_SDK_H10_RECORDING_ID_MAX_BYTES + 1u];
    polar_h10_recording_make_auto_id(self, recording_id, sizeof(recording_id));

    uint8_t query_payload[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES];
    size_t query_payload_len = 0;
    polar_sdk_h10_recording_interval_t interval =
        interval_s == 5u ? POLAR_SDK_H10_RECORDING_INTERVAL_5S : POLAR_SDK_H10_RECORDING_INTERVAL_1S;
    if (!polar_sdk_psftp_encode_h10_start_recording_params(
            recording_id,
            strlen(recording_id),
            sample_type,
            interval,
            query_payload,
            sizeof(query_payload),
            &query_payload_len)) {
        polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("failed to encode recording start"));
    }

    uint8_t response[32];
    size_t response_len = 0;
    polar_sdk_psftp_trans_result_t result = polar_psftp_execute_query(
        self,
        POLAR_SDK_PSFTP_QUERY_REQUEST_START_RECORDING,
        query_payload,
        query_payload_len,
        response,
        sizeof(response),
        POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS,
        &response_len);
    if (result != POLAR_SDK_PSFTP_TRANS_OK) {
        polar_raise_for_psftp_result(self, result);
    }

    self->recording_hr_active = true;
    self->recording_hr_config_known = true;
    self->recording_hr_sample_type = sample_type;
    self->recording_hr_interval_s = interval_s;
    polar_h10_recording_store_id(self, recording_id, strlen(recording_id));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_recording_start_obj, 2, polar_h10_recording_start);

static mp_obj_t polar_h10_recording_stop(mp_obj_t self_in, mp_obj_t kind_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);

    polar_recording_kind_key_t kind = polar_parse_recording_kind_key(kind_in);
    if (kind == POLAR_RECORDING_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid recording kind"));
    }
    if (!polar_recording_kind_is_supported_build(kind)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording kind unsupported"));
    }
    if (!polar_h10_recording_is_supported(self)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording control unsupported"));
    }

    polar_h10_recording_prepare_or_raise(self);

    polar_h10_recording_query_status_or_raise(self);
    if (!self->recording_hr_active) {
        return mp_const_none;
    }

    uint8_t response[32];
    size_t response_len = 0;
    polar_sdk_psftp_trans_result_t result = polar_psftp_execute_query(
        self,
        POLAR_SDK_PSFTP_QUERY_REQUEST_STOP_RECORDING,
        NULL,
        0,
        response,
        sizeof(response),
        POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS,
        &response_len);
    if (result != POLAR_SDK_PSFTP_TRANS_OK) {
        polar_raise_for_psftp_result(self, result);
    }

    polar_h10_recording_clear_local_state(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_recording_stop_obj, polar_h10_recording_stop);

static mp_obj_t polar_h10_recording_status(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);

    if (!polar_h10_recording_is_supported(self)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording status unsupported"));
    }

    polar_h10_recording_prepare_or_raise(self);

    polar_h10_recording_query_status_or_raise(self);
    mp_obj_t by_kind = mp_obj_new_dict(1);
    mp_obj_dict_store(by_kind, mp_obj_new_str("hr", 2), mp_obj_new_bool(self->recording_hr_active));

    mp_obj_t active_kinds = mp_obj_new_list(0, NULL);
    if (self->recording_hr_active) {
        mp_obj_list_append(active_kinds, mp_obj_new_str("hr", 2));
    }

    mp_obj_t out = mp_obj_new_dict(3);
    mp_obj_dict_store(out, mp_obj_new_str("active", 6), mp_obj_new_bool(self->recording_hr_active));
    mp_obj_dict_store(out, mp_obj_new_str("by_kind", 7), by_kind);
    mp_obj_dict_store(out, mp_obj_new_str("active_kinds", 12), active_kinds);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_recording_status_obj, polar_h10_recording_status);

static mp_obj_t polar_h10_recording_list(mp_obj_t self_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);
    polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording is unsupported"));
}
static MP_DEFINE_CONST_FUN_OBJ_1(polar_h10_recording_list_obj, polar_h10_recording_list);

static mp_obj_t polar_h10_recording_default_config(mp_obj_t self_in, mp_obj_t kind_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);

    polar_recording_kind_key_t kind = polar_parse_recording_kind_key(kind_in);
    if (kind == POLAR_RECORDING_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid recording kind"));
    }
    if (!polar_recording_kind_is_supported_build(kind)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording kind unsupported"));
    }
    if (!polar_h10_recording_is_supported(self)) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording control unsupported"));
    }

    mp_obj_t dict = mp_obj_new_dict(2);
    switch (kind) {
        case POLAR_RECORDING_KIND_KEY_HR:
            mp_obj_dict_store(dict, mp_obj_new_str("sample_type", 11), mp_obj_new_str("hr", 2));
            mp_obj_dict_store(dict, mp_obj_new_str("interval_s", 10), mp_obj_new_int(1));
            return dict;
        default:
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording kind unsupported"));
            break;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_recording_default_config_obj, polar_h10_recording_default_config);

static mp_obj_t polar_h10_recording_delete(mp_obj_t self_in, mp_obj_t recording_id_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)recording_id_in;
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);
    polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("recording is unsupported"));
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_recording_delete_obj, polar_h10_recording_delete);

static mp_obj_t polar_h10_delete(mp_obj_t self_in, mp_obj_t path_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);
    if (!mp_obj_is_str(path_in)) {
        mp_raise_TypeError(MP_ERROR_TEXT("path must be str"));
    }
    size_t path_len = 0;
    const char *path = mp_obj_str_get_data(path_in, &path_len);
    if (path_len == 0 || path[0] != '/' || path_len > POLAR_SDK_PSFTP_MAX_PATH_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("path must be absolute"));
    }

#if !POLAR_SDK_HAS_BTSTACK
    polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("Bluetooth BTstack is not available in this build"));
#else
    if (!POLAR_CFG_ENABLE_PSFTP) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("filesystem delete is unsupported"));
    }

    polar_sdk_psftp_trans_result_t r = polar_psftp_execute_remove(
        self,
        path,
        path_len,
        POLAR_SDK_PSFTP_DEFAULT_TIMEOUT_MS);
    if (r != POLAR_SDK_PSFTP_TRANS_OK) {
        polar_raise_for_psftp_result(self, r);
    }
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_delete_obj, polar_h10_delete);

static mp_obj_t polar_call_typed_read_ecg(polar_h10_obj_t *self, mp_int_t max_bytes, mp_int_t timeout_ms) {
    mp_obj_t argv[1] = { MP_OBJ_FROM_PTR(self) };
    mp_map_elem_t kw_table[2] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR_max_bytes), mp_obj_new_int(max_bytes) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_timeout_ms), mp_obj_new_int(timeout_ms) },
    };
    mp_map_t kw_args = {
        .all_keys_are_qstrs = 1,
        .is_fixed = 1,
        .is_ordered = 1,
        .used = 2,
        .alloc = 2,
        .table = kw_table,
    };
    return polar_h10_read_ecg(1, argv, &kw_args);
}

static mp_obj_t polar_call_typed_read_acc(polar_h10_obj_t *self, mp_int_t max_bytes, mp_int_t timeout_ms) {
    mp_obj_t argv[1] = { MP_OBJ_FROM_PTR(self) };
    mp_map_elem_t kw_table[2] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR_max_bytes), mp_obj_new_int(max_bytes) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_timeout_ms), mp_obj_new_int(timeout_ms) },
    };
    mp_map_t kw_args = {
        .all_keys_are_qstrs = 1,
        .is_fixed = 1,
        .is_ordered = 1,
        .used = 2,
        .alloc = 2,
        .table = kw_table,
    };
    return polar_h10_read_imu(1, argv, &kw_args);
}

static mp_obj_t polar_h10_stream_default_config(mp_obj_t self_in, mp_obj_t kind_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_ready(self);
    polar_require_no_active_download_handle(self);
    polar_stream_kind_key_t kind = polar_parse_stream_kind_key(kind_in);

    mp_obj_t dict = mp_obj_new_dict(2);
    switch (kind) {
        case POLAR_STREAM_KIND_KEY_HR:
            return dict;
        case POLAR_STREAM_KIND_KEY_ECG:
            mp_obj_dict_store(dict, mp_obj_new_str("sample_rate_hz", 14), mp_obj_new_int(130));
            return dict;
        case POLAR_STREAM_KIND_KEY_ACC:
            mp_obj_dict_store(dict, mp_obj_new_str("sample_rate_hz", 14), mp_obj_new_int(50));
            mp_obj_dict_store(dict, mp_obj_new_str("range", 5), mp_obj_new_int(POLAR_SDK_IMU_DEFAULT_RANGE_G));
            return dict;
        case POLAR_STREAM_KIND_KEY_PPG:
        case POLAR_STREAM_KIND_KEY_PPI:
        case POLAR_STREAM_KIND_KEY_GYRO:
        case POLAR_STREAM_KIND_KEY_MAG:
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("stream kind unsupported"));
            break;
        case POLAR_STREAM_KIND_KEY_INVALID:
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
            break;
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_stream_default_config_obj, polar_h10_stream_default_config);

static mp_obj_t polar_h10_start_stream(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_kind,
        ARG_format,
        ARG_sample_rate_hz,
        ARG_range,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kind, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_sample_rate_hz, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_range, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    polar_require_no_active_download_handle(self);
    polar_stream_kind_key_t kind = polar_parse_stream_kind_key(parsed_args[ARG_kind].u_obj);
    if (kind == POLAR_STREAM_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
    }

    polar_stream_format_t requested_format = polar_parse_stream_format_or_default(
        parsed_args[ARG_format].u_obj,
        polar_default_stream_format_for_kind(kind));

    mp_obj_t argv[1] = { args[0] };
    switch (kind) {
        case POLAR_STREAM_KIND_KEY_HR:
            if (parsed_args[ARG_sample_rate_hz].u_obj != mp_const_none || parsed_args[ARG_range].u_obj != mp_const_none) {
                mp_raise_ValueError(MP_ERROR_TEXT("unsupported HR stream config"));
            }
            if (!POLAR_CFG_ENABLE_HR) {
                polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("HR feature disabled at build time"));
            }
            if (self->hr_enabled) {
                if (self->hr_stream_format == requested_format) {
                    return mp_const_none;
                }
                polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("HR stream already active with different format"));
            }
            self->hr_stream_format = requested_format;
            polar_hr_set_notify(self, true);
            if (requested_format == POLAR_STREAM_FORMAT_DECODED) {
                self->hr_consumed_seq = self->hr_state.sample_seq;
            } else {
                self->hr_raw_consumed_seq = self->hr_raw_seq;
            }
            return mp_const_none;

        case POLAR_STREAM_KIND_KEY_ECG: {
            if (requested_format != POLAR_STREAM_FORMAT_DECODED) {
                polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ECG raw stream format not implemented"));
            }
            self->ecg_stream_format = requested_format;

            mp_map_elem_t kw_table[1];
            mp_map_t ecg_kw = {
                .all_keys_are_qstrs = 1,
                .is_fixed = 1,
                .is_ordered = 1,
                .used = 0,
                .alloc = 1,
                .table = kw_table,
            };
            if (parsed_args[ARG_sample_rate_hz].u_obj != mp_const_none) {
                kw_table[0].key = MP_OBJ_NEW_QSTR(MP_QSTR_sample_rate);
                kw_table[0].value = parsed_args[ARG_sample_rate_hz].u_obj;
                ecg_kw.used = 1;
            }
            return polar_h10_start_ecg(1, argv, &ecg_kw);
        }

        case POLAR_STREAM_KIND_KEY_ACC: {
            if (requested_format != POLAR_STREAM_FORMAT_DECODED) {
                polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("ACC raw stream format not implemented"));
            }
            self->acc_stream_format = requested_format;

            mp_map_elem_t kw_table[2];
            size_t used = 0;
            if (parsed_args[ARG_sample_rate_hz].u_obj != mp_const_none) {
                kw_table[used].key = MP_OBJ_NEW_QSTR(MP_QSTR_sample_rate);
                kw_table[used].value = parsed_args[ARG_sample_rate_hz].u_obj;
                used += 1;
            }
            if (parsed_args[ARG_range].u_obj != mp_const_none) {
                kw_table[used].key = MP_OBJ_NEW_QSTR(MP_QSTR_range);
                kw_table[used].value = parsed_args[ARG_range].u_obj;
                used += 1;
            }

            mp_map_t acc_kw = {
                .all_keys_are_qstrs = 1,
                .is_fixed = 1,
                .is_ordered = 1,
                .used = used,
                .alloc = 2,
                .table = kw_table,
            };
            return polar_h10_start_imu(1, argv, &acc_kw);
        }

        case POLAR_STREAM_KIND_KEY_PPG:
        case POLAR_STREAM_KIND_KEY_PPI:
        case POLAR_STREAM_KIND_KEY_GYRO:
        case POLAR_STREAM_KIND_KEY_MAG:
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("stream kind unsupported"));
            break;
        case POLAR_STREAM_KIND_KEY_INVALID:
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
            break;
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_start_stream_obj, 1, polar_h10_start_stream);

static mp_obj_t polar_h10_stop_stream(mp_obj_t self_in, mp_obj_t kind_in) {
    polar_h10_obj_t *self = MP_OBJ_TO_PTR(self_in);
    polar_require_no_active_download_handle(self);
    polar_stream_kind_key_t kind = polar_parse_stream_kind_key(kind_in);

    switch (kind) {
        case POLAR_STREAM_KIND_KEY_HR:
            return polar_h10_stop_hr(self_in);
        case POLAR_STREAM_KIND_KEY_ECG:
            return polar_h10_stop_ecg(self_in);
        case POLAR_STREAM_KIND_KEY_ACC:
            return polar_h10_stop_imu(self_in);
        case POLAR_STREAM_KIND_KEY_PPG:
        case POLAR_STREAM_KIND_KEY_PPI:
        case POLAR_STREAM_KIND_KEY_GYRO:
        case POLAR_STREAM_KIND_KEY_MAG:
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("stream kind unsupported"));
            break;
        case POLAR_STREAM_KIND_KEY_INVALID:
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
            break;
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(polar_h10_stop_stream_obj, polar_h10_stop_stream);

static mp_obj_t polar_h10_read_stream_core(
    polar_h10_obj_t *self,
    polar_stream_kind_key_t kind,
    mp_int_t max_bytes,
    mp_int_t timeout_ms) {
    if (max_bytes <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be > 0"));
    }
    if (timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be >= 0"));
    }

    if (kind == POLAR_STREAM_KIND_KEY_HR) {
        if (!POLAR_CFG_ENABLE_HR) {
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("HR feature disabled at build time"));
        }
        polar_require_ready(self);
        if (!self->hr_enabled) {
            polar_raise_exc(&polar_type_Error, MP_ERROR_TEXT("HR stream is not active"));
        }

        if (self->hr_stream_format == POLAR_STREAM_FORMAT_DECODED) {
            if (self->hr_consumed_seq >= self->hr_state.sample_seq && timeout_ms > 0) {
                uint32_t start_ms = polar_now_ms();
                while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
                    polar_require_ready(self);
                    if (self->hr_consumed_seq < self->hr_state.sample_seq) {
                        break;
                    }
                    mp_event_wait_ms(10);
                }
            }

            if (self->hr_consumed_seq >= self->hr_state.sample_seq) {
                return mp_obj_new_bytes((const uint8_t *)"", 0);
            }

            const size_t frame_len = 28u + 12u;
            if ((size_t)max_bytes < frame_len) {
                polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("buffer too small for decoded HR sample"));
            }

            self->hr_consumed_seq = self->hr_state.sample_seq;
            return polar_build_decoded_hr_chunk(&self->hr_state);
        }

        if (self->hr_raw_consumed_seq >= self->hr_raw_seq && timeout_ms > 0) {
            uint32_t start_ms = polar_now_ms();
            while (!polar_elapsed_ms(start_ms, (uint32_t)timeout_ms)) {
                polar_require_ready(self);
                if (self->hr_raw_consumed_seq < self->hr_raw_seq) {
                    break;
                }
                mp_event_wait_ms(10);
            }
        }

        if (self->hr_raw_consumed_seq >= self->hr_raw_seq || self->hr_raw_last_len == 0) {
            return mp_obj_new_bytes((const uint8_t *)"", 0);
        }

        size_t framed_len = (size_t)self->hr_raw_last_len + 2u;
        if ((size_t)max_bytes < framed_len) {
            polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("buffer too small for raw HR sample"));
        }

        uint8_t *tmp = m_new(uint8_t, framed_len);
        uint16_t raw_len = self->hr_raw_last_len;
        tmp[0] = (uint8_t)(raw_len & 0xffu);
        tmp[1] = (uint8_t)((raw_len >> 8) & 0xffu);
        memcpy(&tmp[2], self->hr_raw_last, raw_len);

        self->hr_raw_consumed_seq = self->hr_raw_seq;
        mp_obj_t out = mp_obj_new_bytes(tmp, framed_len);
        m_del(uint8_t, tmp, framed_len);
        return out;
    }

    if (kind == POLAR_STREAM_KIND_KEY_ECG || kind == POLAR_STREAM_KIND_KEY_ACC) {
        const bool is_ecg = (kind == POLAR_STREAM_KIND_KEY_ECG);
        polar_stream_format_t selected = is_ecg ? self->ecg_stream_format : self->acc_stream_format;
        const uint32_t sample_size = is_ecg ? 4u : 6u;
        if (selected == POLAR_STREAM_FORMAT_RAW) {
            polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("raw stream format not implemented for this kind"));
        }

        const size_t header_len = 28;
        if ((size_t)max_bytes < header_len + sample_size) {
            polar_raise_exc(&polar_type_BufferOverflowError, MP_ERROR_TEXT("buffer too small for decoded stream sample"));
        }

        size_t payload_budget = (size_t)max_bytes - header_len;
        payload_budget -= (payload_budget % sample_size);

        mp_obj_t payload_obj = is_ecg
            ? polar_call_typed_read_ecg(self, (mp_int_t)payload_budget, timeout_ms)
            : polar_call_typed_read_acc(self, (mp_int_t)payload_budget, timeout_ms);

        size_t payload_len = 0;
        const uint8_t *payload = (const uint8_t *)mp_obj_str_get_data(payload_obj, &payload_len);
        if (payload_len == 0) {
            return payload_obj;
        }

        return polar_build_decoded_chunk_v1(
            is_ecg ? 1 : 2,
            is_ecg ? 2 : 3,
            0,
            0,
            0,
            0,
            sample_size,
            payload,
            payload_len);
    }

    if (kind == POLAR_STREAM_KIND_KEY_PPG || kind == POLAR_STREAM_KIND_KEY_PPI ||
        kind == POLAR_STREAM_KIND_KEY_GYRO || kind == POLAR_STREAM_KIND_KEY_MAG) {
        polar_raise_exc(&polar_type_UnsupportedError, MP_ERROR_TEXT("stream kind unsupported"));
    }

    mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
    return mp_const_none;
}

static mp_obj_t polar_h10_read_stream(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_kind,
        ARG_max_bytes,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kind, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1024 } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    polar_stream_kind_key_t kind = polar_parse_stream_kind_key(parsed_args[ARG_kind].u_obj);
    if (kind == POLAR_STREAM_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
    }

    return polar_h10_read_stream_core(
        self,
        kind,
        parsed_args[ARG_max_bytes].u_int,
        parsed_args[ARG_timeout_ms].u_int);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_read_stream_obj, 1, polar_h10_read_stream);

static mp_obj_t polar_h10_read_stream_into(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum {
        ARG_kind,
        ARG_buf,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kind, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_buf, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    polar_h10_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    polar_stream_kind_key_t kind = polar_parse_stream_kind_key(parsed_args[ARG_kind].u_obj);
    if (kind == POLAR_STREAM_KIND_KEY_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid stream kind"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(parsed_args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_WRITE);
    if (bufinfo.len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must not be empty"));
    }

    mp_obj_t chunk = polar_h10_read_stream_core(
        self,
        kind,
        (mp_int_t)bufinfo.len,
        parsed_args[ARG_timeout_ms].u_int);

    size_t n = 0;
    const uint8_t *src = (const uint8_t *)mp_obj_str_get_data(chunk, &n);
    if (n > bufinfo.len) {
        n = bufinfo.len;
    }
    if (n > 0) {
        memcpy(bufinfo.buf, src, n);
    }

    return mp_obj_new_int_from_uint((mp_uint_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(polar_h10_read_stream_into_obj, 1, polar_h10_read_stream_into);

static const mp_rom_map_elem_t polar_h10_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&polar_h10_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&polar_h10_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_required_capabilities), MP_ROM_PTR(&polar_h10_required_capabilities_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_required_capabilities), MP_ROM_PTR(&polar_h10_set_required_capabilities_obj) },
    { MP_ROM_QSTR(MP_QSTR_capabilities), MP_ROM_PTR(&polar_h10_capabilities_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&polar_h10_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&polar_h10_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&polar_h10_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_hr), MP_ROM_PTR(&polar_h10_start_hr_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_hr), MP_ROM_PTR(&polar_h10_stop_hr_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_hr), MP_ROM_PTR(&polar_h10_read_hr_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_ecg), MP_ROM_PTR(&polar_h10_start_ecg_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_ecg), MP_ROM_PTR(&polar_h10_stop_ecg_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_ecg), MP_ROM_PTR(&polar_h10_read_ecg_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_acc), MP_ROM_PTR(&polar_h10_start_imu_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_acc), MP_ROM_PTR(&polar_h10_stop_imu_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_acc), MP_ROM_PTR(&polar_h10_read_imu_obj) },
    { MP_ROM_QSTR(MP_QSTR_stream_default_config), MP_ROM_PTR(&polar_h10_stream_default_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_stream), MP_ROM_PTR(&polar_h10_start_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_stream), MP_ROM_PTR(&polar_h10_stop_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_stream), MP_ROM_PTR(&polar_h10_read_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_stream_into), MP_ROM_PTR(&polar_h10_read_stream_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_start), MP_ROM_PTR(&polar_h10_recording_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_stop), MP_ROM_PTR(&polar_h10_recording_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_status), MP_ROM_PTR(&polar_h10_recording_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_list), MP_ROM_PTR(&polar_h10_recording_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_default_config), MP_ROM_PTR(&polar_h10_recording_default_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_recording_delete), MP_ROM_PTR(&polar_h10_recording_delete_obj) },
    { MP_ROM_QSTR(MP_QSTR_list_dir), MP_ROM_PTR(&polar_h10_list_dir_obj) },
    { MP_ROM_QSTR(MP_QSTR_delete), MP_ROM_PTR(&polar_h10_delete_obj) },
    { MP_ROM_QSTR(MP_QSTR_download), MP_ROM_PTR(&polar_h10_download_obj) },
    { MP_ROM_QSTR(MP_QSTR_download_open), MP_ROM_PTR(&polar_h10_download_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_download_read), MP_ROM_PTR(&polar_h10_download_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_download_close), MP_ROM_PTR(&polar_h10_download_close_obj) },
};
static MP_DEFINE_CONST_DICT(polar_h10_locals_dict, polar_h10_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    polar_h10_type,
    MP_QSTR_Device,
    MP_TYPE_FLAG_NONE,
    make_new, polar_h10_make_new,
    locals_dict, &polar_h10_locals_dict
    );

// -----------------------------------------------------------------------------
// Module-level helpers

static mp_obj_t polar_version(void) {
    return mp_obj_new_str("0.1.0-dev", 9);
}
static MP_DEFINE_CONST_FUN_OBJ_0(polar_version_obj, polar_version);

static mp_obj_t polar_build_info(void) {
    mp_obj_t dict = mp_obj_new_dict(11);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_version), mp_obj_new_str("0.1.0-dev", 9));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_git_sha), mp_obj_new_str(POLAR_BUILD_GIT_SHA, strlen(POLAR_BUILD_GIT_SHA)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_git_dirty), mp_obj_new_str(POLAR_BUILD_GIT_DIRTY, strlen(POLAR_BUILD_GIT_DIRTY)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_preset), mp_obj_new_str(POLAR_BUILD_PRESET, strlen(POLAR_BUILD_PRESET)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_build_type), mp_obj_new_str(POLAR_BUILD_TYPE, strlen(POLAR_BUILD_TYPE)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_has_btstack), mp_obj_new_int(POLAR_SDK_HAS_BTSTACK));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_hr), mp_obj_new_int(POLAR_CFG_ENABLE_HR));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_ecg), mp_obj_new_int(POLAR_CFG_ENABLE_ECG));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_feature_psftp), mp_obj_new_int(POLAR_CFG_ENABLE_PSFTP));
    #if POLAR_SDK_HAS_BTSTACK
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_btstack_config), polar_btstack_config_snapshot_dict());
    #endif
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(polar_build_info_obj, polar_build_info);

static MP_DEFINE_STR_OBJ(polar_cap_stream_hr_obj, "stream:hr");
static MP_DEFINE_STR_OBJ(polar_cap_stream_ecg_obj, "stream:ecg");
static MP_DEFINE_STR_OBJ(polar_cap_stream_acc_obj, "stream:acc");
static MP_DEFINE_STR_OBJ(polar_cap_stream_ppg_obj, "stream:ppg");
static MP_DEFINE_STR_OBJ(polar_cap_stream_ppi_obj, "stream:ppi");
static MP_DEFINE_STR_OBJ(polar_cap_stream_gyro_obj, "stream:gyro");
static MP_DEFINE_STR_OBJ(polar_cap_stream_mag_obj, "stream:mag");
static MP_DEFINE_STR_OBJ(polar_cap_recording_obj, "recording");
static MP_DEFINE_STR_OBJ(polar_cap_psftp_read_obj, "psftp:read");
static MP_DEFINE_STR_OBJ(polar_cap_psftp_delete_obj, "psftp:delete");
static MP_DEFINE_STR_OBJ(polar_device_key_obj, "Device");

// -----------------------------------------------------------------------------
// Module definition

static const mp_rom_map_elem_t polar_sdk_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_polar_sdk) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&polar_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_build_info), MP_ROM_PTR(&polar_build_info_obj) },
    { MP_ROM_PTR(&polar_device_key_obj), MP_ROM_PTR(&polar_h10_type) },

    // Error model
    { MP_ROM_QSTR(MP_QSTR_Error), MP_ROM_PTR(&polar_type_Error) },
    { MP_ROM_QSTR(MP_QSTR_TimeoutError), MP_ROM_PTR(&polar_type_TimeoutError) },
    { MP_ROM_QSTR(MP_QSTR_NotConnectedError), MP_ROM_PTR(&polar_type_NotConnectedError) },
    { MP_ROM_QSTR(MP_QSTR_ProtocolError), MP_ROM_PTR(&polar_type_ProtocolError) },
    { MP_ROM_QSTR(MP_QSTR_BufferOverflowError), MP_ROM_PTR(&polar_type_BufferOverflowError) },
    { MP_ROM_QSTR(MP_QSTR_SecurityError), MP_ROM_PTR(&polar_type_SecurityError) },
    { MP_ROM_QSTR(MP_QSTR_UnsupportedError), MP_ROM_PTR(&polar_type_UnsupportedError) },

    // Build-time feature flags
    { MP_ROM_QSTR(MP_QSTR_FEATURE_HR), MP_ROM_INT(POLAR_CFG_ENABLE_HR) },
    { MP_ROM_QSTR(MP_QSTR_FEATURE_ECG), MP_ROM_INT(POLAR_CFG_ENABLE_ECG) },
    { MP_ROM_QSTR(MP_QSTR_FEATURE_PSFTP), MP_ROM_INT(POLAR_CFG_ENABLE_PSFTP) },
    { MP_ROM_QSTR(MP_QSTR_HAS_BTSTACK), MP_ROM_INT(POLAR_SDK_HAS_BTSTACK) },

    // Capability constants
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_HR), MP_ROM_PTR(&polar_cap_stream_hr_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_ECG), MP_ROM_PTR(&polar_cap_stream_ecg_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_ACC), MP_ROM_PTR(&polar_cap_stream_acc_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_PPG), MP_ROM_PTR(&polar_cap_stream_ppg_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_PPI), MP_ROM_PTR(&polar_cap_stream_ppi_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_GYRO), MP_ROM_PTR(&polar_cap_stream_gyro_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_STREAM_MAG), MP_ROM_PTR(&polar_cap_stream_mag_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_RECORDING), MP_ROM_PTR(&polar_cap_recording_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_PSFTP_READ), MP_ROM_PTR(&polar_cap_psftp_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_CAP_PSFTP_DELETE), MP_ROM_PTR(&polar_cap_psftp_delete_obj) },
};
static const mp_obj_dict_t polar_sdk_module_globals = {
    .base = { &mp_type_dict },
    .map = {
        .all_keys_are_qstrs = 0,
        .is_fixed = 1,
        .is_ordered = 1,
        .used = MP_ARRAY_SIZE(polar_sdk_module_globals_table),
        .alloc = MP_ARRAY_SIZE(polar_sdk_module_globals_table),
        .table = (mp_map_elem_t *)(mp_rom_map_elem_t *)polar_sdk_module_globals_table,
    },
};

const mp_obj_module_t polar_sdk_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&polar_sdk_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_polar_sdk, polar_sdk_user_cmodule);
