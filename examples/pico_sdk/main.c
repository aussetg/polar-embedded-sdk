#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack.h"

#include "polar_ble_driver_connect.h"
#include "polar_ble_driver_runtime.h"
#include "polar_ble_driver_runtime_context.h"
#include "polar_ble_driver_btstack_link.h"
#include "polar_ble_driver_btstack_gatt.h"
#include "polar_ble_driver_btstack_helpers.h"
#include "polar_ble_driver_btstack_scan.h"
#include "polar_ble_driver_btstack_adv_runtime.h"
#include "polar_ble_driver_btstack_sm.h"
#include "polar_ble_driver_sm_control.h"
#include "polar_ble_driver_btstack_dispatch.h"
#include "polar_ble_driver_discovery_apply.h"
#include "polar_ble_driver_gatt_notify_runtime.h"
#include "polar_ble_driver_pmd.h"
#include "polar_ble_driver_pmd_control.h"

#ifndef H10_TARGET_ADDR
#define H10_TARGET_ADDR "24:AC:AC:05:A3:10"
#endif

#ifndef H10_FORCE_PAIRING
#define H10_FORCE_PAIRING 0
#endif

#ifndef H10_ENABLE_HR
#define H10_ENABLE_HR 1
#endif

#ifndef H10_ENABLE_ECG_POLICY
#define H10_ENABLE_ECG_POLICY 1
#endif

#ifndef H10_POST_CONNECT_UPDATE
#define H10_POST_CONNECT_UPDATE 1
#endif

#ifndef H10_POST_CONNECT_CONN_INTERVAL_MIN
#define H10_POST_CONNECT_CONN_INTERVAL_MIN 24
#endif

#ifndef H10_POST_CONNECT_CONN_INTERVAL_MAX
#define H10_POST_CONNECT_CONN_INTERVAL_MAX 24
#endif

#ifndef H10_POST_CONNECT_CONN_LATENCY
#define H10_POST_CONNECT_CONN_LATENCY 0
#endif

#ifndef H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS
#define H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS 72
#endif

#define H10_PMD_MIN_MTU 70
#define H10_PMD_SECURITY_ROUNDS 3
#define H10_PMD_SECURITY_WAIT_MS 3500
#define H10_PMD_CCC_ATTEMPTS 4
#define H10_PMD_ECG_SAMPLE_RATE 130
#define H10_PMD_ECG_RESOLUTION 14
#define H10_PMD_IMU_SAMPLE_RATE 50
#define H10_PMD_IMU_RESOLUTION 16
#define H10_PMD_IMU_RANGE 8

#define H10_CONNECT_TIMEOUT_WINDOW_MS 60000
#define H10_CONNECT_ATTEMPT_SLICE_MS 3500

static const uint8_t H10_UUID_PMD_SERVICE_BE[16] = {
    0xFB, 0x00, 0x5C, 0x80, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t H10_UUID_PMD_CP_BE[16] = {
    0xFB, 0x00, 0x5C, 0x81, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t H10_UUID_PMD_DATA_BE[16] = {
    0xFB, 0x00, 0x5C, 0x82, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

typedef enum {
    APP_OFF = 0,
    APP_SCANNING,
    APP_CONNECTING,
    APP_CONNECTED,
    APP_W4_HR_SERVICE,
    APP_W4_HR_CHAR,
    APP_W4_HR_CCC,
    APP_W4_PMD_SERVICE,
    APP_W4_PMD_CHARS,
    APP_W4_PMD_START,
    APP_STREAMING,
} app_state_t;

static app_state_t app_state = APP_OFF;

static btstack_packet_callback_registration_t hci_event_cb;
static btstack_packet_callback_registration_t sm_event_cb;

static btstack_timer_source_t heartbeat_timer;
static btstack_timer_source_t reconnect_timer;

static gatt_client_notification_t hr_listener;
static bool hr_listener_registered = false;

static hci_con_handle_t conn_handle = HCI_CON_HANDLE_INVALID;
static bool connected = false;
static bool target_addr_valid = false;
static bd_addr_t target_addr;

static bd_addr_t peer_addr;
static bd_addr_type_t peer_addr_type = BD_ADDR_TYPE_UNKNOWN;

static uint32_t connect_time_ms = 0;
static uint32_t last_hr_ms = 0;

static gatt_client_service_t hr_service;
static gatt_client_characteristic_t hr_char;
static bool hr_service_found = false;
static bool hr_char_found = false;

static gatt_client_service_t pmd_service;
static gatt_client_characteristic_t pmd_cp_char;
static gatt_client_characteristic_t pmd_data_char;
static bool pmd_service_found = false;
static bool pmd_cp_found = false;
static bool pmd_data_found = false;

static gatt_client_notification_t pmd_cp_listener;
static gatt_client_notification_t pmd_data_listener;
static bool pmd_cp_listener_registered = false;
static bool pmd_data_listener_registered = false;

static bool pmd_cfg_pending = false;
static bool pmd_cfg_done = false;
static uint8_t pmd_cfg_att_status = ATT_ERROR_SUCCESS;

static bool pmd_write_pending = false;
static bool pmd_write_done = false;
static uint8_t pmd_write_att_status = ATT_ERROR_SUCCESS;

static bool pmd_cp_response_waiting = false;
static bool pmd_cp_response_done = false;
static uint8_t pmd_cp_response_expected_opcode = 0;
static uint8_t pmd_cp_response_expected_type = 0;
static uint8_t pmd_cp_response_status = 0xff;

static bool mtu_exchange_pending = false;
static bool mtu_exchange_done = false;
static uint16_t att_mtu = ATT_DEFAULT_MTU;

static bool pmd_policy_started = false;
static bool pmd_policy_done = false;
static uint32_t ecg_start_attempts_total = 0;
static uint32_t ecg_start_success_total = 0;
static uint32_t imu_start_attempts_total = 0;
static uint32_t imu_start_success_total = 0;
static uint32_t pmd_data_notifications_total = 0;
static uint32_t pmd_data_ecg_notifications_total = 0;
static uint32_t pmd_data_imu_notifications_total = 0;
static uint32_t pmd_data_unknown_notifications_total = 0;

static polar_ble_driver_runtime_link_t runtime_link;

static polar_ble_driver_connect_policy_t reconnect_policy = {
    .timeout_ms = H10_CONNECT_TIMEOUT_WINDOW_MS,
    .attempt_slice_ms = H10_CONNECT_ATTEMPT_SLICE_MS,
};
static polar_ble_driver_connect_state_t reconnect_state;

static const char *state_name(app_state_t s) {
    switch (s) {
        case APP_OFF: return "OFF";
        case APP_SCANNING: return "SCANNING";
        case APP_CONNECTING: return "CONNECTING";
        case APP_CONNECTED: return "CONNECTED";
        case APP_W4_HR_SERVICE: return "W4_HR_SERVICE";
        case APP_W4_HR_CHAR: return "W4_HR_CHAR";
        case APP_W4_HR_CCC: return "W4_HR_CCC";
        case APP_W4_PMD_SERVICE: return "W4_PMD_SERVICE";
        case APP_W4_PMD_CHARS: return "W4_PMD_CHARS";
        case APP_W4_PMD_START: return "W4_PMD_START";
        case APP_STREAMING: return "STREAMING";
        default: return "?";
    }
}

static const char *pmd_start_result_name(polar_ble_driver_pmd_start_result_t r) {
    switch (r) {
        case POLAR_BLE_DRIVER_PMD_START_RESULT_OK: return "OK";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED: return "NOT_CONNECTED";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_SECURITY_TIMEOUT: return "SECURITY_TIMEOUT";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_REJECTED: return "CCC_REJECTED";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_TIMEOUT: return "CCC_TIMEOUT";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_MTU_FAILED: return "MTU_FAILED";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_START_TIMEOUT: return "START_TIMEOUT";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_START_REJECTED: return "START_REJECTED";
        case POLAR_BLE_DRIVER_PMD_START_RESULT_TRANSPORT_ERROR: return "TRANSPORT_ERROR";
        default: return "?";
    }
}

static void schedule_reconnect(uint32_t delay_ms);

static uint32_t next_reconnect_delay_ms(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t delay_ms = polar_ble_driver_connect_next_backoff_ms(&reconnect_policy, &reconnect_state, now);
    if (delay_ms > 0) {
        return delay_ms;
    }

    // Restart timeout window and retry from first backoff slot.
    polar_ble_driver_connect_init(&reconnect_state, now);
    delay_ms = polar_ble_driver_connect_next_backoff_ms(&reconnect_policy, &reconnect_state, now);
    return delay_ms > 0 ? delay_ms : 1000;
}

static void request_post_connect_update(hci_con_handle_t handle) {
#if H10_POST_CONNECT_UPDATE
    int err = gap_update_connection_parameters(
        handle,
        H10_POST_CONNECT_CONN_INTERVAL_MIN,
        H10_POST_CONNECT_CONN_INTERVAL_MAX,
        H10_POST_CONNECT_CONN_LATENCY,
        H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS
    );

    printf("[h10probe] post-connect update handle=0x%04x min=%u max=%u lat=%u sup=%u err=%d\n",
           handle,
           H10_POST_CONNECT_CONN_INTERVAL_MIN,
           H10_POST_CONNECT_CONN_INTERVAL_MAX,
           H10_POST_CONNECT_CONN_LATENCY,
           H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS,
           err);
#else
    (void)handle;
#endif
}

#if H10_ENABLE_HR && !H10_ENABLE_ECG_POLICY
static void maybe_start_hr_pipeline(void);
#endif
#if H10_ENABLE_ECG_POLICY
static void maybe_start_pmd_policy_pipeline(void);
static void run_pmd_policy_tick(void);
#endif
static void handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void probe_sleep_ms(uint32_t ms) {
    while (ms--) {
        cyw43_arch_poll();
        sleep_ms(1);
    }
}

static bool wait_flag_until_true(const volatile bool *flag, uint32_t timeout_ms) {
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

typedef struct {
    gatt_client_characteristic_t *chr;
    gatt_client_notification_t *notification;
    bool *listening;
} probe_pmd_notify_ctx_t;

static bool probe_pmd_notify_is_connected_ready(void *ctx) {
    UNUSED(ctx);
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool probe_pmd_notify_listener_active(void *ctx) {
    probe_pmd_notify_ctx_t *p = (probe_pmd_notify_ctx_t *)ctx;
    return *p->listening;
}

static void probe_pmd_notify_start_listener(void *ctx) {
    probe_pmd_notify_ctx_t *p = (probe_pmd_notify_ctx_t *)ctx;
    gatt_client_listen_for_characteristic_value_updates(
        p->notification,
        handle_gatt_event,
        conn_handle,
        p->chr);
    *p->listening = true;
}

static void probe_pmd_notify_stop_listener(void *ctx) {
    probe_pmd_notify_ctx_t *p = (probe_pmd_notify_ctx_t *)ctx;
    gatt_client_stop_listening_for_characteristic_value_updates(p->notification);
    *p->listening = false;
}

static int probe_pmd_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
    probe_pmd_notify_ctx_t *p = (probe_pmd_notify_ctx_t *)ctx;
    pmd_cfg_pending = true;
    pmd_cfg_done = false;
    pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    int err = gatt_client_write_client_characteristic_configuration(
        handle_gatt_event,
        conn_handle,
        p->chr,
        ccc_cfg);
    if (err) {
        pmd_cfg_pending = false;
    }
    return err;
}

static bool probe_pmd_notify_wait_complete(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status) {
    UNUSED(ctx);
    bool done = wait_flag_until_true(&pmd_cfg_done, timeout_ms);
    if (out_att_status != NULL) {
        *out_att_status = pmd_cfg_att_status;
    }
    return done;
}

static int probe_pmd_set_notify_for_char_result(
    gatt_client_characteristic_t *chr,
    gatt_client_notification_t *notification,
    bool *listening,
    bool enable) {
    probe_pmd_notify_ctx_t ctx = {
        .chr = chr,
        .notification = notification,
        .listening = listening,
    };
    polar_ble_driver_gatt_notify_ops_t ops = {
        .ctx = &ctx,
        .is_connected_ready = probe_pmd_notify_is_connected_ready,
        .listener_active = probe_pmd_notify_listener_active,
        .start_listener = probe_pmd_notify_start_listener,
        .stop_listener = probe_pmd_notify_stop_listener,
        .write_ccc = probe_pmd_notify_write_ccc,
        .wait_complete = probe_pmd_notify_wait_complete,
    };
    polar_ble_driver_gatt_notify_runtime_args_t args = {
        .ops = &ops,
        .has_value_handle = chr != NULL && chr->value_handle != 0,
        .enable = enable,
        .properties = chr != NULL ? chr->properties : 0,
        .prop_notify = ATT_PROPERTY_NOTIFY,
        .prop_indicate = ATT_PROPERTY_INDICATE,
        .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
        .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
        .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
        .att_success = ATT_ERROR_SUCCESS,
        .timeout_ms = 2000,
        .cfg_pending = &pmd_cfg_pending,
        .cfg_done = &pmd_cfg_done,
    };

    polar_ble_driver_gatt_notify_runtime_result_t r = polar_ble_driver_gatt_notify_runtime_set(&args);
    return polar_ble_driver_pmd_map_notify_result(
        r,
        pmd_cfg_att_status,
        POLAR_BLE_DRIVER_PMD_OP_OK,
        POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED,
        POLAR_BLE_DRIVER_PMD_OP_TIMEOUT,
        POLAR_BLE_DRIVER_PMD_OP_TRANSPORT);
}

static int pmd_enable_notifications_once(void) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID || !pmd_cp_found || !pmd_data_found) {
        return POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED;
    }

    int status = probe_pmd_set_notify_for_char_result(
        &pmd_cp_char,
        &pmd_cp_listener,
        &pmd_cp_listener_registered,
        true);
    if (status != POLAR_BLE_DRIVER_PMD_OP_OK) {
        return status;
    }

    status = probe_pmd_set_notify_for_char_result(
        &pmd_data_char,
        &pmd_data_listener,
        &pmd_data_listener_registered,
        true);
    if (status != POLAR_BLE_DRIVER_PMD_OP_OK) {
        return status;
    }

    return POLAR_BLE_DRIVER_PMD_OP_OK;
}

static bool pmd_is_connected(void *ctx) {
    UNUSED(ctx);
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

static uint8_t pmd_encryption_key_size(void *ctx) {
    UNUSED(ctx);
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return 0;
    }
    return gap_encryption_key_size(conn_handle);
}

static void pmd_request_pairing(void *ctx) {
    UNUSED(ctx);
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
    sm_request_pairing(conn_handle);
}

static void pmd_sleep_ms_cb(void *ctx, uint32_t ms) {
    UNUSED(ctx);
    probe_sleep_ms(ms);
}

static int pmd_enable_notifications_cb(void *ctx) {
    UNUSED(ctx);
    return pmd_enable_notifications_once();
}

static int pmd_ensure_minimum_mtu_cb(void *ctx, uint16_t minimum_mtu) {
    UNUSED(ctx);
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED;
    }

    uint16_t current_mtu = ATT_DEFAULT_MTU;
    if (gatt_client_get_mtu(conn_handle, &current_mtu) == ERROR_CODE_SUCCESS) {
        att_mtu = current_mtu;
    }
    if (att_mtu >= minimum_mtu) {
        printf("[h10probe] PMD MTU already sufficient: current=%u required=%u\n", att_mtu, minimum_mtu);
        return POLAR_BLE_DRIVER_PMD_OP_OK;
    }

    printf("[h10probe] PMD MTU upgrade requested: current=%u required=%u\n", att_mtu, minimum_mtu);
    mtu_exchange_pending = true;
    mtu_exchange_done = false;
    gatt_client_send_mtu_negotiation(handle_gatt_event, conn_handle);
    if (!wait_flag_until_true(&mtu_exchange_done, 2000)) {
        mtu_exchange_pending = false;
        printf("[h10probe] PMD MTU upgrade timeout\n");
        return POLAR_BLE_DRIVER_PMD_OP_TIMEOUT;
    }
    mtu_exchange_pending = false;

    printf("[h10probe] PMD MTU after upgrade=%u\n", att_mtu);
    return att_mtu >= minimum_mtu ? POLAR_BLE_DRIVER_PMD_OP_OK : POLAR_BLE_DRIVER_PMD_OP_TIMEOUT;
}

static int pmd_start_measurement_and_wait_response_cb(
    void *ctx,
    const uint8_t *start_cmd,
    size_t start_cmd_len,
    uint8_t *out_status) {
    UNUSED(ctx);
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED;
    }
    if (start_cmd == NULL || start_cmd_len < 2 || start_cmd_len > UINT16_MAX) {
        return POLAR_BLE_DRIVER_PMD_OP_TRANSPORT;
    }

    uint8_t measurement_type = start_cmd[1];

    pmd_cp_response_waiting = true;
    pmd_cp_response_done = false;
    pmd_cp_response_expected_opcode = POLAR_BLE_DRIVER_PMD_OPCODE_START_MEASUREMENT;
    pmd_cp_response_expected_type = measurement_type;
    pmd_cp_response_status = 0xff;

    pmd_write_pending = true;
    pmd_write_done = false;
    pmd_write_att_status = ATT_ERROR_SUCCESS;
    printf("[h10probe] PMD START write handle=0x%04x type=0x%02x len=%u\n",
           pmd_cp_char.value_handle,
           measurement_type,
           (unsigned)start_cmd_len);
    int err = gatt_client_write_value_of_characteristic(
        handle_gatt_event,
        conn_handle,
        pmd_cp_char.value_handle,
        (uint16_t)start_cmd_len,
        (uint8_t *)start_cmd);
    if (err) {
        pmd_write_pending = false;
        printf("[h10probe] PMD START write transport err=%d\n", err);
        return POLAR_BLE_DRIVER_PMD_OP_TRANSPORT;
    }

    if (!wait_flag_until_true(&pmd_write_done, 2000)) {
        pmd_write_pending = false;
        printf("[h10probe] PMD START write timeout\n");
        return POLAR_BLE_DRIVER_PMD_OP_TIMEOUT;
    }
    if (pmd_write_att_status != ATT_ERROR_SUCCESS) {
        printf("[h10probe] PMD START write ATT reject=0x%02x\n", pmd_write_att_status);
        return pmd_write_att_status;
    }

    if (!wait_flag_until_true(&pmd_cp_response_done, 2000)) {
        pmd_cp_response_waiting = false;
        printf("[h10probe] PMD START response timeout type=0x%02x\n", measurement_type);
        return POLAR_BLE_DRIVER_PMD_OP_TIMEOUT;
    }

    if (out_status != NULL) {
        *out_status = pmd_cp_response_status;
    }
    printf("[h10probe] PMD START response type=0x%02x status=0x%02x\n", measurement_type, pmd_cp_response_status);

    return POLAR_BLE_DRIVER_PMD_OP_OK;
}

static void handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t event = hci_event_packet_get_type(packet);

    polar_ble_driver_btstack_mtu_event_t mtu_event;
    if (polar_ble_driver_btstack_decode_mtu_event(packet_type, packet, &mtu_event)) {
        att_mtu = mtu_event.mtu;
        mtu_exchange_done = true;
        mtu_exchange_pending = false;
        printf("[h10probe] MTU=%u\n", att_mtu);
        return;
    }

    uint8_t query_complete_att_status = ATT_ERROR_SUCCESS;
    bool have_query_complete = polar_ble_driver_btstack_decode_query_complete_att_status(
        packet_type,
        packet,
        &query_complete_att_status);

    if (have_query_complete && pmd_cfg_pending) {
        pmd_cfg_att_status = query_complete_att_status;
        pmd_cfg_pending = false;
        pmd_cfg_done = true;
        printf("[h10probe] PMD CCC query complete att=0x%02x\n", pmd_cfg_att_status);
        return;
    }

    if (have_query_complete && pmd_write_pending) {
        pmd_write_att_status = query_complete_att_status;
        pmd_write_pending = false;
        pmd_write_done = true;
        printf("[h10probe] PMD START write query complete att=0x%02x\n", pmd_write_att_status);
        return;
    }

    switch (app_state) {
        case APP_W4_HR_SERVICE:
            if (polar_ble_driver_btstack_decode_service_query_result(packet_type, packet, &hr_service)) {
                hr_service_found = true;
                printf("[h10probe] HR service found: start=0x%04x end=0x%04x\n",
                       hr_service.start_group_handle, hr_service.end_group_handle);
            } else if (have_query_complete) {
                uint8_t att_status = query_complete_att_status;
                printf("[h10probe] HR service query complete att=0x%02x found=%d\n", att_status, hr_service_found);
                if (att_status != ATT_ERROR_SUCCESS || !hr_service_found) {
                    app_state = APP_CONNECTED;
                    return;
                }
                app_state = APP_W4_HR_CHAR;
                gatt_client_discover_characteristics_for_service_by_uuid16(
                    handle_gatt_event,
                    conn_handle,
                    &hr_service,
                    ORG_BLUETOOTH_CHARACTERISTIC_HEART_RATE_MEASUREMENT);
            }
            break;

        case APP_W4_HR_CHAR:
            if (polar_ble_driver_btstack_decode_characteristic_query_result(packet_type, packet, &hr_char)) {
                hr_char_found = true;
                printf("[h10probe] HR char found: start=0x%04x value=0x%04x end=0x%04x\n",
                       hr_char.start_handle, hr_char.value_handle, hr_char.end_handle);
            } else if (have_query_complete) {
                uint8_t att_status = query_complete_att_status;
                printf("[h10probe] HR char query complete att=0x%02x found=%d\n", att_status, hr_char_found);
                if (att_status != ATT_ERROR_SUCCESS || !hr_char_found) {
                    app_state = APP_CONNECTED;
                    return;
                }

                if (!hr_listener_registered) {
                    hr_listener_registered = true;
                    gatt_client_listen_for_characteristic_value_updates(
                        &hr_listener,
                        handle_gatt_event,
                        conn_handle,
                        &hr_char);
                }

                app_state = APP_W4_HR_CCC;
                int err = gatt_client_write_client_characteristic_configuration(
                    handle_gatt_event,
                    conn_handle,
                    &hr_char,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                printf("[h10probe] write HR CCC err=%d\n", err);
                if (err) {
                    app_state = APP_CONNECTED;
                }
            }
            break;

        case APP_W4_HR_CCC:
            if (have_query_complete) {
                uint8_t att_status = query_complete_att_status;
                printf("[h10probe] HR CCC complete att=0x%02x\n", att_status);
                if (att_status == ATT_ERROR_SUCCESS) {
                    app_state = APP_STREAMING;
                } else {
                    if (polar_ble_driver_pmd_att_status_requires_security(att_status)) {
                        printf("[h10probe] HR CCC requires security, requesting pairing\n");
                        sm_request_pairing(conn_handle);
                    }
                    app_state = APP_CONNECTED;
                }
            }
            break;

        case APP_W4_PMD_SERVICE: {
            gatt_client_service_t svc;
            if (polar_ble_driver_btstack_decode_service_query_result(packet_type, packet, &svc)) {
                polar_ble_driver_disc_service_kind_t service_kind = polar_ble_driver_btstack_classify_service(
                    svc.uuid16,
                    svc.uuid128,
                    ORG_BLUETOOTH_SERVICE_HEART_RATE,
                    0,
                    H10_UUID_PMD_SERVICE_BE);
                printf("[h10probe] service result start=0x%04x end=0x%04x uuid16=0x%04x kind=%d uuid128=%02x%02x%02x%02x..%02x%02x\n",
                       svc.start_group_handle,
                       svc.end_group_handle,
                       svc.uuid16,
                       service_kind,
                       svc.uuid128[0],
                       svc.uuid128[1],
                       svc.uuid128[2],
                       svc.uuid128[3],
                       svc.uuid128[14],
                       svc.uuid128[15]);
                if (service_kind == POLAR_BLE_DRIVER_DISC_SERVICE_PMD) {
                    pmd_service = svc;
                    polar_ble_driver_discovery_apply_service_kind(
                        POLAR_BLE_DRIVER_DISC_SERVICE_PMD,
                        NULL,
                        &pmd_service_found,
                        NULL);
                    printf("[h10probe] PMD service found start=0x%04x end=0x%04x\n",
                           pmd_service.start_group_handle, pmd_service.end_group_handle);
                }
            } else if (have_query_complete) {
                uint8_t att_status = query_complete_att_status;
                printf("[h10probe] PMD service query complete att=0x%02x found=%d\n", att_status, pmd_service_found);
                if (att_status != ATT_ERROR_SUCCESS || !pmd_service_found) {
                    app_state = APP_CONNECTED;
                    return;
                }
                app_state = APP_W4_PMD_CHARS;
                int err = gatt_client_discover_characteristics_for_service(
                    handle_gatt_event,
                    conn_handle,
                    &pmd_service);
                printf("[h10probe] PMD char discovery err=%d\n", err);
                if (err) {
                    app_state = APP_CONNECTED;
                }
            }
            break;
        }

        case APP_W4_PMD_CHARS: {
            gatt_client_characteristic_t chr;
            if (polar_ble_driver_btstack_decode_characteristic_query_result(packet_type, packet, &chr)) {
                polar_ble_driver_disc_char_kind_t ck = polar_ble_driver_btstack_classify_char(
                    POLAR_BLE_DRIVER_DISC_STAGE_PMD_CHARS,
                    chr.uuid16,
                    chr.uuid128,
                    ORG_BLUETOOTH_CHARACTERISTIC_HEART_RATE_MEASUREMENT,
                    H10_UUID_PMD_CP_BE,
                    H10_UUID_PMD_DATA_BE,
                    0,
                    0,
                    0);
                if (ck == POLAR_BLE_DRIVER_DISC_CHAR_PMD_CP) {
                    pmd_cp_char = chr;
                } else if (ck == POLAR_BLE_DRIVER_DISC_CHAR_PMD_DATA) {
                    pmd_data_char = chr;
                }
                polar_ble_driver_discovery_apply_char_kind(
                    ck,
                    chr.value_handle,
                    NULL,
                    NULL,
                    &pmd_cp_found,
                    NULL,
                    &pmd_data_found,
                    NULL,
                    NULL,
                    NULL,
                    NULL);
                if (ck == POLAR_BLE_DRIVER_DISC_CHAR_PMD_CP) {
                    printf("[h10probe] PMD CP char value=0x%04x props=0x%02x\n", chr.value_handle, chr.properties);
                } else if (ck == POLAR_BLE_DRIVER_DISC_CHAR_PMD_DATA) {
                    printf("[h10probe] PMD DATA char value=0x%04x props=0x%02x\n", chr.value_handle, chr.properties);
                } else {
                    printf("[h10probe] PMD char other start=0x%04x value=0x%04x end=0x%04x uuid16=0x%04x uuid128=%02x%02x%02x%02x..%02x%02x\n",
                           chr.start_handle,
                           chr.value_handle,
                           chr.end_handle,
                           chr.uuid16,
                           chr.uuid128[0],
                           chr.uuid128[1],
                           chr.uuid128[2],
                           chr.uuid128[3],
                           chr.uuid128[14],
                           chr.uuid128[15]);
                }
            } else if (have_query_complete) {
                uint8_t att_status = query_complete_att_status;
                printf("[h10probe] PMD chars query complete att=0x%02x cp=%d data=%d\n",
                       att_status, pmd_cp_found, pmd_data_found);
                app_state = (att_status == ATT_ERROR_SUCCESS && pmd_cp_found && pmd_data_found)
                    ? APP_W4_PMD_START
                    : APP_CONNECTED;
            }
            break;
        }

        default:
            break;
    }

    polar_ble_driver_btstack_value_event_t value_event;
    if (polar_ble_driver_btstack_decode_value_event(packet_type, packet, &value_event) &&
        conn_handle != HCI_CON_HANDLE_INVALID) {

        if (value_event.value_handle == hr_char.value_handle) {
            if (value_event.value_len < 2) {
                return;
            }
            uint8_t flags = value_event.value[0];
            uint16_t hr = (flags & 0x01) ? little_endian_read_16(value_event.value, 1) : value_event.value[1];
            last_hr_ms = btstack_run_loop_get_time_ms();
            printf("[h10probe] HR notify=%u len=%u flags=0x%02x t=%" PRIu32 "\n", hr, value_event.value_len, flags, last_hr_ms);
            return;
        }

        if (value_event.value_handle == pmd_cp_char.value_handle) {
            polar_ble_driver_pmd_cp_response_t response;
            if (!polar_ble_driver_pmd_parse_cp_response(value_event.value, value_event.value_len, &response)) {
                return;
            }
            printf("[h10probe] PMD CP rsp via=%s opcode=0x%02x type=0x%02x status=0x%02x\n",
                   value_event.notification ? "notify" : "indicate",
                   response.opcode,
                   response.measurement_type,
                   response.status);
            if (pmd_cp_response_waiting &&
                response.opcode == pmd_cp_response_expected_opcode &&
                response.measurement_type == pmd_cp_response_expected_type) {
                pmd_cp_response_status = response.status;
                pmd_cp_response_waiting = false;
                pmd_cp_response_done = true;
            }
            return;
        }

        if (value_event.value_handle == pmd_data_char.value_handle) {
            pmd_data_notifications_total += 1;

            if (value_event.value_len > 0) {
                uint8_t measurement_type = value_event.value[0];
                if (measurement_type == POLAR_BLE_DRIVER_PMD_MEASUREMENT_ECG) {
                    pmd_data_ecg_notifications_total += 1;
                } else if (measurement_type == POLAR_BLE_DRIVER_PMD_MEASUREMENT_ACC) {
                    pmd_data_imu_notifications_total += 1;
                } else {
                    pmd_data_unknown_notifications_total += 1;
                }
            } else {
                pmd_data_unknown_notifications_total += 1;
            }
            return;
        }
    }
}

#if H10_ENABLE_HR && !H10_ENABLE_ECG_POLICY
static void maybe_start_hr_pipeline(void) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    hr_service_found = false;
    hr_char_found = false;
    memset(&hr_service, 0, sizeof(hr_service));
    memset(&hr_char, 0, sizeof(hr_char));

    app_state = APP_W4_HR_SERVICE;
    int err = gatt_client_discover_primary_services_by_uuid16(
        handle_gatt_event,
        conn_handle,
        ORG_BLUETOOTH_SERVICE_HEART_RATE);
    printf("[h10probe] discover HR service err=%d\n", err);
    if (err) {
        app_state = APP_CONNECTED;
    }
}
#endif

#if H10_ENABLE_ECG_POLICY
static void maybe_start_pmd_policy_pipeline(void) {
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    memset(&pmd_service, 0, sizeof(pmd_service));
    memset(&pmd_cp_char, 0, sizeof(pmd_cp_char));
    memset(&pmd_data_char, 0, sizeof(pmd_data_char));
    pmd_service_found = false;
    pmd_cp_found = false;
    pmd_data_found = false;

    pmd_cfg_pending = false;
    pmd_cfg_done = false;
    pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    pmd_write_pending = false;
    pmd_write_done = false;
    pmd_write_att_status = ATT_ERROR_SUCCESS;

    pmd_cp_response_waiting = false;
    pmd_cp_response_done = false;
    pmd_cp_response_status = 0xff;

    att_mtu = ATT_DEFAULT_MTU;
    mtu_exchange_pending = false;
    mtu_exchange_done = false;

    pmd_policy_started = false;
    pmd_policy_done = false;
    pmd_data_notifications_total = 0;
    pmd_data_ecg_notifications_total = 0;
    pmd_data_imu_notifications_total = 0;
    pmd_data_unknown_notifications_total = 0;

    app_state = APP_W4_PMD_SERVICE;
    int err = gatt_client_discover_primary_services(
        handle_gatt_event,
        conn_handle);
    printf("[h10probe] discover all primary services err=%d\n", err);
    if (err) {
        app_state = APP_CONNECTED;
    }
#endif
}

static void run_pmd_policy_tick(void) {
#if H10_ENABLE_ECG_POLICY
    if (!connected || conn_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    if (app_state != APP_W4_PMD_START || pmd_policy_done || pmd_policy_started) {
        return;
    }

    pmd_policy_started = true;
    ecg_start_attempts_total += 1;

    printf("[h10probe] PMD policy start state=%s ecg_attempt=%" PRIu32 " imu_attempt=%" PRIu32 " enc_key=%u mtu=%u cp=0x%04x data=0x%04x\n",
           state_name(app_state),
           ecg_start_attempts_total,
           imu_start_attempts_total + 1,
           gap_encryption_key_size(conn_handle),
           att_mtu,
           pmd_cp_char.value_handle,
           pmd_data_char.value_handle);

    polar_ble_driver_pmd_start_ops_t ops = {
        .ctx = NULL,
        .is_connected = pmd_is_connected,
        .encryption_key_size = pmd_encryption_key_size,
        .request_pairing = pmd_request_pairing,
        .sleep_ms = pmd_sleep_ms_cb,
        .enable_notifications = pmd_enable_notifications_cb,
        .ensure_minimum_mtu = pmd_ensure_minimum_mtu_cb,
        .start_ecg_and_wait_response = pmd_start_measurement_and_wait_response_cb,
    };

    polar_ble_driver_pmd_start_policy_t ecg_policy = {
        .ccc_attempts = H10_PMD_CCC_ATTEMPTS,
        .security_rounds_per_attempt = H10_PMD_SECURITY_ROUNDS,
        .security_wait_ms = H10_PMD_SECURITY_WAIT_MS,
        .minimum_mtu = H10_PMD_MIN_MTU,
        .sample_rate = H10_PMD_ECG_SAMPLE_RATE,
        .include_resolution = true,
        .resolution = H10_PMD_ECG_RESOLUTION,
        .include_range = false,
        .range = 0,
    };

    uint8_t ecg_response_status = 0xff;
    int ecg_last_ccc_att_status = 0;
    polar_ble_driver_pmd_start_result_t ecg_result = polar_ble_driver_pmd_start_ecg_with_policy(
        &ecg_policy,
        &ops,
        &ecg_response_status,
        &ecg_last_ccc_att_status);

    printf("[h10probe] shared PMD ECG start result=%d(%s) ccc_att=%d resp=0x%02x enc_key=%u mtu=%u\n",
           ecg_result,
           pmd_start_result_name(ecg_result),
           ecg_last_ccc_att_status,
           ecg_response_status,
           gap_encryption_key_size(conn_handle),
           att_mtu);

    if (ecg_result != POLAR_BLE_DRIVER_PMD_START_RESULT_OK) {
        app_state = APP_CONNECTED;
        pmd_policy_done = true;
        return;
    }

    ecg_start_success_total += 1;
    imu_start_attempts_total += 1;

    polar_ble_driver_pmd_start_policy_t imu_policy = {
        .ccc_attempts = H10_PMD_CCC_ATTEMPTS,
        .security_rounds_per_attempt = H10_PMD_SECURITY_ROUNDS,
        .security_wait_ms = H10_PMD_SECURITY_WAIT_MS,
        .minimum_mtu = H10_PMD_MIN_MTU,
        .sample_rate = H10_PMD_IMU_SAMPLE_RATE,
        .include_resolution = true,
        .resolution = H10_PMD_IMU_RESOLUTION,
        .include_range = true,
        .range = H10_PMD_IMU_RANGE,
    };

    uint8_t imu_response_status = 0xff;
    int imu_last_ccc_att_status = 0;
    polar_ble_driver_pmd_start_result_t imu_result = polar_ble_driver_pmd_start_acc_with_policy(
        &imu_policy,
        &ops,
        &imu_response_status,
        &imu_last_ccc_att_status);

    printf("[h10probe] shared PMD IMU start result=%d(%s) ccc_att=%d resp=0x%02x enc_key=%u mtu=%u\n",
           imu_result,
           pmd_start_result_name(imu_result),
           imu_last_ccc_att_status,
           imu_response_status,
           gap_encryption_key_size(conn_handle),
           att_mtu);

    if (imu_result == POLAR_BLE_DRIVER_PMD_START_RESULT_OK) {
        imu_start_success_total += 1;
        app_state = APP_STREAMING;
    } else {
        app_state = APP_CONNECTED;
    }

    pmd_policy_done = true;
#endif
}

static void start_scan(void) {
    if (app_state == APP_SCANNING || app_state == APP_CONNECTING) {
        return;
    }

    printf("[h10probe] start scan (target=%s valid=%d)\n", H10_TARGET_ADDR, target_addr_valid);
    app_state = APP_SCANNING;
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    gap_start_scan();
}

static void on_disconnect_cleanup(void) {
    connected = false;
    conn_handle = HCI_CON_HANDLE_INVALID;

    if (hr_listener_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&hr_listener);
        hr_listener_registered = false;
    }
    if (pmd_cp_listener_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&pmd_cp_listener);
        pmd_cp_listener_registered = false;
    }
    if (pmd_data_listener_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&pmd_data_listener);
        pmd_data_listener_registered = false;
    }

    memset(&hr_service, 0, sizeof(hr_service));
    memset(&hr_char, 0, sizeof(hr_char));
    hr_service_found = false;
    hr_char_found = false;

    memset(&pmd_service, 0, sizeof(pmd_service));
    memset(&pmd_cp_char, 0, sizeof(pmd_cp_char));
    memset(&pmd_data_char, 0, sizeof(pmd_data_char));
    pmd_service_found = false;
    pmd_cp_found = false;
    pmd_data_found = false;

    pmd_cfg_pending = false;
    pmd_cfg_done = false;
    pmd_cfg_att_status = ATT_ERROR_SUCCESS;
    pmd_write_pending = false;
    pmd_write_done = false;
    pmd_write_att_status = ATT_ERROR_SUCCESS;
    pmd_cp_response_waiting = false;
    pmd_cp_response_done = false;
    pmd_cp_response_status = 0xff;

    mtu_exchange_pending = false;
    mtu_exchange_done = false;
    att_mtu = ATT_DEFAULT_MTU;

    pmd_policy_started = false;
    pmd_policy_done = false;

    pmd_data_notifications_total = 0;
    pmd_data_ecg_notifications_total = 0;
    pmd_data_imu_notifications_total = 0;
    pmd_data_unknown_notifications_total = 0;

    app_state = APP_OFF;
}

static void reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    start_scan();
}

static void schedule_reconnect(uint32_t delay_ms) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, reconnect_timer_handler);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void heartbeat_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t since_connect = connected ? (now - connect_time_ms) : 0;
    uint32_t since_hr = (last_hr_ms > 0) ? (now - last_hr_ms) : UINT32_MAX;

    if (since_hr == UINT32_MAX) {
        printf("[h10probe] hb t=%" PRIu32 " state=%s conn=%d handle=0x%04x since_connect=%" PRIu32 "ms hr=never ecg=%" PRIu32 "/%" PRIu32 " imu=%" PRIu32 "/%" PRIu32 " pmd_data=%" PRIu32 " (ecg=%" PRIu32 " imu=%" PRIu32 " other=%" PRIu32 ")\n",
               now, state_name(app_state), connected, conn_handle, since_connect,
               ecg_start_success_total, ecg_start_attempts_total,
               imu_start_success_total, imu_start_attempts_total,
               pmd_data_notifications_total,
               pmd_data_ecg_notifications_total,
               pmd_data_imu_notifications_total,
               pmd_data_unknown_notifications_total);
    } else {
        printf("[h10probe] hb t=%" PRIu32 " state=%s conn=%d handle=0x%04x since_connect=%" PRIu32 "ms since_hr=%" PRIu32 "ms ecg=%" PRIu32 "/%" PRIu32 " imu=%" PRIu32 "/%" PRIu32 " pmd_data=%" PRIu32 " (ecg=%" PRIu32 " imu=%" PRIu32 " other=%" PRIu32 ")\n",
               now, state_name(app_state), connected, conn_handle, since_connect, since_hr,
               ecg_start_success_total, ecg_start_attempts_total,
               imu_start_success_total, imu_start_attempts_total,
               pmd_data_notifications_total,
               pmd_data_ecg_notifications_total,
               pmd_data_imu_notifications_total,
               pmd_data_unknown_notifications_total);
    }

    if (connected && conn_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(conn_handle);
    }

    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
}

static void on_connection_ready_common(hci_con_handle_t handle) {
    conn_handle = handle;
    connected = runtime_link.connected;
    connect_time_ms = btstack_run_loop_get_time_ms();
    polar_ble_driver_connect_init(&reconnect_state, connect_time_ms);

    request_post_connect_update(conn_handle);

#if H10_FORCE_PAIRING
    int perr = sm_request_pairing(conn_handle);
    printf("[h10probe] sm_request_pairing err=%d\n", perr);
#endif

    app_state = APP_CONNECTED;
#if H10_ENABLE_ECG_POLICY
    maybe_start_pmd_policy_pipeline();
#else
    maybe_start_hr_pipeline();
#endif
}

static bool probe_adv_runtime_is_scanning(void *ctx) {
    UNUSED(ctx);
    return app_state == APP_SCANNING;
}

static void probe_adv_runtime_on_match(void *ctx, const polar_ble_driver_btstack_adv_report_t *report) {
    UNUSED(ctx);
    memcpy(peer_addr, report->addr, sizeof(peer_addr));
    peer_addr_type = report->addr_type;

    printf("[h10probe] adv match addr=%s type=%u rssi=%d -> connect\n",
           bd_addr_to_str(peer_addr), peer_addr_type, report->rssi);
    app_state = APP_CONNECTING;
}

static int probe_adv_runtime_stop_scan(void *ctx) {
    UNUSED(ctx);
    gap_stop_scan();
    return ERROR_CODE_SUCCESS;
}

static int probe_adv_runtime_connect(void *ctx, const uint8_t *addr, uint8_t addr_type) {
    UNUSED(ctx);
    bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    return gap_connect(a, addr_type);
}

static void probe_dispatch_on_adv_report(void *ctx, const polar_ble_driver_btstack_adv_report_t *adv_report) {
    UNUSED(ctx);

    polar_ble_driver_btstack_scan_filter_t filter = {
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

    polar_ble_driver_btstack_adv_runtime_ops_t ops = {
        .ctx = NULL,
        .is_scanning = probe_adv_runtime_is_scanning,
        .on_report = NULL,
        .on_match = probe_adv_runtime_on_match,
        .stop_scan = probe_adv_runtime_stop_scan,
        .connect = probe_adv_runtime_connect,
        .on_connect_error = NULL,
    };
    (void)polar_ble_driver_btstack_adv_runtime_on_report(
        &runtime_link,
        &filter,
        adv_report,
        ERROR_CODE_SUCCESS,
        &ops);
}

static void probe_link_on_connected_ready(void *ctx, const polar_ble_driver_link_event_t *link_event) {
    UNUSED(ctx);
    on_connection_ready_common(link_event->handle);
}

static void probe_link_on_disconnected(void *ctx, const polar_ble_driver_link_event_t *link_event) {
    UNUSED(ctx);
    printf("[h10probe] disconnect status=0x%02x handle=0x%04x reason=0x%02x\n",
           link_event->status, link_event->handle, link_event->reason);
    on_disconnect_cleanup();
    schedule_reconnect(next_reconnect_delay_ms());
}

static void probe_link_on_conn_update_complete(void *ctx, const polar_ble_driver_link_event_t *link_event) {
    UNUSED(ctx);
    printf("[h10probe] conn update complete status=0x%02x handle=0x%04x interval=%u lat=%u sup=%u\n",
           link_event->status, link_event->handle, link_event->conn_interval, link_event->conn_latency, link_event->supervision_timeout_10ms);
}

static void probe_dispatch_on_link_event(void *ctx, const polar_ble_driver_link_event_t *link_event) {
    UNUSED(ctx);
    if (link_event->type == POLAR_BLE_DRIVER_LINK_EVENT_CONN_COMPLETE) {
        printf("[h10probe] conn complete status=0x%02x handle=0x%04x role=%u interval=%u lat=%u sup=%u\n",
               link_event->status,
               link_event->handle,
               0,
               link_event->conn_interval,
               link_event->conn_latency,
               link_event->supervision_timeout_10ms);

        if (connected && link_event->status == ERROR_CODE_SUCCESS && conn_handle == link_event->handle) {
            printf("[h10probe] duplicate conn-complete ignored\n");
            return;
        }
    }

    polar_ble_driver_runtime_context_link_ops_t ops = {
        .ctx = NULL,
        .on_connected_ready = probe_link_on_connected_ready,
        .on_disconnected = probe_link_on_disconnected,
        .on_conn_update_complete = probe_link_on_conn_update_complete,
    };
    bool handled = polar_ble_driver_runtime_context_handle_link_event(
        &runtime_link,
        HCI_CON_HANDLE_INVALID,
        link_event,
        false,
        true,
        &ops);
    if (link_event->type == POLAR_BLE_DRIVER_LINK_EVENT_CONN_COMPLETE &&
        handled &&
        link_event->status != ERROR_CODE_SUCCESS) {
        on_disconnect_cleanup();
        schedule_reconnect(next_reconnect_delay_ms());
    }
}

static void probe_dispatch_on_sm_event(void *ctx, const polar_ble_driver_sm_event_t *sm_event) {
    UNUSED(ctx);
    UNUSED(sm_event);
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    uint8_t event = hci_event_packet_get_type(packet);

    polar_ble_driver_btstack_dispatch_ops_t dispatch_ops = {
        .ctx = NULL,
        .on_adv_report = probe_dispatch_on_adv_report,
        .on_link_event = probe_dispatch_on_link_event,
        .on_sm_event = probe_dispatch_on_sm_event,
    };
    if (polar_ble_driver_btstack_dispatch_event(packet_type, packet, &dispatch_ops)) {
        return;
    }

    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("[h10probe] BTstack ready\n");
                polar_ble_driver_connect_init(&reconnect_state, btstack_run_loop_get_time_ms());
                btstack_run_loop_set_timer_handler(&heartbeat_timer, heartbeat_timer_handler);
                btstack_run_loop_set_timer(&heartbeat_timer, 1000);
                btstack_run_loop_add_timer(&heartbeat_timer);
                start_scan();
            }
            break;

        case HCI_EVENT_ENCRYPTION_CHANGE:
            printf("[h10probe] enc_change status=0x%02x handle=0x%04x enabled=%u\n",
                   hci_event_encryption_change_get_status(packet),
                   hci_event_encryption_change_get_connection_handle(packet),
                   hci_event_encryption_change_get_encryption_enabled(packet));
            break;

        case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
            uint8_t key_size = hci_event_encryption_change_v2_get_encryption_key_size(packet);
            printf("[h10probe] enc_change_v2 status=0x%02x handle=0x%04x enabled=%u key_size=%u secure=%u\n",
                   hci_event_encryption_change_v2_get_status(packet),
                   hci_event_encryption_change_v2_get_connection_handle(packet),
                   hci_event_encryption_change_v2_get_encryption_enabled(packet),
                   key_size,
                   polar_ble_driver_pmd_security_ready(key_size));
            break;
        }

        case GAP_EVENT_RSSI_MEASUREMENT:
            printf("[h10probe] rssi handle=0x%04x rssi=%d\n",
                   gap_event_rssi_measurement_get_con_handle(packet),
                   gap_event_rssi_measurement_get_rssi(packet));
            break;

        default:
            break;
    }
}

static void probe_sm_on_just_works_request(void *ctx, uint16_t handle) {
    UNUSED(ctx);
    printf("[h10probe] SM just works request\n");
    sm_just_works_confirm(handle);
}

static void probe_sm_on_numeric_comparison_request(void *ctx, uint16_t handle) {
    UNUSED(ctx);
    printf("[h10probe] SM numeric comparison request\n");
    sm_numeric_comparison_confirm(handle);
}

static void probe_sm_on_authorization_request(void *ctx, uint16_t handle) {
    UNUSED(ctx);
    printf("[h10probe] SM authorization request\n");
    sm_authorization_grant(handle);
}

static void probe_sm_on_pairing_complete(void *ctx, const polar_ble_driver_sm_event_t *event) {
    UNUSED(ctx);
    printf("[h10probe] SM pairing complete status=0x%02x reason=0x%02x\n",
           event->status,
           event->reason);
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    polar_ble_driver_sm_event_t sm_event;
    if (polar_ble_driver_btstack_decode_sm_event(packet_type, packet, &sm_event)) {
        polar_ble_driver_sm_control_ops_t ops = {
            .ctx = NULL,
            .on_just_works_request = probe_sm_on_just_works_request,
            .on_numeric_comparison_request = probe_sm_on_numeric_comparison_request,
            .on_authorization_request = probe_sm_on_authorization_request,
            .on_pairing_complete = probe_sm_on_pairing_complete,
        };
        if (polar_ble_driver_sm_control_apply(&sm_event, conn_handle, HCI_CON_HANDLE_INVALID, &ops)) {
            return;
        }
    }

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PAIRING_STARTED:
            printf("[h10probe] SM pairing started\n");
            break;

        case SM_EVENT_REENCRYPTION_STARTED:
            printf("[h10probe] SM re-encryption started\n");
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            printf("[h10probe] SM re-encryption complete status=0x%02x\n",
                   sm_event_reencryption_complete_get_status(packet));
            break;

        default:
            break;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    printf("\n[h10probe] ===== RP2-1 BTstack/CYW43 probe =====\n");
    printf("[h10probe] target=%s force_pairing=%d hr=%d pmd_policy=%d post_update=%d\n",
           H10_TARGET_ADDR,
           H10_FORCE_PAIRING,
           H10_ENABLE_HR,
           H10_ENABLE_ECG_POLICY,
           H10_POST_CONNECT_UPDATE);

    uint8_t pmd_ecg_start_cmd[16];
    polar_ble_driver_pmd_ecg_start_config_t pmd_ecg_cfg = {
        .sample_rate = H10_PMD_ECG_SAMPLE_RATE,
        .include_resolution = true,
        .resolution = H10_PMD_ECG_RESOLUTION,
    };
    size_t pmd_ecg_start_cmd_len = polar_ble_driver_pmd_build_ecg_start_command(
        &pmd_ecg_cfg,
        pmd_ecg_start_cmd,
        sizeof(pmd_ecg_start_cmd));

    uint8_t pmd_imu_start_cmd[20];
    polar_ble_driver_pmd_acc_start_config_t pmd_imu_cfg = {
        .sample_rate = H10_PMD_IMU_SAMPLE_RATE,
        .include_resolution = true,
        .resolution = H10_PMD_IMU_RESOLUTION,
        .include_range = true,
        .range = H10_PMD_IMU_RANGE,
    };
    size_t pmd_imu_start_cmd_len = polar_ble_driver_pmd_build_acc_start_command(
        &pmd_imu_cfg,
        pmd_imu_start_cmd,
        sizeof(pmd_imu_start_cmd));
    printf("[h10probe] shared-driver PMD templates ecg_len=%u imu_len=%u\n",
           (unsigned)pmd_ecg_start_cmd_len,
           (unsigned)pmd_imu_start_cmd_len);

    target_addr_valid = sscanf_bd_addr(H10_TARGET_ADDR, target_addr) != 0;
    if (!target_addr_valid) {
        printf("[h10probe] WARNING: invalid H10_TARGET_ADDR, fallback to adv name matching\n");
    }

    if (cyw43_arch_init()) {
        printf("[h10probe] cyw43_arch_init failed\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    l2cap_init();
    sm_init();
    gatt_client_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    polar_ble_driver_runtime_link_init(&runtime_link, HCI_CON_HANDLE_INVALID);

    hci_event_cb.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_cb);

    sm_event_cb.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_cb);

    hci_power_control(HCI_POWER_ON);

    while (true) {
#if H10_ENABLE_ECG_POLICY
        run_pmd_policy_tick();
#endif
        cyw43_arch_poll();
        sleep_ms(1);
    }
}
