// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack.h"

#include "polar_sdk_runtime.h"
#include "polar_sdk_transport_adapter.h"
#include "polar_sdk_btstack_dispatch.h"
#include "polar_sdk_btstack_adv_runtime.h"
#include "polar_sdk_btstack_scan.h"
#include "polar_sdk_btstack_helpers.h"
#include "polar_sdk_btstack_sm.h"
#include "polar_sdk_sm_control.h"
#include "polar_sdk_btstack_gatt.h"
#include "polar_sdk_btstack_gatt_route.h"
#include "polar_sdk_discovery.h"
#include "polar_sdk_gatt_notify_runtime.h"
#include "polar_sdk_pmd.h"
#include "polar_sdk_psftp.h"
#include "polar_sdk_psftp_runtime.h"
#include "polar_sdk_security.h"

#ifndef H10_TARGET_ADDR
#define H10_TARGET_ADDR "24:AC:AC:05:A3:10"
#endif

#ifndef H10_MAX_ROUNDS
#define H10_MAX_ROUNDS 10
#endif

#ifndef H10_DOWNLOAD_MAX_BYTES
#define H10_DOWNLOAD_MAX_BYTES 4096
#endif

#ifndef H10_CLEAR_BOND_ON_BOOT
// 1 = clear stored bond for target device once at startup (repeatability mode)
// 0 = keep normal bonding behavior
#define H10_CLEAR_BOND_ON_BOOT 1
#endif

#ifndef H10_PSFTP_TX_CHAR_MODE
// 0 = write PSFTP request frames to MTU characteristic (official SDK parity)
// 1 = write PSFTP request frames to H2D characteristic (debug experiment)
#define H10_PSFTP_TX_CHAR_MODE 0
#endif

#ifndef H10_PSFTP_WRITE_MODE
// 0 = ATT Write Request (always with response)
// 1 = ATT Write Command (always without response)
// 2 = SDK-like cadence: mostly without response, every Nth packet with response
#define H10_PSFTP_WRITE_MODE 2
#endif

#ifndef H10_PSFTP_WRITE_PERIOD_N
// Used when H10_PSFTP_WRITE_MODE=2. If 0, all packets are without response.
#define H10_PSFTP_WRITE_PERIOD_N 5
#endif

#ifndef H10_PSFTP_RAW_VALUE_TAP
// 1 = log unfiltered GATT value events (pre-router), 0 = disabled
#define H10_PSFTP_RAW_VALUE_TAP 1
#endif

#ifndef H10_PSFTP_TX_HEX_DUMP_BYTES
#define H10_PSFTP_TX_HEX_DUMP_BYTES 24
#endif

#ifndef H10_PSFTP_RX_HEX_DUMP_BYTES
#define H10_PSFTP_RX_HEX_DUMP_BYTES 24
#endif

#ifndef H10_PSFTP_TEST_PATH_LEN
// 1 => "/" (normal path). >1 => synthetic long GET path of requested length.
#define H10_PSFTP_TEST_PATH_LEN 1
#endif

#ifndef H10_PSFTP_PRE_TX_DELAY_MS
// Optional pause between channel prep and first PSFTP frame TX.
#define H10_PSFTP_PRE_TX_DELAY_MS 0
#endif

#ifndef H10_RECORDING_QUERY_STATUS
#define H10_RECORDING_QUERY_STATUS 1
#endif

#ifndef H10_RECORDING_START_STOP
#define H10_RECORDING_START_STOP 0
#endif

#ifndef H10_RECORDING_EXERCISE_ID
#define H10_RECORDING_EXERCISE_ID "SDKPROBE"
#endif

#ifndef H10_RECORDING_SAMPLE_TYPE
// 0 = HR, 1 = RR
#define H10_RECORDING_SAMPLE_TYPE 0
#endif

#ifndef H10_RECORDING_INTERVAL_S
// Valid values: 1 or 5
#define H10_RECORDING_INTERVAL_S 1
#endif

#define PSFTP_SERVICE_UUID16 (0xFEEE)

#define PSFTP_SECURITY_ROUNDS 3
#define PSFTP_SECURITY_WAIT_MS 3500
#define PSFTP_GATT_TIMEOUT_MS 5000
#define PSFTP_RESPONSE_TIMEOUT_MS 15000
#define RECONNECT_DELAY_MS 1200

#define PSFTP_QC_CTX_SERVICE_CCC   0x4343
#define PSFTP_QC_CTX_SERVICE_WRITE 0x5053

#define POST_CONN_INTERVAL_MIN 24
#define POST_CONN_INTERVAL_MAX 40
#define POST_CONN_LATENCY 0
#define POST_CONN_SUPERVISION_TIMEOUT_10MS 600

#define PSFTP_DIR_BUF_BYTES 8192
#define PSFTP_DL_BUF_BYTES 32768
#define PSFTP_MAX_DIR_ENTRIES 64

static const uint8_t UUID_PMD_SERVICE_BE[16] = {
    0xFB, 0x00, 0x5C, 0x80, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t UUID_PSFTP_MTU_BE[16] = {
    0xFB, 0x00, 0x5C, 0x51, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t UUID_PSFTP_D2H_BE[16] = {
    0xFB, 0x00, 0x5C, 0x52, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t UUID_PSFTP_H2D_BE[16] = {
    0xFB, 0x00, 0x5C, 0x53, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

typedef enum {
    APP_OFF = 0,
    APP_SCANNING,
    APP_CONNECTING,
    APP_CONNECTED,
    APP_W4_PSFTP_SERVICE,
    APP_W4_PSFTP_CHARS,
    APP_READY,
    APP_RUNNING_ROUND,
    APP_DONE,
} app_state_t;

static app_state_t app_state = APP_OFF;

static btstack_packet_callback_registration_t hci_event_cb;
static btstack_packet_callback_registration_t sm_event_cb;
static btstack_timer_source_t reconnect_timer;
static btstack_timer_source_t heartbeat_timer;

static polar_sdk_runtime_link_t runtime_link;

static bool target_addr_valid = false;
static bd_addr_t target_addr;
static bd_addr_t peer_addr;
static bd_addr_type_t peer_addr_type = BD_ADDR_TYPE_UNKNOWN;

static bool connected = false;
static hci_con_handle_t conn_handle = HCI_CON_HANDLE_INVALID;
static bool connect_intent = false;
static bool user_disconnect_requested = false;
static bool bond_clear_done = false;

static gatt_client_service_t psftp_service;
static bool psftp_service_found = false;

static gatt_client_characteristic_t psftp_mtu_char;
static gatt_client_characteristic_t psftp_d2h_char;
static gatt_client_characteristic_t psftp_h2d_char;
static bool psftp_mtu_found = false;
static bool psftp_d2h_found = false;
static bool psftp_h2d_found = false;

static gatt_client_notification_t psftp_mtu_listener;
static gatt_client_notification_t psftp_d2h_listener;
static bool psftp_mtu_listener_registered = false;
static bool psftp_d2h_listener_registered = false;
static bool psftp_mtu_enabled = false;
static bool psftp_d2h_enabled = false;

static bool cfg_pending = false;
static bool cfg_done = false;
static uint8_t cfg_att_status = ATT_ERROR_SUCCESS;
static uint16_t cfg_expected_ctx_conn = 0;

static bool write_pending = false;
static bool write_done = false;
static uint8_t write_att_status = ATT_ERROR_SUCCESS;
static uint16_t write_expected_ctx_conn = 0;
static uint16_t write_ctx_seq = 0;
static uint32_t write_packet_index = 0;

static uint32_t qc_event_total = 0;
static uint32_t qc_match_cfg_total = 0;
static uint32_t qc_match_write_total = 0;
static uint16_t qc_last_service_id = 0;
static uint16_t qc_last_connection_id = 0;
static uint8_t qc_last_att_status = ATT_ERROR_SUCCESS;

static bool psftp_response_waiting = false;
static bool psftp_response_done = false;
static polar_sdk_psftp_rx_result_t psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
static polar_sdk_psftp_rx_state_t psftp_rx_state;

static uint16_t att_mtu = ATT_DEFAULT_MTU;

static uint8_t psftp_dir_buf[PSFTP_DIR_BUF_BYTES];
static uint8_t psftp_dl_buf[PSFTP_DL_BUF_BYTES];

static uint32_t rounds_total = 0;
static uint32_t rounds_ok = 0;
static uint32_t rounds_fail = 0;
static bool probe_done = false;

static uint32_t sm_just_works_total = 0;
static uint32_t sm_pairing_complete_total = 0;
static uint8_t sm_last_pairing_status = 0;
static uint8_t sm_last_pairing_reason = 0;

static uint32_t psftp_tx_frames_total = 0;
static uint32_t psftp_rx_frames_total = 0;

static uint32_t raw_value_events_total = 0;
static uint32_t raw_value_mtu_total = 0;
static uint32_t raw_value_d2h_total = 0;
static uint32_t raw_value_other_total = 0;

static uint32_t routed_psftp_mtu_total = 0;
static uint32_t routed_psftp_d2h_total = 0;
static uint32_t routed_query_complete_total = 0;

typedef enum {
    PSFTP_VERDICT_UNKNOWN = 0,
    PSFTP_VERDICT_A_SECURITY,
    PSFTP_VERDICT_B_DISCOVERY_OR_CCC,
    PSFTP_VERDICT_C_TX_TRANSPORT,
    PSFTP_VERDICT_D_FILTER_OR_ROUTE,
    PSFTP_VERDICT_E_PARSER,
    PSFTP_VERDICT_F_REMOTE_ERROR,
} psftp_verdict_t;

typedef struct {
    bool success;
    int prep_code;
    int write_code;
    bool response_timeout;
    bool remote_error;
    uint16_t remote_error_code;
    polar_sdk_psftp_rx_result_t response_result;
    uint32_t tx_delta;
    uint32_t rx_delta;
    uint32_t raw_mtu_delta;
    uint32_t raw_d2h_delta;
    uint32_t raw_other_delta;
    uint32_t routed_mtu_delta;
    uint32_t routed_d2h_delta;
} psftp_diag_result_t;

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t raw_mtu;
    uint32_t raw_d2h;
    uint32_t raw_other;
    uint32_t routed_mtu;
    uint32_t routed_d2h;
} psftp_diag_snapshot_t;

static psftp_diag_result_t psftp_last_diag;
static psftp_verdict_t psftp_last_verdict = PSFTP_VERDICT_UNKNOWN;

static bool psftp_security_ready(void);
static void handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static const char *state_name(app_state_t s) {
    switch (s) {
        case APP_OFF: return "OFF";
        case APP_SCANNING: return "SCANNING";
        case APP_CONNECTING: return "CONNECTING";
        case APP_CONNECTED: return "CONNECTED";
        case APP_W4_PSFTP_SERVICE: return "W4_PSFTP_SERVICE";
        case APP_W4_PSFTP_CHARS: return "W4_PSFTP_CHARS";
        case APP_READY: return "READY";
        case APP_RUNNING_ROUND: return "RUNNING_ROUND";
        case APP_DONE: return "DONE";
        default: return "?";
    }
}

static const char *psftp_verdict_name(psftp_verdict_t verdict) {
    switch (verdict) {
        case PSFTP_VERDICT_A_SECURITY: return "A_SECURITY";
        case PSFTP_VERDICT_B_DISCOVERY_OR_CCC: return "B_DISCOVERY_OR_CCC";
        case PSFTP_VERDICT_C_TX_TRANSPORT: return "C_TX_TRANSPORT";
        case PSFTP_VERDICT_D_FILTER_OR_ROUTE: return "D_FILTER_OR_ROUTE";
        case PSFTP_VERDICT_E_PARSER: return "E_PARSER";
        case PSFTP_VERDICT_F_REMOTE_ERROR: return "F_REMOTE_ERROR";
        default: return "UNKNOWN";
    }
}

static const char *psftp_tx_mode_name(void) {
#if H10_PSFTP_TX_CHAR_MODE == 1
    return "H2D";
#else
    return "MTU";
#endif
}

static const char *psftp_write_mode_name(void) {
#if H10_PSFTP_WRITE_MODE == 1
    return "WITHOUT_RESPONSE";
#elif H10_PSFTP_WRITE_MODE == 2
    return "PERIODIC";
#else
    return "WITH_RESPONSE";
#endif
}

static bool psftp_write_should_use_response(uint32_t packet_index) {
#if H10_PSFTP_WRITE_MODE == 1
    (void)packet_index;
    return false;
#elif H10_PSFTP_WRITE_MODE == 2
    if (H10_PSFTP_WRITE_PERIOD_N <= 0) {
        return false;
    }
    return (packet_index % (uint32_t)H10_PSFTP_WRITE_PERIOD_N) == 0;
#else
    (void)packet_index;
    return true;
#endif
}

static const char *psftp_write_opcode_name(bool with_response) {
    return with_response ? "REQ" : "CMD";
}

static uint16_t psftp_tx_value_handle(void) {
#if H10_PSFTP_TX_CHAR_MODE == 1
    return psftp_h2d_char.value_handle;
#else
    return psftp_mtu_char.value_handle;
#endif
}

static void print_hex_prefix(const uint8_t *data, uint16_t len, uint16_t max_bytes) {
    if (data == NULL || len == 0 || max_bytes == 0) {
        printf("-");
        return;
    }
    uint16_t n = len < max_bytes ? len : max_bytes;
    for (uint16_t i = 0; i < n; ++i) {
        printf("%02x", data[i]);
    }
    if (len > n) {
        printf("...");
    }
}

static void psftp_diag_reset(void) {
    memset(&psftp_last_diag, 0, sizeof(psftp_last_diag));
    psftp_last_diag.response_result = POLAR_SDK_PSFTP_RX_MORE;
    psftp_last_verdict = PSFTP_VERDICT_UNKNOWN;
}

static psftp_diag_snapshot_t psftp_diag_snapshot_take(void) {
    psftp_diag_snapshot_t s = {
        .tx_frames = psftp_tx_frames_total,
        .rx_frames = psftp_rx_frames_total,
        .raw_mtu = raw_value_mtu_total,
        .raw_d2h = raw_value_d2h_total,
        .raw_other = raw_value_other_total,
        .routed_mtu = routed_psftp_mtu_total,
        .routed_d2h = routed_psftp_d2h_total,
    };
    return s;
}

static void psftp_diag_finalize(const psftp_diag_snapshot_t *before, bool success) {
    if (before == NULL) {
        return;
    }

    psftp_last_diag.success = success;
    psftp_last_diag.tx_delta = psftp_tx_frames_total - before->tx_frames;
    psftp_last_diag.rx_delta = psftp_rx_frames_total - before->rx_frames;
    psftp_last_diag.raw_mtu_delta = raw_value_mtu_total - before->raw_mtu;
    psftp_last_diag.raw_d2h_delta = raw_value_d2h_total - before->raw_d2h;
    psftp_last_diag.raw_other_delta = raw_value_other_total - before->raw_other;
    psftp_last_diag.routed_mtu_delta = routed_psftp_mtu_total - before->routed_mtu;
    psftp_last_diag.routed_d2h_delta = routed_psftp_d2h_total - before->routed_d2h;
    psftp_last_diag.response_result = psftp_response_result;

    if (success) {
        psftp_last_verdict = PSFTP_VERDICT_UNKNOWN;
        return;
    }

    if (psftp_last_diag.prep_code != 0) {
        if (!psftp_security_ready()) {
            psftp_last_verdict = PSFTP_VERDICT_A_SECURITY;
        } else {
            psftp_last_verdict = PSFTP_VERDICT_B_DISCOVERY_OR_CCC;
        }
        return;
    }

    if (psftp_last_diag.remote_error) {
        psftp_last_verdict = PSFTP_VERDICT_F_REMOTE_ERROR;
        return;
    }

    if (psftp_last_diag.response_result == POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR ||
        psftp_last_diag.response_result == POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR ||
        psftp_last_diag.response_result == POLAR_SDK_PSFTP_RX_OVERFLOW) {
        psftp_last_verdict = PSFTP_VERDICT_E_PARSER;
        return;
    }

    if (psftp_last_diag.tx_delta > 0 && psftp_last_diag.rx_delta == 0) {
        if ((psftp_last_diag.raw_mtu_delta + psftp_last_diag.raw_d2h_delta) > 0 &&
            (psftp_last_diag.routed_mtu_delta + psftp_last_diag.routed_d2h_delta) == 0) {
            psftp_last_verdict = PSFTP_VERDICT_D_FILTER_OR_ROUTE;
        } else {
            psftp_last_verdict = PSFTP_VERDICT_C_TX_TRANSPORT;
        }
        return;
    }

    if (psftp_last_diag.write_code != 0 || psftp_last_diag.response_timeout) {
        psftp_last_verdict = PSFTP_VERDICT_C_TX_TRANSPORT;
        return;
    }

    psftp_last_verdict = PSFTP_VERDICT_UNKNOWN;
}

static void probe_sleep_ms(uint32_t ms) {
    while (ms--) {
        cyw43_arch_poll();
        sleep_ms(1);
    }
}

static bool wait_flag_until_true(volatile bool *flag, uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (*flag) {
            return true;
        }
        if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
            return false;
        }
        probe_sleep_ms(1);
        elapsed += 1;
    }
    return *flag;
}

static bool psftp_security_ready(void) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }
    return gap_encryption_key_size(conn_handle) > 0;
}

static void schedule_reconnect(uint32_t delay_ms);

static void maybe_clear_bond_for_addr(bd_addr_type_t addr_type, const bd_addr_t addr, const char *origin) {
#if H10_CLEAR_BOND_ON_BOOT
    if (bond_clear_done) {
        return;
    }

    bd_addr_t target;
    memcpy(target, addr, sizeof(target));

    if (addr_type == BD_ADDR_TYPE_LE_PUBLIC || addr_type == BD_ADDR_TYPE_LE_RANDOM) {
        gap_delete_bonding(addr_type, target);
        printf("[psftp-probe] bond clear origin=%s addr=%s type=%u\n", origin, bd_addr_to_str(target), addr_type);
    } else {
        gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, target);
        gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, target);
        printf("[psftp-probe] bond clear origin=%s addr=%s type=public+random\n", origin, bd_addr_to_str(target));
    }

    bond_clear_done = true;
#else
    (void)addr_type;
    (void)addr;
    (void)origin;
#endif
}

static void on_disconnect_cleanup(void) {
    connected = false;
    conn_handle = HCI_CON_HANDLE_INVALID;

    if (psftp_mtu_listener_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&psftp_mtu_listener);
        psftp_mtu_listener_registered = false;
    }
    if (psftp_d2h_listener_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&psftp_d2h_listener);
        psftp_d2h_listener_registered = false;
    }

    memset(&psftp_service, 0, sizeof(psftp_service));
    memset(&psftp_mtu_char, 0, sizeof(psftp_mtu_char));
    memset(&psftp_d2h_char, 0, sizeof(psftp_d2h_char));
    memset(&psftp_h2d_char, 0, sizeof(psftp_h2d_char));

    psftp_service_found = false;
    psftp_mtu_found = false;
    psftp_d2h_found = false;
    psftp_h2d_found = false;

    psftp_mtu_enabled = false;
    psftp_d2h_enabled = false;

    cfg_pending = false;
    cfg_done = false;
    cfg_att_status = ATT_ERROR_SUCCESS;

    write_pending = false;
    write_done = false;
    write_att_status = ATT_ERROR_SUCCESS;

    psftp_response_waiting = false;
    psftp_response_done = false;
    psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    polar_sdk_psftp_rx_reset(&psftp_rx_state, NULL, 0);

    psftp_diag_reset();

    att_mtu = ATT_DEFAULT_MTU;

    app_state = probe_done ? APP_DONE : APP_OFF;
}

static void start_scan(void) {
    if (probe_done) {
        return;
    }
    if (app_state == APP_SCANNING || app_state == APP_CONNECTING) {
        return;
    }

    app_state = APP_SCANNING;
    connect_intent = true;
    user_disconnect_requested = false;
    runtime_link.state = POLAR_SDK_RUNTIME_STATE_SCANNING;

    printf("[psftp-probe] start scan target=%s valid=%d\n", H10_TARGET_ADDR, target_addr_valid);
    if (target_addr_valid) {
        maybe_clear_bond_for_addr(BD_ADDR_TYPE_UNKNOWN, target_addr, "start_scan");
    }
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    gap_start_scan();
}

static void reconnect_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    start_scan();
}

static void schedule_reconnect(uint32_t delay_ms) {
    if (probe_done) {
        return;
    }
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, reconnect_timer_handler);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void start_psftp_service_discovery(void) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    memset(&psftp_service, 0, sizeof(psftp_service));
    memset(&psftp_mtu_char, 0, sizeof(psftp_mtu_char));
    memset(&psftp_d2h_char, 0, sizeof(psftp_d2h_char));
    memset(&psftp_h2d_char, 0, sizeof(psftp_h2d_char));

    psftp_service_found = false;
    psftp_mtu_found = false;
    psftp_d2h_found = false;
    psftp_h2d_found = false;

    app_state = APP_W4_PSFTP_SERVICE;
    int err = gatt_client_discover_primary_services(handle_gatt_event, conn_handle);
    printf("[psftp-probe] discover services err=%d\n", err);
    if (err) {
        app_state = APP_CONNECTED;
    }
}

static void request_post_connect_update(hci_con_handle_t handle) {
    int err = gap_update_connection_parameters(
        handle,
        POST_CONN_INTERVAL_MIN,
        POST_CONN_INTERVAL_MAX,
        POST_CONN_LATENCY,
        POST_CONN_SUPERVISION_TIMEOUT_10MS);

    printf("[psftp-probe] post-connect update handle=0x%04x err=%d\n", handle, err);
}

static void on_connection_ready(hci_con_handle_t handle) {
    conn_handle = handle;
    connected = true;
    connect_intent = false;
    user_disconnect_requested = false;

    app_state = APP_CONNECTED;
    request_post_connect_update(conn_handle);
    start_psftp_service_discovery();
}

static void probe_sm_on_just_works_request(void *ctx, uint16_t handle) {
    (void)ctx;
    sm_just_works_total += 1;
    printf("[psftp-probe] SM just works request handle=0x%04x\n", handle);
    sm_just_works_confirm(handle);
}

static void probe_sm_on_numeric_comparison_request(void *ctx, uint16_t handle) {
    (void)ctx;
    sm_just_works_total += 1;
    printf("[psftp-probe] SM numeric comparison request handle=0x%04x\n", handle);
    sm_numeric_comparison_confirm(handle);
}

static void probe_sm_on_authorization_request(void *ctx, uint16_t handle) {
    (void)ctx;
    printf("[psftp-probe] SM authorization request handle=0x%04x\n", handle);
    sm_authorization_grant(handle);
}

static void probe_sm_on_pairing_complete(void *ctx, const polar_sdk_sm_event_t *event) {
    (void)ctx;
    sm_pairing_complete_total += 1;
    sm_last_pairing_status = event->status;
    sm_last_pairing_reason = event->reason;
    printf("[psftp-probe] SM pairing complete status=0x%02x reason=0x%02x\n",
           sm_last_pairing_status,
           sm_last_pairing_reason);
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_sdk_sm_event_t sm_event;
    if (!polar_sdk_btstack_decode_sm_event(packet_type, packet, &sm_event)) {
        return;
    }

    polar_sdk_sm_control_ops_t ops = {
        .ctx = NULL,
        .on_just_works_request = probe_sm_on_just_works_request,
        .on_numeric_comparison_request = probe_sm_on_numeric_comparison_request,
        .on_authorization_request = probe_sm_on_authorization_request,
        .on_pairing_complete = probe_sm_on_pairing_complete,
    };
    (void)polar_sdk_sm_control_apply(&sm_event, conn_handle, HCI_CON_HANDLE_INVALID, &ops);
}

static bool probe_adv_runtime_is_scanning(void *ctx) {
    (void)ctx;
    return app_state == APP_SCANNING;
}

static void probe_adv_runtime_on_match(void *ctx, const polar_sdk_btstack_adv_report_t *report) {
    (void)ctx;
    memcpy(peer_addr, report->addr, sizeof(peer_addr));
    peer_addr_type = report->addr_type;

    maybe_clear_bond_for_addr(peer_addr_type, peer_addr, "adv_match");

    printf("[psftp-probe] adv match addr=%s type=%u rssi=%d\n",
           bd_addr_to_str(peer_addr),
           peer_addr_type,
           report->rssi);
    app_state = APP_CONNECTING;
}

static int probe_adv_runtime_stop_scan(void *ctx) {
    (void)ctx;
    gap_stop_scan();
    return ERROR_CODE_SUCCESS;
}

static int probe_adv_runtime_connect(void *ctx, const uint8_t *addr, uint8_t addr_type) {
    (void)ctx;
    bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    return gap_connect(a, addr_type);
}

static void probe_adv_runtime_on_connect_error(void *ctx, int status) {
    (void)ctx;
    runtime_link.last_hci_status = (uint8_t)status;
    app_state = APP_OFF;
    schedule_reconnect(RECONNECT_DELAY_MS);
}

static void probe_dispatch_on_adv_report(void *ctx, const polar_sdk_btstack_adv_report_t *adv_report) {
    (void)ctx;

    polar_sdk_btstack_scan_filter_t filter = {
        .use_addr = target_addr_valid,
        .addr = {0},
        .use_name_prefix = false,
        .name_prefix = 0,
        .name_prefix_len = 0,
        .use_name_contains_pair = !target_addr_valid,
        .name_contains_a = "Polar",
        .name_contains_b = "H10",
    };
    if (target_addr_valid) {
        memcpy(filter.addr, target_addr, sizeof(bd_addr_t));
    }

    polar_sdk_btstack_adv_runtime_ops_t ops = {
        .ctx = NULL,
        .is_scanning = probe_adv_runtime_is_scanning,
        .on_report = NULL,
        .on_match = probe_adv_runtime_on_match,
        .stop_scan = probe_adv_runtime_stop_scan,
        .connect = probe_adv_runtime_connect,
        .on_connect_error = probe_adv_runtime_on_connect_error,
    };
    (void)polar_sdk_btstack_adv_runtime_on_report(
        &runtime_link,
        &filter,
        adv_report,
        ERROR_CODE_SUCCESS,
        &ops);
}

static void probe_dispatch_on_link_event(void *ctx, const polar_sdk_link_event_t *event) {
    (void)ctx;

    switch (event->type) {
        case POLAR_SDK_LINK_EVENT_CONN_COMPLETE:
            if (connected && event->status == ERROR_CODE_SUCCESS && conn_handle == event->handle) {
                printf("[psftp-probe] duplicate conn-complete ignored handle=0x%04x\n", event->handle);
                break;
            }

            polar_sdk_runtime_on_connection_complete(
                &runtime_link,
                HCI_CON_HANDLE_INVALID,
                event->status,
                event->handle,
                event->conn_interval,
                event->conn_latency,
                event->supervision_timeout_10ms);

            if (event->status == ERROR_CODE_SUCCESS) {
                printf("[psftp-probe] connected handle=0x%04x interval=%u latency=%u sup=%u\n",
                       event->handle,
                       event->conn_interval,
                       event->conn_latency,
                       event->supervision_timeout_10ms);
                on_connection_ready(event->handle);
            } else {
                printf("[psftp-probe] connection complete status=0x%02x\n", event->status);
                on_disconnect_cleanup();
                schedule_reconnect(RECONNECT_DELAY_MS);
            }
            break;

        case POLAR_SDK_LINK_EVENT_CONN_UPDATE_COMPLETE:
            polar_sdk_runtime_on_connection_update_complete(
                &runtime_link,
                event->status,
                event->conn_interval,
                event->conn_latency,
                event->supervision_timeout_10ms);
            printf("[psftp-probe] conn update status=0x%02x interval=%u latency=%u sup=%u\n",
                   event->status,
                   event->conn_interval,
                   event->conn_latency,
                   event->supervision_timeout_10ms);
            break;

        case POLAR_SDK_LINK_EVENT_DISCONNECT:
            polar_sdk_runtime_on_disconnect(
                &runtime_link,
                HCI_CON_HANDLE_INVALID,
                event->status,
                event->reason,
                user_disconnect_requested,
                connect_intent);

            printf("[psftp-probe] disconnect status=0x%02x reason=0x%02x\n",
                   event->status,
                   event->reason);
            user_disconnect_requested = false;
            connect_intent = false;
            on_disconnect_cleanup();
            schedule_reconnect(RECONNECT_DELAY_MS);
            break;
    }
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_sdk_btstack_dispatch_ops_t ops = {
        .ctx = NULL,
        .on_adv_report = probe_dispatch_on_adv_report,
        .on_link_event = probe_dispatch_on_link_event,
        .on_sm_event = NULL,
    };
    (void)polar_sdk_btstack_dispatch_event(packet_type, packet, &ops);

    uint8_t event_type = hci_event_packet_get_type(packet);
    if (event_type == BTSTACK_EVENT_STATE && btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
        start_scan();
        return;
    }

    if (event_type == GAP_EVENT_RSSI_MEASUREMENT) {
        printf("[psftp-probe] rssi handle=0x%04x rssi=%d\n",
               gap_event_rssi_measurement_get_con_handle(packet),
               (int)gap_event_rssi_measurement_get_rssi(packet));
    }
}

typedef struct {
    gatt_client_characteristic_t *chr;
    gatt_client_notification_t *notification;
    bool *listening;
} psftp_notify_ctx_t;

static bool psftp_notify_is_connected_ready(void *ctx) {
    (void)ctx;
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool psftp_notify_listener_active(void *ctx) {
    psftp_notify_ctx_t *p = (psftp_notify_ctx_t *)ctx;
    return *p->listening;
}

static void psftp_notify_start_listener(void *ctx) {
    psftp_notify_ctx_t *p = (psftp_notify_ctx_t *)ctx;
    gatt_client_listen_for_characteristic_value_updates(
        p->notification,
        handle_gatt_event,
        conn_handle,
        p->chr);
    *p->listening = true;
}

static void psftp_notify_stop_listener(void *ctx) {
    psftp_notify_ctx_t *p = (psftp_notify_ctx_t *)ctx;
    gatt_client_stop_listening_for_characteristic_value_updates(p->notification);
    *p->listening = false;
}

static int psftp_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
    psftp_notify_ctx_t *p = (psftp_notify_ctx_t *)ctx;

    cfg_pending = true;
    cfg_done = false;
    cfg_att_status = ATT_ERROR_SUCCESS;
    cfg_expected_ctx_conn = p->chr != NULL ? p->chr->value_handle : 0;

    int ready = gatt_client_is_ready(conn_handle);
    printf("[psftp-probe] ccc write req handle=0x%04x cfg=0x%04x ready=%d ctx=(svc=0x%04x,cid=0x%04x)\n",
           p->chr != NULL ? p->chr->value_handle : 0,
           ccc_cfg,
           ready,
           (unsigned)PSFTP_QC_CTX_SERVICE_CCC,
           (unsigned)cfg_expected_ctx_conn);

    int err = gatt_client_write_client_characteristic_configuration_with_context(
        handle_gatt_event,
        conn_handle,
        p->chr,
        ccc_cfg,
        PSFTP_QC_CTX_SERVICE_CCC,
        cfg_expected_ctx_conn);
    if (err) {
        cfg_pending = false;
        printf("[psftp-probe] ccc write submit failed err=%d\n", err);
    }
    return err;
}

static bool psftp_notify_wait_complete(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    (void)ctx;
    bool done = wait_flag_until_true(&cfg_done, timeout_ms);
    if (out_att_status) {
        *out_att_status = cfg_att_status;
    }
    return done;
}

#define PSFTP_OP_OK POLAR_SDK_PSFTP_OP_OK
#define PSFTP_OP_NOT_CONNECTED POLAR_SDK_PSFTP_OP_NOT_CONNECTED
#define PSFTP_OP_TIMEOUT POLAR_SDK_PSFTP_OP_TIMEOUT
#define PSFTP_OP_TRANSPORT POLAR_SDK_PSFTP_OP_TRANSPORT
#define PSFTP_OP_MISSING_CHAR POLAR_SDK_PSFTP_OP_MISSING_CHAR
#define PSFTP_OP_NO_NOTIFY_PROP POLAR_SDK_PSFTP_OP_NO_NOTIFY_PROP

static int psftp_set_notify(gatt_client_characteristic_t *chr, gatt_client_notification_t *notification, bool *listening, bool enable) {
    psftp_notify_ctx_t ctx = {
        .chr = chr,
        .notification = notification,
        .listening = listening,
    };

    polar_sdk_gatt_notify_ops_t ops = {
        .ctx = &ctx,
        .is_connected_ready = psftp_notify_is_connected_ready,
        .listener_active = psftp_notify_listener_active,
        .start_listener = psftp_notify_start_listener,
        .stop_listener = psftp_notify_stop_listener,
        .write_ccc = psftp_notify_write_ccc,
        .wait_complete = psftp_notify_wait_complete,
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
        .timeout_ms = PSFTP_GATT_TIMEOUT_MS,
        .cfg_pending = &cfg_pending,
        .cfg_done = &cfg_done,
    };

    polar_sdk_gatt_notify_runtime_result_t r = polar_sdk_gatt_notify_runtime_set(&args);
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_OK) {
        return PSFTP_OP_OK;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED) {
        return PSFTP_OP_NOT_CONNECTED;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT) {
        return PSFTP_OP_TIMEOUT;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_MISSING_CHAR) {
        return PSFTP_OP_MISSING_CHAR;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_NO_NOTIFY_PROP) {
        return PSFTP_OP_NO_NOTIFY_PROP;
    }
    if (r == POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED) {
        return cfg_att_status;
    }
    return PSFTP_OP_TRANSPORT;
}

static int psftp_write_frame(const uint8_t *frame, uint16_t frame_len) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return PSFTP_OP_NOT_CONNECTED;
    }

    uint16_t value_handle = psftp_tx_value_handle();
    if (value_handle == 0) {
        return PSFTP_OP_MISSING_CHAR;
    }

    write_pending = false;
    write_done = false;
    write_att_status = ATT_ERROR_SUCCESS;

    write_packet_index += 1;
    uint32_t packet_index = write_packet_index;
    bool use_response = psftp_write_should_use_response(packet_index);

    printf("[psftp-probe] tx frame via=%s write_mode=%s op=%s pkt=%" PRIu32 " period_n=%d handle=0x%04x len=%u hex=",
           psftp_tx_mode_name(),
           psftp_write_mode_name(),
           psftp_write_opcode_name(use_response),
           packet_index,
           H10_PSFTP_WRITE_PERIOD_N,
           value_handle,
           frame_len);
    print_hex_prefix(frame, frame_len, H10_PSFTP_TX_HEX_DUMP_BYTES);
    printf("\n");

    if (!use_response) {
        int err = gatt_client_write_value_of_characteristic_without_response(
            conn_handle,
            value_handle,
            frame_len,
            (uint8_t *)frame);
        if (err != ERROR_CODE_SUCCESS) {
            runtime_link.last_hci_status = (uint8_t)err;
            printf("[psftp-probe] write command failed handle=0x%04x len=%u err=%d\n",
                   value_handle,
                   frame_len,
                   err);
            return PSFTP_OP_TRANSPORT;
        }
        return PSFTP_OP_OK;
    }

    int ready_before = gatt_client_is_ready(conn_handle);
    uint32_t qc_before = qc_event_total;
    uint32_t qc_write_before = qc_match_write_total;

    write_ctx_seq += 1;
    if (write_ctx_seq == 0) {
        write_ctx_seq = 1;
    }
    write_expected_ctx_conn = write_ctx_seq;

    printf("[psftp-probe] write request submit ready=%d cfg_pending=%d ctx=(svc=0x%04x,cid=0x%04x) qc_total=%" PRIu32 "\n",
           ready_before,
           cfg_pending ? 1 : 0,
           (unsigned)PSFTP_QC_CTX_SERVICE_WRITE,
           (unsigned)write_expected_ctx_conn,
           qc_before);

    write_pending = true;
    int err = gatt_client_write_value_of_characteristic_with_context(
        handle_gatt_event,
        conn_handle,
        value_handle,
        frame_len,
        (uint8_t *)frame,
        PSFTP_QC_CTX_SERVICE_WRITE,
        write_expected_ctx_conn);
    if (err != ERROR_CODE_SUCCESS) {
        write_pending = false;
        runtime_link.last_hci_status = (uint8_t)err;
        printf("[psftp-probe] write request failed handle=0x%04x len=%u err=%d\n",
               value_handle,
               frame_len,
               err);
        return PSFTP_OP_TRANSPORT;
    }

    if (!wait_flag_until_true(&write_done, PSFTP_GATT_TIMEOUT_MS)) {
        int ready_now = gatt_client_is_ready(conn_handle);
        uint32_t qc_delta = qc_event_total - qc_before;
        uint32_t qc_write_delta = qc_match_write_total - qc_write_before;

        write_pending = false;
        write_done = false;
        printf("[psftp-probe] write wait timeout ready_before=%d ready_now=%d qc_delta=%" PRIu32 " qc_write_match_delta=%" PRIu32 " last_qc(svc=0x%04x,cid=0x%04x,att=0x%02x)\n",
               ready_before,
               ready_now,
               qc_delta,
               qc_write_delta,
               (unsigned)qc_last_service_id,
               (unsigned)qc_last_connection_id,
               qc_last_att_status);
        return PSFTP_OP_TIMEOUT;
    }

    if (write_att_status != ATT_ERROR_SUCCESS) {
        return write_att_status;
    }

    return PSFTP_OP_OK;
}

static bool psftp_security_is_connected_cb(void *ctx) {
    (void)ctx;
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool psftp_security_is_connected_const_cb(const void *ctx) {
    return psftp_security_is_connected_cb((void *)ctx);
}

static bool psftp_security_is_secure_cb(void *ctx) {
    (void)ctx;
    return psftp_security_ready();
}

static bool psftp_security_is_secure_const_cb(const void *ctx) {
    return psftp_security_is_secure_cb((void *)ctx);
}

static void psftp_security_request_pairing_cb(void *ctx) {
    (void)ctx;
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    sm_request_pairing(conn_handle);
}

static void psftp_security_request_pairing_const_cb(const void *ctx) {
    psftp_security_request_pairing_cb((void *)ctx);
}

static void psftp_security_sleep_ms_cb(void *ctx, uint32_t ms) {
    (void)ctx;
    probe_sleep_ms(ms);
}

static void psftp_security_sleep_ms_const_cb(const void *ctx, uint32_t ms) {
    psftp_security_sleep_ms_cb((void *)ctx, ms);
}

static bool psftp_request_pairing_and_wait(void) {
    if (psftp_security_ready()) {
        return true;
    }

    polar_sdk_btstack_sm_apply_default_auth_policy();

    polar_sdk_security_policy_t policy = {
        .rounds = PSFTP_SECURITY_ROUNDS,
        .wait_ms_per_round = PSFTP_SECURITY_WAIT_MS,
        .request_gap_ms = 120,
        .poll_ms = 20,
    };
    polar_sdk_security_ops_t ops = {
        .ctx = NULL,
        .is_connected = psftp_security_is_connected_const_cb,
        .is_secure = psftp_security_is_secure_const_cb,
        .request_pairing = psftp_security_request_pairing_const_cb,
        .sleep_ms = psftp_security_sleep_ms_const_cb,
    };

    polar_sdk_security_result_t r = polar_sdk_security_request_with_retry(&policy, &ops);
    return r == POLAR_SDK_SECURITY_RESULT_OK;
}

static bool psftp_prepare_is_connected_ready_cb(void *ctx) {
    (void)ctx;
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool psftp_prepare_has_required_characteristics_cb(void *ctx) {
    (void)ctx;
    return psftp_mtu_found && psftp_h2d_found &&
        psftp_mtu_char.value_handle != 0 && psftp_h2d_char.value_handle != 0;
}

static polar_sdk_security_result_t psftp_prepare_ensure_security_cb(void *ctx) {
    (void)ctx;
    if (psftp_request_pairing_and_wait()) {
        return POLAR_SDK_SECURITY_RESULT_OK;
    }
    return connected ? POLAR_SDK_SECURITY_RESULT_TIMEOUT : POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
}

static bool psftp_prepare_mtu_notify_enabled_cb(void *ctx) {
    (void)ctx;
    return psftp_mtu_enabled;
}

static int psftp_prepare_enable_mtu_notify_cb(void *ctx) {
    (void)ctx;
    int status = psftp_set_notify(&psftp_mtu_char, &psftp_mtu_listener, &psftp_mtu_listener_registered, true);
    if (status == PSFTP_OP_OK) {
        psftp_mtu_enabled = true;
    }
    return status;
}

static bool psftp_prepare_d2h_notify_supported_cb(void *ctx) {
    (void)ctx;
    return psftp_d2h_found && psftp_d2h_char.value_handle != 0;
}

static bool psftp_prepare_d2h_notify_enabled_cb(void *ctx) {
    (void)ctx;
    return psftp_d2h_enabled;
}

static int psftp_prepare_enable_d2h_notify_cb(void *ctx) {
    (void)ctx;
    int status = psftp_set_notify(&psftp_d2h_char, &psftp_d2h_listener, &psftp_d2h_listener_registered, true);
    if (status == PSFTP_OP_OK) {
        psftp_d2h_enabled = true;
    }
    return status;
}

static int psftp_prepare_channels(void) {
    polar_sdk_psftp_prepare_policy_t policy = {
        .retry_security_on_att = true,
        .strict_d2h_enable = false,
    };
    polar_sdk_psftp_prepare_ops_t ops = {
        .ctx = NULL,
        .is_connected_ready = psftp_prepare_is_connected_ready_cb,
        .has_required_characteristics = psftp_prepare_has_required_characteristics_cb,
        .security_ready = psftp_security_is_secure_cb,
        .ensure_security = psftp_prepare_ensure_security_cb,
        .mtu_notify_enabled = psftp_prepare_mtu_notify_enabled_cb,
        .enable_mtu_notify = psftp_prepare_enable_mtu_notify_cb,
        .d2h_notify_supported = psftp_prepare_d2h_notify_supported_cb,
        .d2h_notify_enabled = psftp_prepare_d2h_notify_enabled_cb,
        .enable_d2h_notify = psftp_prepare_enable_d2h_notify_cb,
    };
    return polar_sdk_psftp_prepare_channels(&policy, &ops);
}

static int psftp_get_prepare_channels_cb(void *ctx) {
    (void)ctx;
    return psftp_prepare_channels();
}

static uint16_t psftp_get_frame_capacity_cb(void *ctx) {
    (void)ctx;
    return att_mtu <= 3 ? 20 : (uint16_t)(att_mtu - 3);
}

static int psftp_get_write_frame_cb(void *ctx, const uint8_t *frame, uint16_t frame_len) {
    (void)ctx;
    return psftp_write_frame(frame, frame_len);
}

static void psftp_get_on_tx_frame_ok_cb(void *ctx) {
    (void)ctx;
    psftp_tx_frames_total += 1;
}

static void psftp_get_begin_response_cb(void *ctx, uint8_t *response, size_t response_capacity) {
    (void)ctx;

    printf("[psftp-probe] pre-tx channels mtu(handle=0x%04x listen=%d enabled=%d) d2h(handle=0x%04x listen=%d enabled=%d) att_mtu=%u ready=%d pre_tx_delay_ms=%u\n",
           psftp_mtu_char.value_handle,
           psftp_mtu_listener_registered ? 1 : 0,
           psftp_mtu_enabled ? 1 : 0,
           psftp_d2h_char.value_handle,
           psftp_d2h_listener_registered ? 1 : 0,
           psftp_d2h_enabled ? 1 : 0,
           att_mtu,
           gatt_client_is_ready(conn_handle),
           (unsigned)H10_PSFTP_PRE_TX_DELAY_MS);

    if (H10_PSFTP_PRE_TX_DELAY_MS > 0) {
        probe_sleep_ms((uint32_t)H10_PSFTP_PRE_TX_DELAY_MS);
    }

    psftp_response_waiting = true;
    psftp_response_done = false;
    psftp_response_result = POLAR_SDK_PSFTP_RX_MORE;
    polar_sdk_psftp_rx_reset(&psftp_rx_state, response, response_capacity);
    write_packet_index = 0;
}

static bool psftp_get_wait_response_cb(void *ctx, uint32_t timeout_ms) {
    (void)ctx;
    bool done = wait_flag_until_true(&psftp_response_done, timeout_ms);
    if (!done) {
        psftp_response_waiting = false;
        psftp_response_done = false;
    }
    return done;
}

static polar_sdk_psftp_rx_result_t psftp_get_response_result_cb(void *ctx) {
    (void)ctx;
    return psftp_response_result;
}

static const polar_sdk_psftp_rx_state_t *psftp_get_rx_state_cb(void *ctx) {
    (void)ctx;
    return &psftp_rx_state;
}

static bool psftp_get_is_remote_success_cb(void *ctx, uint16_t error_code) {
    (void)ctx;
    (void)error_code;
    return false;
}

static void psftp_init_runtime_ops(polar_sdk_psftp_get_ops_t *ops) {
    if (ops == NULL) {
        return;
    }

    *ops = (polar_sdk_psftp_get_ops_t){
        .ctx = NULL,
        .prepare_channels = psftp_get_prepare_channels_cb,
        .frame_capacity = psftp_get_frame_capacity_cb,
        .write_frame = psftp_get_write_frame_cb,
        .on_tx_frame_ok = psftp_get_on_tx_frame_ok_cb,
        .begin_response = psftp_get_begin_response_cb,
        .wait_response = psftp_get_wait_response_cb,
        .response_result = psftp_get_response_result_cb,
        .rx_state = psftp_get_rx_state_cb,
        .is_remote_success = psftp_get_is_remote_success_cb,
    };
}

static bool psftp_finish_transaction(
    const char *label,
    polar_sdk_psftp_trans_result_t r,
    int prep_status,
    int write_status,
    uint16_t error_code,
    uint16_t *out_error_code,
    psftp_diag_snapshot_t *diag_before) {
    psftp_last_diag.prep_code = prep_status;
    psftp_last_diag.write_code = write_status;

    if (r == POLAR_SDK_PSFTP_TRANS_OK) {
        psftp_response_waiting = false;
        psftp_diag_finalize(diag_before, true);
        return true;
    }

    psftp_response_waiting = false;

    if (r == POLAR_SDK_PSFTP_TRANS_RESPONSE_TIMEOUT) {
        psftp_last_diag.response_timeout = true;
        printf("[psftp-probe] response timeout %s\n", label);
        printf("[psftp-probe] timeout channels mtu(listen=%d enabled=%d) d2h(listen=%d enabled=%d) ready=%d qc(total=%" PRIu32 ",cfg=%" PRIu32 ",write=%" PRIu32 ")\n",
               psftp_mtu_listener_registered ? 1 : 0,
               psftp_mtu_enabled ? 1 : 0,
               psftp_d2h_listener_registered ? 1 : 0,
               psftp_d2h_enabled ? 1 : 0,
               gatt_client_is_ready(conn_handle),
               qc_event_total,
               qc_match_cfg_total,
               qc_match_write_total);
    } else if (r == POLAR_SDK_PSFTP_TRANS_REMOTE_ERROR) {
        if (out_error_code != NULL) {
            *out_error_code = error_code;
        }
        psftp_last_diag.remote_error = true;
        psftp_last_diag.remote_error_code = error_code;
        printf("[psftp-probe] remote error %s code=%u\n", label, (unsigned)error_code);
    } else {
        printf("[psftp-probe] %s failed trans=%d prep=%d write=%d resp=%d\n",
               label,
               (int)r,
               prep_status,
               write_status,
               (int)psftp_response_result);
    }

    psftp_diag_finalize(diag_before, false);
    return false;
}

static bool psftp_execute_get(
    const char *path,
    uint8_t *out_buf,
    size_t out_capacity,
    size_t *out_len,
    uint16_t *out_error_code) {
    if (out_len) {
        *out_len = 0;
    }
    if (out_error_code) {
        *out_error_code = 0;
    }

    psftp_diag_reset();
    psftp_diag_snapshot_t diag_before = psftp_diag_snapshot_take();

    printf("[psftp-probe] GET path=%s\n", path);

    polar_sdk_psftp_get_ops_t ops;
    psftp_init_runtime_ops(&ops);

    int prep_status = PSFTP_OP_OK;
    int write_status = PSFTP_OP_OK;
    uint16_t error_code = 0;

    polar_sdk_psftp_trans_result_t r = polar_sdk_psftp_execute_get_operation(
        &ops,
        path,
        strlen(path),
        out_buf,
        out_capacity,
        PSFTP_RESPONSE_TIMEOUT_MS,
        out_len,
        &error_code,
        &prep_status,
        &write_status);

    char label[320];
    snprintf(label, sizeof(label), "GET path=%s", path);
    return psftp_finish_transaction(label, r, prep_status, write_status, error_code, out_error_code, &diag_before);
}

static bool psftp_execute_query(
    uint16_t query_id,
    const uint8_t *query_payload,
    size_t query_payload_len,
    uint8_t *out_buf,
    size_t out_capacity,
    size_t *out_len,
    uint16_t *out_error_code) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out_error_code != NULL) {
        *out_error_code = 0;
    }

    psftp_diag_reset();
    psftp_diag_snapshot_t diag_before = psftp_diag_snapshot_take();

    printf("[psftp-probe] QUERY id=%u payload_len=%u\n",
           (unsigned)query_id,
           (unsigned)query_payload_len);

    polar_sdk_psftp_get_ops_t ops;
    psftp_init_runtime_ops(&ops);

    int prep_status = PSFTP_OP_OK;
    int write_status = PSFTP_OP_OK;
    uint16_t error_code = 0;

    polar_sdk_psftp_trans_result_t r = polar_sdk_psftp_execute_query_operation(
        &ops,
        query_id,
        query_payload,
        query_payload_len,
        out_buf,
        out_capacity,
        PSFTP_RESPONSE_TIMEOUT_MS,
        out_len,
        &error_code,
        &prep_status,
        &write_status);

    char label[96];
    snprintf(label, sizeof(label), "QUERY id=%u", (unsigned)query_id);
    return psftp_finish_transaction(label, r, prep_status, write_status, error_code, out_error_code, &diag_before);
}

static const char *h10_recording_sample_type_name(polar_sdk_h10_recording_sample_type_t sample_type) {
    switch (sample_type) {
        case POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE:
            return "hr";
        case POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL:
            return "rr";
        default:
            return "unknown";
    }
}

static polar_sdk_h10_recording_sample_type_t configured_h10_recording_sample_type(void) {
    return H10_RECORDING_SAMPLE_TYPE == 1
        ? POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL
        : POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE;
}

static polar_sdk_h10_recording_interval_t configured_h10_recording_interval(void) {
    return H10_RECORDING_INTERVAL_S == 5
        ? POLAR_SDK_H10_RECORDING_INTERVAL_5S
        : POLAR_SDK_H10_RECORDING_INTERVAL_1S;
}

static bool psftp_query_h10_recording_status(bool *out_recording_on, char *out_recording_id, size_t out_recording_id_capacity) {
    size_t response_len = 0;
    uint16_t error_code = 0;
    if (out_recording_on != NULL) {
        *out_recording_on = false;
    }
    if (out_recording_id != NULL && out_recording_id_capacity > 0u) {
        out_recording_id[0] = '\0';
    }

    if (!psftp_execute_query(
            POLAR_SDK_PSFTP_QUERY_REQUEST_RECORDING_STATUS,
            NULL,
            0,
            psftp_dir_buf,
            sizeof(psftp_dir_buf),
            &response_len,
            &error_code)) {
        return false;
    }

    size_t recording_id_len = 0;
    if (!polar_sdk_psftp_decode_h10_recording_status_result(
            psftp_dir_buf,
            response_len,
            out_recording_on,
            out_recording_id,
            out_recording_id_capacity,
            &recording_id_len)) {
        printf("[psftp-probe] decode H10 recording status failed len=%u\n", (unsigned)response_len);
        return false;
    }

    printf("[psftp-probe] h10 recording status on=%d id=%s id_len=%u\n",
           (out_recording_on != NULL && *out_recording_on) ? 1 : 0,
           (out_recording_id != NULL && out_recording_id_capacity > 0u) ? out_recording_id : "",
           (unsigned)recording_id_len);
    return true;
}

static bool psftp_start_h10_recording(const char *recording_id) {
    if (recording_id == NULL) {
        return false;
    }

    uint8_t params[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES];
    size_t params_len = 0;
    polar_sdk_h10_recording_sample_type_t sample_type = configured_h10_recording_sample_type();
    polar_sdk_h10_recording_interval_t interval = configured_h10_recording_interval();
    size_t recording_id_len = strlen(recording_id);

    if (!polar_sdk_psftp_encode_h10_start_recording_params(
            recording_id,
            recording_id_len,
            sample_type,
            interval,
            params,
            sizeof(params),
            &params_len)) {
        printf("[psftp-probe] encode H10 start recording failed id=%s sample=%s interval=%u\n",
               recording_id,
               h10_recording_sample_type_name(sample_type),
               (unsigned)interval);
        return false;
    }

    printf("[psftp-probe] h10 start recording id=%s sample=%s interval=%u\n",
           recording_id,
           h10_recording_sample_type_name(sample_type),
           (unsigned)interval);

    size_t response_len = 0;
    uint16_t error_code = 0;
    if (!psftp_execute_query(
            POLAR_SDK_PSFTP_QUERY_REQUEST_START_RECORDING,
            params,
            params_len,
            psftp_dir_buf,
            sizeof(psftp_dir_buf),
            &response_len,
            &error_code)) {
        return false;
    }

    printf("[psftp-probe] h10 start recording response_len=%u\n", (unsigned)response_len);
    return true;
}

static bool psftp_stop_h10_recording(void) {
    size_t response_len = 0;
    uint16_t error_code = 0;
    printf("[psftp-probe] h10 stop recording\n");
    if (!psftp_execute_query(
            POLAR_SDK_PSFTP_QUERY_REQUEST_STOP_RECORDING,
            NULL,
            0,
            psftp_dir_buf,
            sizeof(psftp_dir_buf),
            &response_len,
            &error_code)) {
        return false;
    }

    printf("[psftp-probe] h10 stop recording response_len=%u\n", (unsigned)response_len);
    return true;
}

static bool psftp_run_h10_recording_probe(void) {
    bool recording_on = false;
    char recording_id[POLAR_SDK_H10_RECORDING_ID_MAX_BYTES + 1u];

    if (H10_RECORDING_QUERY_STATUS) {
        if (!psftp_query_h10_recording_status(&recording_on, recording_id, sizeof(recording_id))) {
            return false;
        }
    }

    if (!H10_RECORDING_START_STOP) {
        return true;
    }

    if (recording_on) {
        printf("[psftp-probe] h10 start/stop smoke skipped: recording already active id=%s\n", recording_id);
        return true;
    }

    if (!psftp_start_h10_recording(H10_RECORDING_EXERCISE_ID)) {
        return false;
    }
    if (!psftp_query_h10_recording_status(&recording_on, recording_id, sizeof(recording_id))) {
        return false;
    }
    if (!recording_on) {
        printf("[psftp-probe] h10 start recording did not become active\n");
        return false;
    }
    if (strcmp(recording_id, H10_RECORDING_EXERCISE_ID) != 0) {
        printf("[psftp-probe] h10 active recording id mismatch expected=%s actual=%s\n",
               H10_RECORDING_EXERCISE_ID,
               recording_id);
        return false;
    }

    if (!psftp_stop_h10_recording()) {
        return false;
    }
    if (!psftp_query_h10_recording_status(&recording_on, recording_id, sizeof(recording_id))) {
        return false;
    }
    if (recording_on) {
        printf("[psftp-probe] h10 stop recording did not clear active state id=%s\n", recording_id);
        return false;
    }
    return true;
}

static size_t build_test_get_path(char *dst, size_t cap) {
    if (dst == NULL || cap < 2) {
        return 0;
    }

    size_t want = (size_t)H10_PSFTP_TEST_PATH_LEN;
    if (want < 1) {
        want = 1;
    }
    if (want > 250) {
        want = 250;
    }
    if (want >= cap) {
        want = cap - 1;
    }

    dst[0] = '/';
    for (size_t i = 1; i < want; ++i) {
        dst[i] = (char)('A' + ((i - 1) % 26));
    }
    dst[want] = '\0';
    return want;
}

static bool run_psftp_round(void) {
    size_t dir_len = 0;
    uint16_t error_code = 0;

    if (!psftp_run_h10_recording_probe()) {
        return false;
    }

    char test_path[260];
    size_t test_path_len = build_test_get_path(test_path, sizeof(test_path));
    if (test_path_len == 0) {
        printf("[psftp-probe] test path build failed\n");
        return false;
    }

    if (test_path_len > 1) {
        printf("[psftp-probe] synthetic GET path_len=%u\n", (unsigned)test_path_len);
        if (!psftp_execute_get(test_path, psftp_dir_buf, sizeof(psftp_dir_buf), &dir_len, &error_code)) {
            return false;
        }
        printf("[psftp-probe] synthetic GET ok bytes=%u\n", (unsigned)dir_len);
        return true;
    }

    if (!psftp_execute_get("/", psftp_dir_buf, sizeof(psftp_dir_buf), &dir_len, &error_code)) {
        return false;
    }

    polar_sdk_psftp_dir_entry_t entries[PSFTP_MAX_DIR_ENTRIES];
    size_t entry_count = 0;
    polar_sdk_psftp_dir_decode_result_t decode = polar_sdk_psftp_decode_directory(
        psftp_dir_buf,
        dir_len,
        entries,
        PSFTP_MAX_DIR_ENTRIES,
        &entry_count);

    if (decode != POLAR_SDK_PSFTP_DIR_DECODE_OK) {
        printf("[psftp-probe] directory decode failed result=%d len=%u\n", (int)decode, (unsigned)dir_len);
        return false;
    }

    printf("[psftp-probe] list_dir('/') entries=%u\n", (unsigned)entry_count);
    for (size_t i = 0; i < entry_count; ++i) {
        printf("  - %s (%" PRIu64 ")\n", entries[i].name, entries[i].size);
    }

    const polar_sdk_psftp_dir_entry_t *target = NULL;
    for (size_t i = 0; i < entry_count; ++i) {
        size_t nlen = strlen(entries[i].name);
        if (nlen == 0 || entries[i].name[nlen - 1] == '/') {
            continue;
        }
        if (entries[i].size <= H10_DOWNLOAD_MAX_BYTES) {
            target = &entries[i];
            break;
        }
    }

    if (!target) {
        printf("[psftp-probe] no candidate file <= %u bytes\n", (unsigned)H10_DOWNLOAD_MAX_BYTES);
        return true;
    }

    char path[160];
    int path_n = snprintf(path, sizeof(path), "/%s", target->name);
    if (path_n <= 0 || (size_t)path_n >= sizeof(path)) {
        printf("[psftp-probe] candidate path overflow\n");
        return false;
    }

    size_t dl_len = 0;
    error_code = 0;
    if (!psftp_execute_get(path, psftp_dl_buf, sizeof(psftp_dl_buf), &dl_len, &error_code)) {
        return false;
    }

    printf("[psftp-probe] download('%s') bytes=%u head=", path, (unsigned)dl_len);
    size_t head = dl_len < 16 ? dl_len : 16;
    for (size_t i = 0; i < head; ++i) {
        printf("%02x", psftp_dl_buf[i]);
    }
    printf("\n");

    return true;
}

static void run_psftp_round_tick(void) {
    if (probe_done) {
        return;
    }
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    if (app_state != APP_READY) {
        return;
    }

    app_state = APP_RUNNING_ROUND;
    rounds_total += 1;

    printf("[psftp-probe] ===== round %" PRIu32 " =====\n", rounds_total);
    printf("[psftp-probe] pre-round enc_key=%u pair_total=%" PRIu32 " pair_status=0x%02x state=%s\n",
           gap_encryption_key_size(conn_handle),
           sm_pairing_complete_total,
           sm_last_pairing_status,
           state_name(app_state));

    bool ok = run_psftp_round();
    if (ok) {
        rounds_ok += 1;
    } else {
        rounds_fail += 1;
    }

    printf("[psftp-probe] round result ok=%d totals ok=%" PRIu32 " fail=%" PRIu32 "\n",
           ok ? 1 : 0,
           rounds_ok,
           rounds_fail);

    if (!ok) {
        printf("[psftp-probe] verdict=%s prep=%d write=%d timeout=%d remote=%d remote_code=%u resp=%d tx_delta=%" PRIu32 " rx_delta=%" PRIu32 " raw(mtu=%" PRIu32 ",d2h=%" PRIu32 ",other=%" PRIu32 ") routed(mtu=%" PRIu32 ",d2h=%" PRIu32 ")\n",
               psftp_verdict_name(psftp_last_verdict),
               psftp_last_diag.prep_code,
               psftp_last_diag.write_code,
               psftp_last_diag.response_timeout ? 1 : 0,
               psftp_last_diag.remote_error ? 1 : 0,
               (unsigned)psftp_last_diag.remote_error_code,
               (int)psftp_last_diag.response_result,
               psftp_last_diag.tx_delta,
               psftp_last_diag.rx_delta,
               psftp_last_diag.raw_mtu_delta,
               psftp_last_diag.raw_d2h_delta,
               psftp_last_diag.raw_other_delta,
               psftp_last_diag.routed_mtu_delta,
               psftp_last_diag.routed_d2h_delta);
    }

    if (H10_MAX_ROUNDS > 0 && rounds_total >= (uint32_t)H10_MAX_ROUNDS) {
        probe_done = true;
        printf("[psftp-probe] completed max rounds=%u\n", (unsigned)H10_MAX_ROUNDS);
        if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
            user_disconnect_requested = true;
            uint8_t err = gap_disconnect(conn_handle);
            printf("[psftp-probe] final clean disconnect err=%u\n", (unsigned)err);
            if (err != ERROR_CODE_SUCCESS) {
                user_disconnect_requested = false;
                on_disconnect_cleanup();
            }
        } else {
            app_state = APP_DONE;
        }
        return;
    }

    if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
        user_disconnect_requested = true;
        uint8_t err = gap_disconnect(conn_handle);
        printf("[psftp-probe] disconnect after round err=%u\n", (unsigned)err);
        if (err != ERROR_CODE_SUCCESS) {
            user_disconnect_requested = false;
            on_disconnect_cleanup();
            schedule_reconnect(RECONNECT_DELAY_MS);
        }
    }
}

static void handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    polar_sdk_btstack_value_event_t raw_value;
    if (polar_sdk_btstack_decode_value_event(packet_type, packet, &raw_value) &&
        connected && conn_handle != HCI_CON_HANDLE_INVALID && raw_value.handle == conn_handle) {
        raw_value_events_total += 1;

        const char *bucket = "other";
        if (raw_value.value_handle == psftp_mtu_char.value_handle) {
            raw_value_mtu_total += 1;
            bucket = "mtu";
        } else if (raw_value.value_handle == psftp_d2h_char.value_handle) {
            raw_value_d2h_total += 1;
            bucket = "d2h";
        } else {
            raw_value_other_total += 1;
        }

#if H10_PSFTP_RAW_VALUE_TAP
        printf("[psftp-probe] raw value bucket=%s handle=0x%04x len=%u notif=%d hex=",
               bucket,
               raw_value.value_handle,
               raw_value.value_len,
               raw_value.notification ? 1 : 0);
        print_hex_prefix(raw_value.value, raw_value.value_len, H10_PSFTP_RX_HEX_DUMP_BYTES);
        printf("\n");
#endif
    }

    polar_sdk_btstack_gatt_route_state_t route_state = {
        .conn_handle = conn_handle,
        .connected = connected,
        .hr_value_handle = 0,
        .hr_enabled = false,
        .pmd_cp_value_handle = 0,
        .pmd_cp_listening = false,
        .pmd_data_value_handle = 0,
        .ecg_enabled = false,
        .psftp_mtu_value_handle = psftp_mtu_char.value_handle,
        .psftp_mtu_listening = psftp_mtu_listener_registered,
        .psftp_d2h_value_handle = psftp_d2h_char.value_handle,
        .psftp_d2h_listening = psftp_d2h_listener_registered,
    };

    polar_sdk_btstack_gatt_route_result_t route;
    bool routed = polar_sdk_btstack_route_gatt_event(packet_type, packet, &route_state, &route);
    if (routed) {
        if (route.kind == POLAR_SDK_GATT_ROUTE_MTU_EVENT) {
            if (route.mtu.handle == conn_handle) {
                att_mtu = route.mtu.mtu;
                printf("[psftp-probe] ATT MTU event mtu=%u\n", att_mtu);
            }
            return;
        }

        if (route.kind == POLAR_SDK_GATT_ROUTE_PSFTP_MTU_VALUE) {
            routed_psftp_mtu_total += 1;
            psftp_rx_frames_total += 1;
            if (psftp_response_waiting) {
                psftp_response_result = polar_sdk_psftp_rx_feed_frame(
                    &psftp_rx_state,
                    route.value.value,
                    route.value.value_len);
                printf("[psftp-probe] routed MTU value len=%u result=%d hex=",
                       route.value.value_len,
                       (int)psftp_response_result);
                print_hex_prefix(route.value.value, route.value.value_len, H10_PSFTP_RX_HEX_DUMP_BYTES);
                printf("\n");
                if (psftp_response_result == POLAR_SDK_PSFTP_RX_COMPLETE ||
                    psftp_response_result == POLAR_SDK_PSFTP_RX_ERROR_FRAME ||
                    psftp_response_result == POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR ||
                    psftp_response_result == POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR ||
                    psftp_response_result == POLAR_SDK_PSFTP_RX_OVERFLOW) {
                    psftp_response_done = true;
                    psftp_response_waiting = false;
                }
            }
            return;
        }

        if (route.kind == POLAR_SDK_GATT_ROUTE_PSFTP_D2H_VALUE) {
            routed_psftp_d2h_total += 1;
            printf("[psftp-probe] routed D2H value len=%u hex=", route.value.value_len);
            print_hex_prefix(route.value.value, route.value.value_len, H10_PSFTP_RX_HEX_DUMP_BYTES);
            printf("\n");
            return;
        }

        if (route.kind == POLAR_SDK_GATT_ROUTE_QUERY_COMPLETE) {
            routed_query_complete_total += 1;

            uint16_t qc_service_id = gatt_event_query_complete_get_service_id(packet);
            uint16_t qc_connection_id = gatt_event_query_complete_get_connection_id(packet);
            qc_event_total += 1;
            qc_last_service_id = qc_service_id;
            qc_last_connection_id = qc_connection_id;
            qc_last_att_status = route.query_complete_att_status;

            bool qc_match_cfg = cfg_pending &&
                                qc_service_id == PSFTP_QC_CTX_SERVICE_CCC &&
                                qc_connection_id == cfg_expected_ctx_conn;
            bool qc_match_write = write_pending &&
                                  qc_service_id == PSFTP_QC_CTX_SERVICE_WRITE &&
                                  qc_connection_id == write_expected_ctx_conn;
            if (qc_match_cfg) {
                qc_match_cfg_total += 1;
            }
            if (qc_match_write) {
                qc_match_write_total += 1;
            }

            printf("[psftp-probe] gatt query complete att=0x%02x svc=0x%04x cid=0x%04x match(cfg=%d,write=%d) exp(cfg=0x%04x,write=0x%04x) cfg_pending=%d write_pending=%d state=%s\n",
                   route.query_complete_att_status,
                   qc_service_id,
                   qc_connection_id,
                   qc_match_cfg ? 1 : 0,
                   qc_match_write ? 1 : 0,
                   cfg_expected_ctx_conn,
                   write_expected_ctx_conn,
                   cfg_pending ? 1 : 0,
                   write_pending ? 1 : 0,
                   state_name(app_state));

            if (cfg_pending) {
                if (!qc_match_cfg) {
                    printf("[psftp-probe] qc mismatch while cfg pending (svc=0x%04x cid=0x%04x)\n",
                           qc_service_id,
                           qc_connection_id);
                }
                cfg_pending = false;
                cfg_done = true;
                cfg_att_status = route.query_complete_att_status;
            }
            if (write_pending) {
                if (!qc_match_write) {
                    printf("[psftp-probe] qc mismatch while write pending (svc=0x%04x cid=0x%04x)\n",
                           qc_service_id,
                           qc_connection_id);
                }
                write_pending = false;
                write_done = true;
                write_att_status = route.query_complete_att_status;
            }
            // continue into discovery-state handling below.
        }
    }

    switch (app_state) {
        case APP_W4_PSFTP_SERVICE: {
            gatt_client_service_t svc;
            if (polar_sdk_btstack_decode_service_query_result(packet_type, packet, &svc)) {
                polar_sdk_disc_service_kind_t kind = polar_sdk_btstack_classify_service(
                    svc.uuid16,
                    svc.uuid128,
                    ORG_BLUETOOTH_SERVICE_HEART_RATE,
                    PSFTP_SERVICE_UUID16,
                    UUID_PMD_SERVICE_BE);
                if (kind == POLAR_SDK_DISC_SERVICE_PSFTP) {
                    psftp_service = svc;
                    psftp_service_found = true;
                    printf("[psftp-probe] PSFTP service found start=0x%04x end=0x%04x\n",
                           psftp_service.start_group_handle,
                           psftp_service.end_group_handle);
                }
            }

            uint8_t att_status = ATT_ERROR_SUCCESS;
            if (polar_sdk_btstack_decode_query_complete_att_status(packet_type, packet, &att_status)) {
                printf("[psftp-probe] PSFTP service discovery complete att=0x%02x found=%d\n",
                       att_status,
                       psftp_service_found ? 1 : 0);
                if (att_status != ATT_ERROR_SUCCESS || !psftp_service_found) {
                    if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
                        user_disconnect_requested = true;
                        (void)gap_disconnect(conn_handle);
                    }
                    app_state = APP_CONNECTED;
                    return;
                }

                app_state = APP_W4_PSFTP_CHARS;
                int err = gatt_client_discover_characteristics_for_service(
                    handle_gatt_event,
                    conn_handle,
                    &psftp_service);
                printf("[psftp-probe] discover PSFTP chars err=%d\n", err);
                if (err) {
                    app_state = APP_CONNECTED;
                }
            }
            break;
        }

        case APP_W4_PSFTP_CHARS: {
            gatt_client_characteristic_t chr;
            if (polar_sdk_btstack_decode_characteristic_query_result(packet_type, packet, &chr)) {
                polar_sdk_disc_char_kind_t kind = polar_sdk_btstack_classify_char(
                    POLAR_SDK_DISC_STAGE_PSFTP_CHARS,
                    chr.uuid16,
                    chr.uuid128,
                    ORG_BLUETOOTH_CHARACTERISTIC_HEART_RATE_MEASUREMENT,
                    UUID_PMD_SERVICE_BE,
                    UUID_PMD_SERVICE_BE,
                    UUID_PSFTP_MTU_BE,
                    UUID_PSFTP_D2H_BE,
                    UUID_PSFTP_H2D_BE);

                if (kind == POLAR_SDK_DISC_CHAR_PSFTP_MTU) {
                    psftp_mtu_char = chr;
                    psftp_mtu_found = true;
                    printf("[psftp-probe] PSFTP MTU char value=0x%04x props=0x%02x\n", chr.value_handle, chr.properties);
                } else if (kind == POLAR_SDK_DISC_CHAR_PSFTP_D2H) {
                    psftp_d2h_char = chr;
                    psftp_d2h_found = true;
                    printf("[psftp-probe] PSFTP D2H char value=0x%04x props=0x%02x\n", chr.value_handle, chr.properties);
                } else if (kind == POLAR_SDK_DISC_CHAR_PSFTP_H2D) {
                    psftp_h2d_char = chr;
                    psftp_h2d_found = true;
                    printf("[psftp-probe] PSFTP H2D char value=0x%04x props=0x%02x\n", chr.value_handle, chr.properties);
                }
            }

            uint8_t att_status = ATT_ERROR_SUCCESS;
            if (polar_sdk_btstack_decode_query_complete_att_status(packet_type, packet, &att_status)) {
                printf("[psftp-probe] PSFTP char discovery complete att=0x%02x mtu=%d d2h=%d h2d=%d\n",
                       att_status,
                       psftp_mtu_found ? 1 : 0,
                       psftp_d2h_found ? 1 : 0,
                       psftp_h2d_found ? 1 : 0);

                if (att_status == ATT_ERROR_SUCCESS && psftp_mtu_found && psftp_d2h_found && psftp_h2d_found) {
                    app_state = APP_READY;
                } else {
                    if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
                        user_disconnect_requested = true;
                        (void)gap_disconnect(conn_handle);
                    }
                    app_state = APP_CONNECTED;
                }
            }
            break;
        }

        default:
            break;
    }
}

static void heartbeat_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = btstack_run_loop_get_time_ms();
    uint8_t enc_key = (connected && conn_handle != HCI_CON_HANDLE_INVALID) ? gap_encryption_key_size(conn_handle) : 0;

    printf("[psftp-probe] hb t=%" PRIu32 " state=%s conn=%d handle=0x%04x enc=%u tx_mode=%s write_mode=%s jw=%" PRIu32 " pair=%" PRIu32 " pair_status=0x%02x rounds=%" PRIu32 "/ok=%" PRIu32 "/fail=%" PRIu32 " tx=%" PRIu32 " rx=%" PRIu32 " raw=%" PRIu32 "(mtu=%" PRIu32 ",d2h=%" PRIu32 ",other=%" PRIu32 ") routed(mtu=%" PRIu32 ",d2h=%" PRIu32 ",qc=%" PRIu32 ") qc(total=%" PRIu32 ",cfg=%" PRIu32 ",write=%" PRIu32 ",last=0x%04x/0x%04x/0x%02x)\n",
           now,
           state_name(app_state),
           connected ? 1 : 0,
           conn_handle,
           enc_key,
           psftp_tx_mode_name(),
           psftp_write_mode_name(),
           sm_just_works_total,
           sm_pairing_complete_total,
           sm_last_pairing_status,
           rounds_total,
           rounds_ok,
           rounds_fail,
           psftp_tx_frames_total,
           psftp_rx_frames_total,
           raw_value_events_total,
           raw_value_mtu_total,
           raw_value_d2h_total,
           raw_value_other_total,
           routed_psftp_mtu_total,
           routed_psftp_d2h_total,
           routed_query_complete_total,
           qc_event_total,
           qc_match_cfg_total,
           qc_match_write_total,
           qc_last_service_id,
           qc_last_connection_id,
           qc_last_att_status);

    if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(conn_handle);
    }

    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    printf("\n[psftp-probe] ===== RP2 BTstack PSFTP probe =====\n");
    printf("[psftp-probe] target=%s max_rounds=%u download_max=%u test_path_len=%u\n",
           H10_TARGET_ADDR,
           (unsigned)H10_MAX_ROUNDS,
           (unsigned)H10_DOWNLOAD_MAX_BYTES,
           (unsigned)H10_PSFTP_TEST_PATH_LEN);
    printf("[psftp-probe] tx_mode=%s write_mode=%s write_period_n=%d pre_tx_delay_ms=%u auth=bond+sc raw_tap=%d clear_bond_on_boot=%d tx_hex_max=%u rx_hex_max=%u\n",
           psftp_tx_mode_name(),
           psftp_write_mode_name(),
           H10_PSFTP_WRITE_PERIOD_N,
           (unsigned)H10_PSFTP_PRE_TX_DELAY_MS,
           H10_PSFTP_RAW_VALUE_TAP,
           H10_CLEAR_BOND_ON_BOOT,
           (unsigned)H10_PSFTP_TX_HEX_DUMP_BYTES,
           (unsigned)H10_PSFTP_RX_HEX_DUMP_BYTES);
    printf("[psftp-probe] h10_recording status_query=%d start_stop=%d exercise_id=%s sample=%s interval_s=%u\n",
           H10_RECORDING_QUERY_STATUS,
           H10_RECORDING_START_STOP,
           H10_RECORDING_EXERCISE_ID,
           h10_recording_sample_type_name(configured_h10_recording_sample_type()),
           (unsigned)configured_h10_recording_interval());

    target_addr_valid = sscanf_bd_addr(H10_TARGET_ADDR, target_addr) != 0;
    if (!target_addr_valid) {
        printf("[psftp-probe] WARNING invalid H10_TARGET_ADDR, fallback to name matching\n");
    }

    if (cyw43_arch_init()) {
        printf("[psftp-probe] cyw43_arch_init failed\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    l2cap_init();
    sm_init();
    gatt_client_init();
    polar_sdk_btstack_sm_apply_default_auth_policy();

    polar_sdk_runtime_link_init(&runtime_link, HCI_CON_HANDLE_INVALID);
    psftp_diag_reset();

    hci_event_cb.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_cb);

    sm_event_cb.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_cb);

    btstack_run_loop_set_timer_handler(&heartbeat_timer, heartbeat_timer_handler);
    btstack_run_loop_set_timer(&heartbeat_timer, 1000);
    btstack_run_loop_add_timer(&heartbeat_timer);

    hci_power_control(HCI_POWER_ON);

    while (true) {
        run_psftp_round_tick();
        cyw43_arch_poll();
        sleep_ms(1);
    }
}
