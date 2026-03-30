#include "logger/h10.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "polar_sdk_btstack_adv_runtime.h"
#include "polar_sdk_btstack_dispatch.h"
#include "polar_sdk_btstack_scan.h"
#include "polar_sdk_btstack_sm.h"
#include "polar_sdk_connect.h"
#include "polar_sdk_runtime.h"
#include "polar_sdk_runtime_context.h"
#include "polar_sdk_sm_control.h"

#define LOGGER_H10_SCAN_INTERVAL_UNITS 0x0030
#define LOGGER_H10_SCAN_WINDOW_UNITS 0x0030
#define LOGGER_H10_CONNECT_TIMEOUT_WINDOW_MS 60000u
#define LOGGER_H10_CONNECT_ATTEMPT_SLICE_MS 3500u

static logger_h10_state_t *g_h10 = NULL;
static bool g_btstack_core_initialized = false;
static btstack_packet_callback_registration_t g_hci_event_cb;
static btstack_packet_callback_registration_t g_sm_event_cb;
static polar_sdk_runtime_link_t g_runtime_link;
static polar_sdk_connect_policy_t g_reconnect_policy = {
    .timeout_ms = LOGGER_H10_CONNECT_TIMEOUT_WINDOW_MS,
    .attempt_slice_ms = LOGGER_H10_CONNECT_ATTEMPT_SLICE_MS,
};
static polar_sdk_connect_state_t g_reconnect_state;
static hci_con_handle_t g_conn_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t g_target_addr;
static bd_addr_t g_peer_addr;
static bd_addr_type_t g_peer_addr_type = BD_ADDR_TYPE_UNKNOWN;
static bool g_user_disconnect_requested = false;

static void logger_copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0u;
    while (src[i] != '\0' && (i + 1u) < dst_len) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

const char *logger_h10_phase_name(logger_h10_phase_t phase) {
    switch (phase) {
        case LOGGER_H10_PHASE_OFF:
            return "off";
        case LOGGER_H10_PHASE_WAITING:
            return "waiting";
        case LOGGER_H10_PHASE_SCANNING:
            return "scanning";
        case LOGGER_H10_PHASE_CONNECTING:
            return "connecting";
        case LOGGER_H10_PHASE_SECURING:
            return "securing";
        case LOGGER_H10_PHASE_READY:
            return "ready";
        default:
            return "unknown";
    }
}

static void logger_h10_clear_link_state(logger_h10_state_t *state) {
    state->scanning = false;
    state->connect_intent = false;
    state->connected = false;
    state->encrypted = false;
    state->secure = false;
    state->pairing_requested = false;
    state->encryption_key_size = 0u;
    state->connected_address[0] = '\0';
    g_conn_handle = HCI_CON_HANDLE_INVALID;
    g_peer_addr_type = BD_ADDR_TYPE_UNKNOWN;
    memset(g_peer_addr, 0, sizeof(g_peer_addr));
}

static void logger_h10_set_phase(logger_h10_state_t *state, logger_h10_phase_t phase) {
    state->phase = phase;
}

static uint32_t logger_h10_next_reconnect_delay_ms(uint32_t now_ms) {
    uint32_t delay_ms = polar_sdk_connect_next_backoff_ms(&g_reconnect_policy, &g_reconnect_state, now_ms);
    if (delay_ms > 0u) {
        return delay_ms;
    }

    polar_sdk_connect_init(&g_reconnect_state, now_ms);
    delay_ms = polar_sdk_connect_next_backoff_ms(&g_reconnect_policy, &g_reconnect_state, now_ms);
    return delay_ms > 0u ? delay_ms : 1000u;
}

static void logger_h10_schedule_retry(logger_h10_state_t *state, uint32_t now_ms) {
    state->next_retry_mono_ms = now_ms + logger_h10_next_reconnect_delay_ms(now_ms);
    logger_h10_set_phase(state, LOGGER_H10_PHASE_WAITING);
}

static void logger_h10_stop_scan(logger_h10_state_t *state) {
    if (state->scanning) {
        gap_stop_scan();
        state->scanning = false;
    }
}

static void logger_h10_start_scan(logger_h10_state_t *state) {
    if (!state->enabled || !state->controller_ready || !state->target_address_valid || state->scanning || state->connect_intent || state->connected) {
        return;
    }

    gap_set_scan_parameters(1, LOGGER_H10_SCAN_INTERVAL_UNITS, LOGGER_H10_SCAN_WINDOW_UNITS);
    gap_start_scan();
    state->scanning = true;
    state->next_retry_mono_ms = 0u;
    logger_h10_set_phase(state, LOGGER_H10_PHASE_SCANNING);
}

static void logger_h10_power_off(logger_h10_state_t *state) {
    logger_h10_stop_scan(state);
    hci_power_control(HCI_POWER_OFF);
    state->controller_ready = false;
    state->enabled = false;
    state->next_retry_mono_ms = 0u;
    logger_h10_clear_link_state(state);
    logger_h10_set_phase(state, LOGGER_H10_PHASE_OFF);
}

static void logger_h10_on_connected_ready(void *ctx, const polar_sdk_link_event_t *event) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    state->connected = true;
    state->connect_intent = false;
    state->scanning = false;
    state->connect_count += 1u;
    g_conn_handle = event->handle;
    polar_sdk_connect_init(&g_reconnect_state, btstack_run_loop_get_time_ms());
    if (g_peer_addr_type != BD_ADDR_TYPE_UNKNOWN) {
        logger_copy_string(state->connected_address, sizeof(state->connected_address), bd_addr_to_str(g_peer_addr));
    }
    logger_h10_set_phase(state, LOGGER_H10_PHASE_SECURING);
}

static void logger_h10_on_disconnected(void *ctx, const polar_sdk_link_event_t *event) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    state->disconnect_count += 1u;
    state->last_disconnect_reason = event->reason;
    logger_h10_clear_link_state(state);
    if (state->enabled && state->controller_ready && state->target_address_valid) {
        logger_h10_schedule_retry(state, btstack_run_loop_get_time_ms());
    } else {
        logger_h10_set_phase(state, state->enabled ? LOGGER_H10_PHASE_WAITING : LOGGER_H10_PHASE_OFF);
    }
}

static void logger_h10_on_conn_update_complete(void *ctx, const polar_sdk_link_event_t *event) {
    (void)ctx;
    (void)event;
}

static bool logger_h10_adv_runtime_is_scanning(void *ctx) {
    const logger_h10_state_t *state = (const logger_h10_state_t *)ctx;
    return state->scanning;
}

static void logger_h10_adv_runtime_on_match(void *ctx, const polar_sdk_btstack_adv_report_t *report) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    memcpy(g_peer_addr, report->addr, sizeof(g_peer_addr));
    g_peer_addr_type = report->addr_type;
    state->seen_bound_device = true;
    state->seen_count += 1u;
    state->last_seen_rssi = report->rssi;
    state->last_seen_mono_ms = btstack_run_loop_get_time_ms();
    logger_copy_string(state->last_seen_address, sizeof(state->last_seen_address), bd_addr_to_str(g_peer_addr));
    state->scanning = false;
    state->connect_intent = true;
    logger_h10_set_phase(state, LOGGER_H10_PHASE_CONNECTING);
}

static int logger_h10_adv_runtime_stop_scan(void *ctx) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    logger_h10_stop_scan(state);
    return ERROR_CODE_SUCCESS;
}

static int logger_h10_adv_runtime_connect(void *ctx, const uint8_t *addr, uint8_t addr_type) {
    (void)ctx;
    bd_addr_t target;
    memcpy(target, addr, sizeof(target));
    return gap_connect(target, addr_type);
}

static void logger_h10_dispatch_on_adv_report(void *ctx, const polar_sdk_btstack_adv_report_t *adv_report) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    polar_sdk_btstack_scan_filter_t filter = {
        .use_addr = state->target_address_valid,
        .addr = {0},
        .use_name_prefix = false,
        .name_prefix = NULL,
        .name_prefix_len = 0u,
        .use_name_contains_pair = false,
        .name_contains_a = NULL,
        .name_contains_b = NULL,
    };
    if (state->target_address_valid) {
        memcpy(filter.addr, g_target_addr, sizeof(g_target_addr));
    }

    polar_sdk_btstack_adv_runtime_ops_t ops = {
        .ctx = state,
        .is_scanning = logger_h10_adv_runtime_is_scanning,
        .on_report = NULL,
        .on_match = logger_h10_adv_runtime_on_match,
        .stop_scan = logger_h10_adv_runtime_stop_scan,
        .connect = logger_h10_adv_runtime_connect,
        .on_connect_error = NULL,
    };
    (void)polar_sdk_btstack_adv_runtime_on_report(
        &g_runtime_link,
        &filter,
        adv_report,
        ERROR_CODE_SUCCESS,
        &ops);
}

static void logger_h10_dispatch_on_link_event(void *ctx, const polar_sdk_link_event_t *link_event) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    polar_sdk_runtime_context_link_ops_t ops = {
        .ctx = state,
        .on_connected_ready = logger_h10_on_connected_ready,
        .on_disconnected = logger_h10_on_disconnected,
        .on_conn_update_complete = logger_h10_on_conn_update_complete,
    };
    const bool handled = polar_sdk_runtime_context_handle_link_event(
        &g_runtime_link,
        HCI_CON_HANDLE_INVALID,
        link_event,
        g_user_disconnect_requested,
        state->enabled,
        &ops);
    if (link_event->type == POLAR_SDK_LINK_EVENT_CONN_COMPLETE && handled && link_event->status != ERROR_CODE_SUCCESS) {
        state->connect_intent = false;
        logger_h10_schedule_retry(state, btstack_run_loop_get_time_ms());
    }
    if (link_event->type == POLAR_SDK_LINK_EVENT_DISCONNECT) {
        g_user_disconnect_requested = false;
    }
}

static void logger_h10_dispatch_on_sm_event(void *ctx, const polar_sdk_sm_event_t *sm_event) {
    logger_h10_state_t *state = (logger_h10_state_t *)ctx;
    if (sm_event->type == POLAR_SDK_SM_EVENT_JUST_WORKS_REQUEST) {
        sm_just_works_confirm(sm_event->handle);
        return;
    }
    if (sm_event->type == POLAR_SDK_SM_EVENT_NUMERIC_COMPARISON_REQUEST) {
        sm_numeric_comparison_confirm(sm_event->handle);
        return;
    }
    if (sm_event->type == POLAR_SDK_SM_EVENT_AUTHORIZATION_REQUEST) {
        sm_authorization_grant(sm_event->handle);
        return;
    }
    if (sm_event->type == POLAR_SDK_SM_EVENT_PAIRING_COMPLETE) {
        state->pairing_requested = false;
        state->last_pairing_status = sm_event->status;
        state->last_pairing_reason = sm_event->reason;
        if (sm_event->status == ERROR_CODE_SUCCESS) {
            state->bonded = true;
        }
    }
}

static void logger_h10_hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET || g_h10 == NULL) {
        return;
    }

    polar_sdk_btstack_dispatch_ops_t dispatch_ops = {
        .ctx = g_h10,
        .on_adv_report = logger_h10_dispatch_on_adv_report,
        .on_link_event = logger_h10_dispatch_on_link_event,
        .on_sm_event = logger_h10_dispatch_on_sm_event,
    };
    if (polar_sdk_btstack_dispatch_event(packet_type, packet, &dispatch_ops)) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t btstack_state = btstack_event_state_get_state(packet);
            g_h10->controller_ready = btstack_state == HCI_STATE_WORKING;
            if (btstack_state == HCI_STATE_WORKING) {
                polar_sdk_connect_init(&g_reconnect_state, btstack_run_loop_get_time_ms());
                logger_h10_set_phase(g_h10, LOGGER_H10_PHASE_WAITING);
            } else if (!g_h10->enabled) {
                logger_h10_set_phase(g_h10, LOGGER_H10_PHASE_OFF);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            if (hci_event_encryption_change_get_connection_handle(packet) != g_conn_handle) {
                break;
            }
            const bool encrypted = hci_event_encryption_change_get_encryption_enabled(packet) != 0;
            g_h10->encrypted = encrypted;
            g_h10->secure = encrypted;
            g_h10->bonded = g_h10->bonded || encrypted;
            logger_h10_set_phase(g_h10, encrypted ? LOGGER_H10_PHASE_READY : LOGGER_H10_PHASE_SECURING);
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
            if (hci_event_encryption_change_v2_get_connection_handle(packet) != g_conn_handle) {
                break;
            }
            const bool encrypted = hci_event_encryption_change_v2_get_encryption_enabled(packet) != 0;
            g_h10->encryption_key_size = hci_event_encryption_change_v2_get_encryption_key_size(packet);
            g_h10->encrypted = encrypted;
            g_h10->secure = encrypted && g_h10->encryption_key_size > 0u;
            g_h10->bonded = g_h10->bonded || g_h10->secure;
            logger_h10_set_phase(g_h10, g_h10->secure ? LOGGER_H10_PHASE_READY : LOGGER_H10_PHASE_SECURING);
            break;
        }

        default:
            break;
    }
}

static void logger_h10_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET || g_h10 == NULL) {
        return;
    }

    polar_sdk_sm_event_t sm_event;
    if (polar_sdk_btstack_decode_sm_event(packet_type, packet, &sm_event)) {
        logger_h10_dispatch_on_sm_event(g_h10, &sm_event);
    }
}

static void logger_h10_init_btstack_core(void) {
    if (g_btstack_core_initialized) {
        return;
    }

    l2cap_init();
    sm_init();
    gatt_client_init();
    polar_sdk_btstack_sm_apply_default_auth_policy();

    polar_sdk_runtime_link_init(&g_runtime_link, HCI_CON_HANDLE_INVALID);
    polar_sdk_connect_init(&g_reconnect_state, 0u);

    g_hci_event_cb.callback = logger_h10_hci_packet_handler;
    hci_add_event_handler(&g_hci_event_cb);
    g_sm_event_cb.callback = logger_h10_sm_packet_handler;
    sm_add_event_handler(&g_sm_event_cb);

    g_btstack_core_initialized = true;
}

void logger_h10_init(logger_h10_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->last_seen_rssi = -127;
    state->phase = LOGGER_H10_PHASE_OFF;
    state->initialized = true;
    g_h10 = state;
    logger_h10_init_btstack_core();
}

bool logger_h10_set_bound_address(logger_h10_state_t *state, const char *bound_address) {
    logger_copy_string(state->bound_address, sizeof(state->bound_address), bound_address);
    if (bound_address == NULL || bound_address[0] == '\0') {
        state->target_address_valid = false;
        memset(g_target_addr, 0, sizeof(g_target_addr));
        return true;
    }

    state->target_address_valid = sscanf_bd_addr(bound_address, g_target_addr) != 0;
    return state->target_address_valid;
}

void logger_h10_set_enabled(logger_h10_state_t *state, bool enabled) {
    if (enabled) {
        if (state->enabled) {
            return;
        }
        state->enabled = true;
        state->bonded = false;
        state->last_pairing_status = 0u;
        state->last_pairing_reason = 0u;
        state->last_disconnect_reason = 0u;
        state->next_retry_mono_ms = 0u;
        logger_h10_clear_link_state(state);
        logger_h10_set_phase(state, LOGGER_H10_PHASE_WAITING);
        hci_power_control(HCI_POWER_ON);
        return;
    }

    if (!state->enabled && state->phase == LOGGER_H10_PHASE_OFF) {
        return;
    }
    g_user_disconnect_requested = true;
    logger_h10_power_off(state);
}

void logger_h10_poll(logger_h10_state_t *state, uint32_t now_ms) {
    if (!state->enabled || !state->controller_ready) {
        return;
    }
    if (!state->target_address_valid) {
        logger_h10_set_phase(state, LOGGER_H10_PHASE_WAITING);
        return;
    }

    if (state->connected) {
        if (state->secure) {
            logger_h10_set_phase(state, LOGGER_H10_PHASE_READY);
            return;
        }
        logger_h10_set_phase(state, LOGGER_H10_PHASE_SECURING);
        if (!state->pairing_requested && g_conn_handle != HCI_CON_HANDLE_INVALID) {
            sm_request_pairing(g_conn_handle);
            state->pairing_requested = true;
        }
        return;
    }

    if (state->connect_intent) {
        logger_h10_set_phase(state, LOGGER_H10_PHASE_CONNECTING);
        return;
    }
    if (state->scanning) {
        logger_h10_set_phase(state, LOGGER_H10_PHASE_SCANNING);
        return;
    }

    logger_h10_set_phase(state, LOGGER_H10_PHASE_WAITING);
    if (state->next_retry_mono_ms != 0u && (int32_t)(now_ms - state->next_retry_mono_ms) < 0) {
        return;
    }
    logger_h10_start_scan(state);
}