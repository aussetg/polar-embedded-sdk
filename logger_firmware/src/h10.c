#include "logger/h10.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "polar_sdk_btstack_adv_runtime.h"
#include "polar_sdk_btstack_dispatch.h"
#include "polar_sdk_btstack_gatt.h"
#include "polar_sdk_btstack_helpers.h"
#include "polar_sdk_btstack_scan.h"
#include "polar_sdk_btstack_security.h"
#include "polar_sdk_btstack_sm.h"
#include "polar_sdk_connect.h"
#include "polar_sdk_discovery_apply.h"
#include "polar_sdk_gatt_mtu.h"
#include "polar_sdk_gatt_notify_runtime.h"
#include "polar_sdk_pmd.h"
#include "polar_sdk_pmd_control.h"
#include "polar_sdk_runtime.h"
#include "polar_sdk_runtime_context.h"
#include "polar_sdk_sm_control.h"

#include "logger/capture_stats.h"
#include "logger/util.h"

#define LOGGER_H10_SCAN_INTERVAL_UNITS 0x0030
#define LOGGER_H10_SCAN_WINDOW_UNITS 0x0030
#define LOGGER_H10_CONNECT_TIMEOUT_WINDOW_MS 60000u
#define LOGGER_H10_CONNECT_ATTEMPT_SLICE_MS 3500u

#define LOGGER_H10_PMD_MIN_MTU 70u
#define LOGGER_H10_PMD_SECURITY_ROUNDS 3u
#define LOGGER_H10_PMD_SECURITY_WAIT_MS 3500u
#define LOGGER_H10_PMD_CCC_ATTEMPTS 4u
#define LOGGER_H10_ECG_SAMPLE_RATE 130u
#define LOGGER_H10_ECG_RESOLUTION 14u
#define LOGGER_H10_ACC_SAMPLE_RATE 50u
#define LOGGER_H10_ACC_RESOLUTION 16u
#define LOGGER_H10_ACC_RANGE 8u
#define LOGGER_H10_STREAM_START_TIMEOUT_MS 5000u
#define LOGGER_H10_SECURING_TIMEOUT_MS 12000u
#define LOGGER_H10_SECURITY_FAILURE_THRESHOLD 3u
#define LOGGER_H10_BOND_AUTO_CLEAR_COOLDOWN_MS 60000u
#define LOGGER_H10_BATTERY_PERIOD_MS 3600000u

#define LOGGER_H10_BATTERY_REASON_NONE 0u
#define LOGGER_H10_BATTERY_REASON_CONNECT 1u
#define LOGGER_H10_BATTERY_REASON_PERIODIC 2u

static const uint8_t LOGGER_H10_UUID_PMD_SERVICE_BE[16] = {
    0xFB, 0x00, 0x5C, 0x80, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t LOGGER_H10_UUID_PMD_CP_BE[16] = {
    0xFB, 0x00, 0x5C, 0x81, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static const uint8_t LOGGER_H10_UUID_PMD_DATA_BE[16] = {
    0xFB, 0x00, 0x5C, 0x82, 0x02, 0xE7, 0xF3, 0x87,
    0x1C, 0xAD, 0x8A, 0xCD, 0x2D, 0x8D, 0xF0, 0xC8,
};

static logger_h10_state_t *g_h10 = NULL;
static logger_capture_stats_t *g_capture_stats = NULL;
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

static gatt_client_service_t g_pmd_service;
static gatt_client_characteristic_t g_pmd_cp_char;
static gatt_client_characteristic_t g_pmd_data_char;
static bool g_pmd_service_found = false;
static bool g_pmd_cp_found = false;
static bool g_pmd_data_found = false;
static bool g_service_query_active = false;
static bool g_char_query_active = false;

static gatt_client_notification_t g_pmd_cp_listener;
static gatt_client_notification_t g_pmd_data_listener;
static bool g_pmd_cp_listener_registered = false;
static bool g_pmd_data_listener_registered = false;

static bool g_pmd_cfg_pending = false;
static bool g_pmd_cfg_done = false;
static uint8_t g_pmd_cfg_att_status = ATT_ERROR_SUCCESS;

static bool g_pmd_write_pending = false;
static bool g_pmd_write_done = false;
static uint8_t g_pmd_write_att_status = ATT_ERROR_SUCCESS;

static bool g_pmd_cp_response_waiting = false;
static bool g_pmd_cp_response_done = false;
static uint8_t g_pmd_cp_response_expected_opcode = 0u;
static uint8_t g_pmd_cp_response_expected_type = 0xffu;
static uint8_t g_pmd_cp_response_status = 0xffu;
static uint8_t g_pmd_cp_response_type = 0xffu;

static bool g_mtu_exchange_done = false;
static bool g_battery_read_active = false;
static bool g_battery_value_received = false;
static uint8_t g_battery_pending_reason = LOGGER_H10_BATTERY_REASON_NONE;

static void logger_h10_gatt_packet_handler(uint8_t packet_type,
                                           uint16_t channel, uint8_t *packet,
                                           uint16_t size);
static void logger_h10_schedule_retry(logger_h10_state_t *state,
                                      uint32_t now_ms);
static void logger_h10_stop_scan(logger_h10_state_t *state);
static void logger_h10_disconnect_for_restart(logger_h10_state_t *state);

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
  case LOGGER_H10_PHASE_STARTING:
    return "starting";
  case LOGGER_H10_PHASE_STREAMING:
    return "streaming";
  default:
    return "unknown";
  }
}

const char *
logger_h10_security_failure_name(logger_h10_security_failure_t failure) {
  switch (failure) {
  case LOGGER_H10_SECURITY_FAILURE_PAIRING_FAILED:
    return "pairing_failed";
  case LOGGER_H10_SECURITY_FAILURE_SECURE_TIMEOUT:
    return "secure_timeout";
  case LOGGER_H10_SECURITY_FAILURE_PMD_AUTH:
    return "pmd_auth";
  case LOGGER_H10_SECURITY_FAILURE_NONE:
  default:
    return NULL;
  }
}

const char *logger_h10_recovery_event_name(logger_h10_recovery_event_t event) {
  switch (event) {
  case LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_CLEARED:
    return "bond_auto_cleared";
  case LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_REPAIRED:
    return "bond_auto_repaired";
  case LOGGER_H10_RECOVERY_EVENT_NONE:
  default:
    return NULL;
  }
}

static bool logger_h10_deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return deadline_ms != 0u && (int32_t)(now_ms - deadline_ms) >= 0;
}

static void logger_h10_emit_recovery_event(logger_h10_state_t *state,
                                           logger_h10_recovery_event_t event) {
  if (state == NULL || event == LOGGER_H10_RECOVERY_EVENT_NONE) {
    return;
  }
  state->recovery_event = event;
}

static void
logger_h10_reset_security_failure_counters(logger_h10_state_t *state) {
  if (state == NULL) {
    return;
  }
  state->security_failure_count = 0u;
  state->secure_deadline_mono_ms = 0u;
}

static void
logger_h10_reset_security_recovery_cycle(logger_h10_state_t *state) {
  if (state == NULL) {
    return;
  }
  logger_h10_reset_security_failure_counters(state);
  state->last_security_failure = LOGGER_H10_SECURITY_FAILURE_NONE;
  state->bond_clear_cooldown_until_mono_ms = 0u;
  state->bond_repair_in_progress = false;
}

static bool logger_h10_maybe_auto_clear_bond(logger_h10_state_t *state,
                                             uint32_t now_ms) {
  if (state == NULL ||
      state->security_failure_count < LOGGER_H10_SECURITY_FAILURE_THRESHOLD) {
    return false;
  }
  if (state->bond_clear_cooldown_until_mono_ms != 0u &&
      !logger_h10_deadline_reached(now_ms,
                                   state->bond_clear_cooldown_until_mono_ms)) {
    return false;
  }
  if (g_peer_addr_type == BD_ADDR_TYPE_UNKNOWN) {
    return false;
  }

  gap_delete_bonding(g_peer_addr_type, g_peer_addr);
  state->bonded = false;
  state->bond_repair_in_progress = true;
  state->bond_auto_clear_count += 1u;
  state->bond_clear_cooldown_until_mono_ms =
      now_ms + LOGGER_H10_BOND_AUTO_CLEAR_COOLDOWN_MS;
  logger_h10_reset_security_failure_counters(state);
  logger_h10_emit_recovery_event(state,
                                 LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_CLEARED);
  printf("[logger] h10 auto-cleared bond for %s\n",
         state->connected_address[0] != '\0' ? state->connected_address
                                             : bd_addr_to_str(g_peer_addr));
  return true;
}

static void
logger_h10_note_security_failure(logger_h10_state_t *state,
                                 logger_h10_security_failure_t failure,
                                 uint32_t now_ms) {
  if (state == NULL || failure == LOGGER_H10_SECURITY_FAILURE_NONE ||
      state->disconnect_requested) {
    return;
  }

  state->last_security_failure = failure;
  if (state->security_failure_count < UINT32_MAX) {
    state->security_failure_count += 1u;
  }
  printf("[logger] h10 security failure kind=%s count=%lu\n",
         logger_h10_security_failure_name(failure),
         (unsigned long)state->security_failure_count);
  (void)logger_h10_maybe_auto_clear_bond(state, now_ms);
}

static void logger_h10_note_secure_start_success(logger_h10_state_t *state) {
  if (state == NULL) {
    return;
  }

  logger_h10_reset_security_failure_counters(state);
  state->last_security_failure = LOGGER_H10_SECURITY_FAILURE_NONE;
  if (!state->bond_repair_in_progress) {
    return;
  }

  state->bond_repair_in_progress = false;
  state->bond_auto_repair_count += 1u;
  logger_h10_emit_recovery_event(state,
                                 LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_REPAIRED);
  printf("[logger] h10 auto-repaired bond for %s\n",
         state->connected_address[0] != '\0' ? state->connected_address
                                             : state->bound_address);
}

static bool logger_h10_force_stale_bond_injection(logger_h10_state_t *state,
                                                  uint32_t now_ms) {
  if (state == NULL || !state->connected || state->disconnect_requested ||
      g_conn_handle == HCI_CON_HANDLE_INVALID ||
      g_peer_addr_type == BD_ADDR_TYPE_UNKNOWN) {
    return false;
  }

  state->debug_stale_bond_injection_armed = false;
  state->bond_clear_cooldown_until_mono_ms = 0u;
  state->security_failure_count = LOGGER_H10_SECURITY_FAILURE_THRESHOLD - 1u;
  logger_h10_note_security_failure(
      state, LOGGER_H10_SECURITY_FAILURE_SECURE_TIMEOUT, now_ms);
  logger_h10_disconnect_for_restart(state);
  return true;
}

static void logger_h10_reset_packet_queue(logger_h10_state_t *state) {
  state->packet_read_index = 0u;
  state->packet_write_index = 0u;
  state->packet_count = 0u;
  memset(state->packets, 0, sizeof(state->packets));
}

static void logger_h10_note_packet_drop(logger_h10_state_t *state,
                                        logger_h10_stream_kind_t stream_kind) {
  if (stream_kind == LOGGER_H10_STREAM_KIND_ACC) {
    state->acc_packet_drop_count += 1u;
  } else {
    state->ecg_packet_drop_count += 1u;
  }
}

static void logger_h10_reset_pmd_state(logger_h10_state_t *state) {
  if (g_pmd_cp_listener_registered) {
    gatt_client_stop_listening_for_characteristic_value_updates(
        &g_pmd_cp_listener);
    g_pmd_cp_listener_registered = false;
  }
  if (g_pmd_data_listener_registered) {
    gatt_client_stop_listening_for_characteristic_value_updates(
        &g_pmd_data_listener);
    g_pmd_data_listener_registered = false;
  }

  memset(&g_pmd_service, 0, sizeof(g_pmd_service));
  memset(&g_pmd_cp_char, 0, sizeof(g_pmd_cp_char));
  memset(&g_pmd_data_char, 0, sizeof(g_pmd_data_char));
  g_pmd_service_found = false;
  g_pmd_cp_found = false;
  g_pmd_data_found = false;
  g_service_query_active = false;
  g_char_query_active = false;

  g_pmd_cfg_pending = false;
  g_pmd_cfg_done = false;
  g_pmd_cfg_att_status = ATT_ERROR_SUCCESS;
  g_pmd_write_pending = false;
  g_pmd_write_done = false;
  g_pmd_write_att_status = ATT_ERROR_SUCCESS;
  g_pmd_cp_response_waiting = false;
  g_pmd_cp_response_done = false;
  g_pmd_cp_response_expected_opcode = 0u;
  g_pmd_cp_response_expected_type = 0xffu;
  g_pmd_cp_response_status = 0xffu;
  g_pmd_cp_response_type = 0xffu;
  g_mtu_exchange_done = false;
  g_battery_read_active = false;
  g_battery_value_received = false;
  g_battery_pending_reason = LOGGER_H10_BATTERY_REASON_NONE;

  state->start_in_progress = false;
  state->start_succeeded = false;
  state->streaming = false;
  state->packet_overflow = false;
  state->last_start_response_status = 0xffu;
  state->last_gatt_att_status = ATT_ERROR_SUCCESS;
  state->att_mtu = ATT_DEFAULT_MTU;
  state->start_deadline_mono_ms = 0u;
  state->battery_event_pending = false;
  state->battery_event_reason = LOGGER_H10_BATTERY_REASON_NONE;
  state->battery_read_due_connect = false;
  logger_h10_reset_packet_queue(state);
}

static void logger_h10_clear_link_state(logger_h10_state_t *state) {
  state->scanning = false;
  state->connect_intent = false;
  state->connected = false;
  state->disconnect_requested = false;
  state->encrypted = false;
  state->secure = false;
  state->pairing_requested = false;
  state->encryption_key_size = 0u;
  state->secure_deadline_mono_ms = 0u;
  state->connected_address[0] = '\0';
  g_conn_handle = HCI_CON_HANDLE_INVALID;
  g_peer_addr_type = BD_ADDR_TYPE_UNKNOWN;
  memset(g_peer_addr, 0, sizeof(g_peer_addr));
  logger_h10_reset_pmd_state(state);
}

static void logger_h10_set_phase(logger_h10_state_t *state,
                                 logger_h10_phase_t phase) {
  state->phase = phase;
}

static uint32_t logger_h10_next_reconnect_delay_ms(uint32_t now_ms) {
  uint32_t delay_ms = polar_sdk_connect_next_backoff_ms(
      &g_reconnect_policy, &g_reconnect_state, now_ms);
  if (delay_ms > 0u) {
    return delay_ms;
  }

  polar_sdk_connect_init(&g_reconnect_state, now_ms);
  delay_ms = polar_sdk_connect_next_backoff_ms(&g_reconnect_policy,
                                               &g_reconnect_state, now_ms);
  return delay_ms > 0u ? delay_ms : 1000u;
}

static void logger_h10_schedule_retry(logger_h10_state_t *state,
                                      uint32_t now_ms) {
  state->next_retry_mono_ms =
      now_ms + logger_h10_next_reconnect_delay_ms(now_ms);
  logger_h10_set_phase(state, LOGGER_H10_PHASE_WAITING);
}

static void logger_h10_stop_scan(logger_h10_state_t *state) {
  if (state->scanning) {
    gap_stop_scan();
    state->scanning = false;
  }
}

static void logger_h10_disconnect_for_restart(logger_h10_state_t *state) {
  state->start_in_progress = false;
  state->start_deadline_mono_ms = 0u;
  if (state->connected && g_conn_handle != HCI_CON_HANDLE_INVALID) {
    if (state->disconnect_requested) {
      return;
    }
    state->disconnect_requested = true;
    (void)gap_disconnect(g_conn_handle);
    return;
  }
  logger_h10_schedule_retry(state, btstack_run_loop_get_time_ms());
}

static bool logger_h10_queue_push_packet(logger_h10_state_t *state,
                                         logger_h10_stream_kind_t stream_kind,
                                         uint64_t mono_us, const uint8_t *value,
                                         uint16_t value_len) {
  /* The H10 packet queue is deliberately lock-free because both BTstack
   * callbacks and app drains are expected to run synchronously on core 0 in
   * pico_cyw43_arch_lwip_poll mode.  Fail fast if that invariant changes. */
  hard_assert(get_core_num() == 0u);
  hard_assert(__get_current_exception() == 0u);
  if (value == NULL || value_len == 0u ||
      value_len > LOGGER_H10_PACKET_MAX_BYTES ||
      (stream_kind != LOGGER_H10_STREAM_KIND_ECG &&
       stream_kind != LOGGER_H10_STREAM_KIND_ACC)) {
    logger_h10_note_packet_drop(state, stream_kind);
    return false;
  }
  if (state->packet_count >= LOGGER_H10_PACKET_QUEUE_DEPTH) {
    state->packet_overflow = true;
    logger_h10_note_packet_drop(state, stream_kind);
    return false;
  }

  logger_h10_packet_t *slot = &state->packets[state->packet_write_index];
  slot->stream_kind = (uint16_t)stream_kind;
  slot->mono_us = mono_us;
  slot->value_len = value_len;
  memcpy(slot->value, value, value_len);
  /* Index wraps in [0, QUEUE_DEPTH); fits uint8_t (QUEUE_DEPTH ≤ 255). */
  state->packet_write_index = (uint8_t)((state->packet_write_index + 1u) %
                                        LOGGER_H10_PACKET_QUEUE_DEPTH);
  state->packet_count += 1u;
  if (g_capture_stats != NULL) {
    logger_capture_stats_observe_queue_depth(g_capture_stats,
                                             state->packet_count);
  }
  return true;
}

bool logger_h10_pop_packet(logger_h10_state_t *state,
                           logger_h10_packet_t *out) {
  hard_assert(get_core_num() == 0u);
  hard_assert(__get_current_exception() == 0u);
  if (state == NULL || out == NULL || state->packet_count == 0u) {
    return false;
  }
  *out = state->packets[state->packet_read_index];
  memset(&state->packets[state->packet_read_index], 0,
         sizeof(state->packets[state->packet_read_index]));
  /* Index wraps in [0, QUEUE_DEPTH); fits uint8_t (QUEUE_DEPTH ≤ 255). */
  state->packet_read_index = (uint8_t)((state->packet_read_index + 1u) %
                                       LOGGER_H10_PACKET_QUEUE_DEPTH);
  state->packet_count = (uint8_t)(state->packet_count - 1u);
  return true;
}

bool logger_h10_take_battery_event(logger_h10_state_t *state,
                                   logger_h10_battery_event_t *out) {
  if (state == NULL || out == NULL || !state->battery_event_pending ||
      state->battery_percent < 0) {
    return false;
  }

  out->battery_percent = (uint8_t)state->battery_percent;
  out->read_reason =
      state->battery_event_reason == LOGGER_H10_BATTERY_REASON_CONNECT
          ? "connect"
          : "periodic";
  state->battery_event_pending = false;
  state->battery_event_reason = LOGGER_H10_BATTERY_REASON_NONE;
  return true;
}

bool logger_h10_take_recovery_event(logger_h10_state_t *state,
                                    logger_h10_recovery_event_t *out) {
  if (state == NULL || out == NULL ||
      state->recovery_event == LOGGER_H10_RECOVERY_EVENT_NONE) {
    return false;
  }

  *out = state->recovery_event;
  state->recovery_event = LOGGER_H10_RECOVERY_EVENT_NONE;
  return true;
}

bool logger_h10_debug_arm_stale_bond_injection(logger_h10_state_t *state,
                                               uint32_t now_ms,
                                               bool *restart_requested_out) {
  if (restart_requested_out != NULL) {
    *restart_requested_out = false;
  }
  if (state == NULL || !state->enabled || !state->target_address_valid) {
    return false;
  }

  state->debug_stale_bond_injection_armed = true;

  if (state->scanning) {
    logger_h10_stop_scan(state);
  }
  if (state->connect_intent) {
    state->connect_intent = false;
  }
  if (state->connected) {
    logger_h10_disconnect_for_restart(state);
    if (restart_requested_out != NULL) {
      *restart_requested_out = true;
    }
    return true;
  }
  if (state->controller_ready) {
    logger_h10_schedule_retry(state, now_ms);
    if (restart_requested_out != NULL) {
      *restart_requested_out = true;
    }
  }
  return true;
}

static void logger_h10_start_scan(logger_h10_state_t *state) {
  if (!state->enabled || !state->controller_ready ||
      !state->target_address_valid || state->scanning ||
      state->connect_intent || state->connected) {
    return;
  }

  gap_set_scan_parameters(1, LOGGER_H10_SCAN_INTERVAL_UNITS,
                          LOGGER_H10_SCAN_WINDOW_UNITS);
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

static void logger_h10_sleep_ms(uint32_t ms) {
  while (ms-- > 0u) {
    cyw43_arch_poll();
    sleep_ms(1u);
  }
}

static void logger_h10_refresh_link_security_state(logger_h10_state_t *state) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID) {
    return;
  }

  if (!state->encrypted) {
    state->encryption_key_size = 0u;
    state->secure = false;
    return;
  }

  state->encryption_key_size = polar_sdk_btstack_security_encryption_key_size(
      g_conn_handle, HCI_CON_HANDLE_INVALID);
  state->secure = state->encrypted && state->encryption_key_size > 0u;
}

static bool logger_h10_wait_flag_until_true(const volatile bool *flag,
                                            uint32_t timeout_ms) {
  uint32_t elapsed = 0u;
  while (elapsed < timeout_ms) {
    if (*flag) {
      return true;
    }
    if (g_h10 == NULL || !g_h10->connected ||
        g_conn_handle == HCI_CON_HANDLE_INVALID) {
      return false;
    }
    logger_h10_sleep_ms(1u);
    elapsed += 1u;
  }
  return *flag;
}

typedef struct {
  gatt_client_characteristic_t *chr;
  gatt_client_notification_t *notification;
  bool *listening;
} logger_h10_notify_ctx_t;

static bool logger_h10_notify_is_connected_ready(void *ctx) {
  (void)ctx;
  return g_h10 != NULL && g_h10->connected &&
         g_conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool logger_h10_notify_listener_active(void *ctx) {
  logger_h10_notify_ctx_t *notify_ctx = (logger_h10_notify_ctx_t *)ctx;
  return notify_ctx != NULL && notify_ctx->listening != NULL &&
         *notify_ctx->listening;
}

static void logger_h10_notify_start_listener(void *ctx) {
  logger_h10_notify_ctx_t *notify_ctx = (logger_h10_notify_ctx_t *)ctx;
  if (notify_ctx == NULL || notify_ctx->chr == NULL ||
      notify_ctx->notification == NULL || notify_ctx->listening == NULL) {
    return;
  }
  gatt_client_listen_for_characteristic_value_updates(
      notify_ctx->notification, logger_h10_gatt_packet_handler, g_conn_handle,
      notify_ctx->chr);
  *notify_ctx->listening = true;
}

static void logger_h10_notify_stop_listener(void *ctx) {
  logger_h10_notify_ctx_t *notify_ctx = (logger_h10_notify_ctx_t *)ctx;
  if (notify_ctx == NULL || notify_ctx->notification == NULL ||
      notify_ctx->listening == NULL) {
    return;
  }
  gatt_client_stop_listening_for_characteristic_value_updates(
      notify_ctx->notification);
  *notify_ctx->listening = false;
}

static int logger_h10_notify_write_ccc(void *ctx, uint16_t ccc_cfg) {
  logger_h10_notify_ctx_t *notify_ctx = (logger_h10_notify_ctx_t *)ctx;
  if (notify_ctx == NULL || notify_ctx->chr == NULL) {
    return -1;
  }
  g_pmd_cfg_pending = true;
  g_pmd_cfg_done = false;
  g_pmd_cfg_att_status = ATT_ERROR_SUCCESS;
  int err = gatt_client_write_client_characteristic_configuration(
      logger_h10_gatt_packet_handler, g_conn_handle, notify_ctx->chr, ccc_cfg);
  if (err != 0) {
    g_pmd_cfg_pending = false;
  }
  return err;
}

static bool logger_h10_notify_wait_complete(void *ctx, uint32_t timeout_ms,
                                            uint8_t *out_att_status) {
  (void)ctx;
  const bool done =
      logger_h10_wait_flag_until_true(&g_pmd_cfg_done, timeout_ms);
  if (out_att_status != NULL) {
    *out_att_status = g_pmd_cfg_att_status;
  }
  return done;
}

static int
logger_h10_set_notify_for_char_result(gatt_client_characteristic_t *chr,
                                      gatt_client_notification_t *notification,
                                      bool *listening, bool enable) {
  logger_h10_notify_ctx_t notify_ctx = {
      .chr = chr,
      .notification = notification,
      .listening = listening,
  };
  polar_sdk_gatt_notify_ops_t ops = {
      .ctx = &notify_ctx,
      .is_connected_ready = logger_h10_notify_is_connected_ready,
      .listener_active = logger_h10_notify_listener_active,
      .start_listener = logger_h10_notify_start_listener,
      .stop_listener = logger_h10_notify_stop_listener,
      .write_ccc = logger_h10_notify_write_ccc,
      .wait_complete = logger_h10_notify_wait_complete,
  };
  polar_sdk_gatt_notify_runtime_args_t args = {
      .ops = &ops,
      .has_value_handle = chr != NULL && chr->value_handle != 0u,
      .enable = enable,
      .properties = chr != NULL ? chr->properties : 0u,
      .prop_notify = ATT_PROPERTY_NOTIFY,
      .prop_indicate = ATT_PROPERTY_INDICATE,
      .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
      .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
      .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
      .att_success = ATT_ERROR_SUCCESS,
      .timeout_ms = 2000u,
      .cfg_pending = &g_pmd_cfg_pending,
      .cfg_done = &g_pmd_cfg_done,
  };

  polar_sdk_gatt_notify_runtime_result_t r =
      polar_sdk_gatt_notify_runtime_set(&args);
  return polar_sdk_pmd_map_notify_result(
      r, g_pmd_cfg_att_status, POLAR_SDK_PMD_OP_OK,
      POLAR_SDK_PMD_OP_NOT_CONNECTED, POLAR_SDK_PMD_OP_TIMEOUT,
      POLAR_SDK_PMD_OP_TRANSPORT);
}

static int logger_h10_enable_notifications_once(logger_h10_state_t *state) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID || !g_pmd_cp_found ||
      !g_pmd_data_found) {
    return POLAR_SDK_PMD_OP_NOT_CONNECTED;
  }

  int status = logger_h10_set_notify_for_char_result(
      &g_pmd_cp_char, &g_pmd_cp_listener, &g_pmd_cp_listener_registered, true);
  if (status != POLAR_SDK_PMD_OP_OK) {
    return status;
  }

  status = logger_h10_set_notify_for_char_result(
      &g_pmd_data_char, &g_pmd_data_listener, &g_pmd_data_listener_registered,
      true);
  if (status != POLAR_SDK_PMD_OP_OK) {
    return status;
  }
  return POLAR_SDK_PMD_OP_OK;
}

static bool logger_h10_pmd_is_connected(void *ctx) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  return state != NULL && state->connected &&
         g_conn_handle != HCI_CON_HANDLE_INVALID;
}

static bool logger_h10_pmd_security_ready(void *ctx) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  if (!logger_h10_pmd_is_connected(ctx)) {
    return false;
  }
  logger_h10_refresh_link_security_state(state);
  return state->secure;
}

static void logger_h10_pmd_security_sleep_ms(void *ctx, uint32_t ms) {
  (void)ctx;
  logger_h10_sleep_ms(ms);
}

static polar_sdk_security_result_t logger_h10_pmd_ensure_security(void *ctx) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  if (state == NULL || !logger_h10_pmd_is_connected(ctx)) {
    return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
  }

  polar_sdk_security_policy_t policy = {
      .rounds = LOGGER_H10_PMD_SECURITY_ROUNDS,
      .wait_ms_per_round = LOGGER_H10_PMD_SECURITY_WAIT_MS,
      .request_gap_ms = 120u,
      .poll_ms = 20u,
  };

  state->pairing_requested = true;
  polar_sdk_security_result_t result = polar_sdk_btstack_security_ensure(
      &g_conn_handle, HCI_CON_HANDLE_INVALID, &policy,
      logger_h10_pmd_security_sleep_ms, state);
  logger_h10_refresh_link_security_state(state);
  return result;
}

static int logger_h10_pmd_enable_notifications_cb(void *ctx) {
  return logger_h10_enable_notifications_once((logger_h10_state_t *)ctx);
}

static int logger_h10_mtu_read_cb(void *ctx, uint16_t *out_mtu) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID) {
    return -1;
  }

  uint16_t current_mtu = ATT_DEFAULT_MTU;
  if (gatt_client_get_mtu(g_conn_handle, &current_mtu) != ERROR_CODE_SUCCESS) {
    return -1;
  }
  state->att_mtu = current_mtu;
  if (out_mtu != NULL) {
    *out_mtu = current_mtu;
  }
  return 0;
}

static int logger_h10_mtu_request_exchange_cb(void *ctx) {
  (void)ctx;
  g_mtu_exchange_done = false;
  gatt_client_send_mtu_negotiation(logger_h10_gatt_packet_handler,
                                   g_conn_handle);
  return 0;
}

static bool logger_h10_mtu_wait_exchange_complete_cb(void *ctx,
                                                     uint32_t timeout_ms) {
  (void)ctx;
  return logger_h10_wait_flag_until_true(&g_mtu_exchange_done, timeout_ms);
}

static uint16_t logger_h10_mtu_current_cb(void *ctx) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  return state != NULL ? state->att_mtu : ATT_DEFAULT_MTU;
}

static int logger_h10_ensure_minimum_mtu_cb(void *ctx, uint16_t minimum_mtu) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID) {
    return POLAR_SDK_PMD_OP_NOT_CONNECTED;
  }

  uint16_t mtu_before = state->att_mtu;
  if (gatt_client_get_mtu(g_conn_handle, &mtu_before) == ERROR_CODE_SUCCESS) {
    state->att_mtu = mtu_before;
  }

  polar_sdk_gatt_mtu_ops_t mtu_ops = {
      .ctx = state,
      .is_connected = logger_h10_pmd_is_connected,
      .read_mtu = logger_h10_mtu_read_cb,
      .request_exchange = logger_h10_mtu_request_exchange_cb,
      .wait_exchange_complete = logger_h10_mtu_wait_exchange_complete_cb,
      .current_mtu = logger_h10_mtu_current_cb,
  };

  polar_sdk_gatt_mtu_result_t r = polar_sdk_gatt_mtu_ensure_minimum(
      &mtu_ops, minimum_mtu, 2000u, &state->att_mtu);
  if (r == POLAR_SDK_GATT_MTU_RESULT_OK) {
    return POLAR_SDK_PMD_OP_OK;
  }
  if (r == POLAR_SDK_GATT_MTU_RESULT_NOT_CONNECTED) {
    return POLAR_SDK_PMD_OP_NOT_CONNECTED;
  }
  if (r == POLAR_SDK_GATT_MTU_RESULT_TIMEOUT) {
    return POLAR_SDK_PMD_OP_TIMEOUT;
  }
  return POLAR_SDK_PMD_OP_TRANSPORT;
}

static int logger_h10_write_cp_command_and_wait_response(
    logger_h10_state_t *state, const uint8_t *cmd, size_t cmd_len,
    uint8_t expected_opcode, uint8_t expected_type, uint8_t *out_status) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID) {
    return POLAR_SDK_PMD_OP_NOT_CONNECTED;
  }
  if (cmd == NULL || cmd_len < 2u || cmd_len > UINT16_MAX) {
    return POLAR_SDK_PMD_OP_TRANSPORT;
  }

  g_pmd_cp_response_waiting = true;
  g_pmd_cp_response_done = false;
  g_pmd_cp_response_expected_opcode = expected_opcode;
  g_pmd_cp_response_expected_type = expected_type;
  g_pmd_cp_response_status = 0xffu;
  g_pmd_cp_response_type = 0xffu;

  g_pmd_write_pending = true;
  g_pmd_write_done = false;
  g_pmd_write_att_status = ATT_ERROR_SUCCESS;

  int err = gatt_client_write_value_of_characteristic(
      logger_h10_gatt_packet_handler, g_conn_handle, g_pmd_cp_char.value_handle,
      (uint16_t)cmd_len, (uint8_t *)cmd);
  if (err != 0) {
    g_pmd_write_pending = false;
    return POLAR_SDK_PMD_OP_TRANSPORT;
  }

  if (!logger_h10_wait_flag_until_true(&g_pmd_write_done, 2000u)) {
    g_pmd_write_pending = false;
    return POLAR_SDK_PMD_OP_TIMEOUT;
  }
  if (g_pmd_write_att_status != ATT_ERROR_SUCCESS) {
    return (int)g_pmd_write_att_status;
  }

  if (!logger_h10_wait_flag_until_true(&g_pmd_cp_response_done, 2000u)) {
    g_pmd_cp_response_waiting = false;
    return POLAR_SDK_PMD_OP_TIMEOUT;
  }
  if (out_status != NULL) {
    *out_status = g_pmd_cp_response_status;
  }
  return POLAR_SDK_PMD_OP_OK;
}

static int logger_h10_start_ecg_and_wait_response_cb(void *ctx,
                                                     const uint8_t *start_cmd,
                                                     size_t start_cmd_len,
                                                     uint8_t *out_status) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  if (start_cmd == NULL || start_cmd_len < 2u) {
    return POLAR_SDK_PMD_OP_TRANSPORT;
  }
  return logger_h10_write_cp_command_and_wait_response(
      state, start_cmd, start_cmd_len, POLAR_SDK_PMD_OPCODE_START_MEASUREMENT,
      start_cmd[1], out_status);
}

static bool logger_h10_start_service_discovery(logger_h10_state_t *state) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID || g_service_query_active) {
    return false;
  }
  g_service_query_active = true;
  state->last_gatt_att_status = ATT_ERROR_SUCCESS;
  const int err = gatt_client_discover_primary_services(
      logger_h10_gatt_packet_handler, g_conn_handle);
  if (err != 0) {
    g_service_query_active = false;
    state->last_gatt_att_status = (uint8_t)err;
    return false;
  }
  return true;
}

static bool
logger_h10_start_characteristic_discovery(logger_h10_state_t *state) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID || g_char_query_active ||
      !g_pmd_service_found) {
    return false;
  }
  g_char_query_active = true;
  state->last_gatt_att_status = ATT_ERROR_SUCCESS;
  const int err = gatt_client_discover_characteristics_for_service(
      logger_h10_gatt_packet_handler, g_conn_handle, &g_pmd_service);
  if (err != 0) {
    g_char_query_active = false;
    state->last_gatt_att_status = (uint8_t)err;
    return false;
  }
  return true;
}

static bool logger_h10_request_battery_read(logger_h10_state_t *state,
                                            uint8_t reason) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID || g_battery_read_active ||
      g_service_query_active || g_char_query_active ||
      state->start_in_progress || g_pmd_cfg_pending || g_pmd_write_pending) {
    return false;
  }

  g_battery_read_active = true;
  g_battery_value_received = false;
  g_battery_pending_reason = reason;
  state->last_gatt_att_status = ATT_ERROR_SUCCESS;
  const int err = gatt_client_read_value_of_characteristics_by_uuid16(
      logger_h10_gatt_packet_handler, g_conn_handle, 0x0001u, 0xffffu,
      ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL);
  if (err != 0) {
    g_battery_read_active = false;
    g_battery_pending_reason = LOGGER_H10_BATTERY_REASON_NONE;
    state->last_gatt_att_status = (uint8_t)err;
    return false;
  }
  return true;
}

static void logger_h10_maybe_schedule_battery_read(logger_h10_state_t *state,
                                                   uint32_t now_ms) {
  if (state == NULL || !state->connected ||
      g_conn_handle == HCI_CON_HANDLE_INVALID || !state->secure) {
    return;
  }
  if (state->battery_read_due_connect) {
    if (logger_h10_request_battery_read(state,
                                        LOGGER_H10_BATTERY_REASON_CONNECT)) {
      state->battery_read_due_connect = false;
    }
    return;
  }
  if (state->last_battery_read_mono_ms != 0u &&
      (now_ms - state->last_battery_read_mono_ms) <
          LOGGER_H10_BATTERY_PERIOD_MS) {
    return;
  }
  (void)logger_h10_request_battery_read(state,
                                        LOGGER_H10_BATTERY_REASON_PERIODIC);
}

static bool logger_h10_start_measurement(logger_h10_state_t *state,
                                         logger_h10_stream_kind_t stream_kind) {
  if (state == NULL || !state->connected || !state->secure || !g_pmd_cp_found ||
      !g_pmd_data_found) {
    return false;
  }

  const bool is_acc = stream_kind == LOGGER_H10_STREAM_KIND_ACC;
  if (stream_kind != LOGGER_H10_STREAM_KIND_ECG && !is_acc) {
    return false;
  }

  state->last_start_response_status = 0xffu;
  if (is_acc) {
    state->acc_start_attempt_count += 1u;
  } else {
    state->ecg_start_attempt_count += 1u;
  }

  polar_sdk_pmd_start_policy_t policy = {
      .ccc_attempts = LOGGER_H10_PMD_CCC_ATTEMPTS,
      .minimum_mtu = LOGGER_H10_PMD_MIN_MTU,
      .sample_rate =
          is_acc ? LOGGER_H10_ACC_SAMPLE_RATE : LOGGER_H10_ECG_SAMPLE_RATE,
      .include_resolution = true,
      .resolution =
          is_acc ? LOGGER_H10_ACC_RESOLUTION : LOGGER_H10_ECG_RESOLUTION,
      .include_range = is_acc,
      .range = is_acc ? LOGGER_H10_ACC_RANGE : 0u,
  };
  polar_sdk_pmd_start_ops_t ops = {
      .ctx = state,
      .is_connected = logger_h10_pmd_is_connected,
      .security_ready = logger_h10_pmd_security_ready,
      .ensure_security = logger_h10_pmd_ensure_security,
      .enable_notifications = logger_h10_pmd_enable_notifications_cb,
      .ensure_minimum_mtu = logger_h10_ensure_minimum_mtu_cb,
      .start_ecg_and_wait_response = logger_h10_start_ecg_and_wait_response_cb,
  };

  uint8_t response_status = 0xffu;
  int last_ccc_att_status = 0;
  const polar_sdk_pmd_start_result_t result =
      is_acc
          ? polar_sdk_pmd_start_acc_with_policy(&policy, &ops, &response_status,
                                                &last_ccc_att_status)
          : polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, &response_status,
                                                &last_ccc_att_status);

  state->last_start_response_status = response_status;
  if (last_ccc_att_status > 0 && last_ccc_att_status <= UINT8_MAX) {
    state->last_gatt_att_status = (uint8_t)last_ccc_att_status;
  }

  if (result != POLAR_SDK_PMD_START_RESULT_OK) {
    if (result == POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT ||
        (result == POLAR_SDK_PMD_START_RESULT_CCC_REJECTED &&
         polar_sdk_pmd_att_status_requires_security(
             state->last_gatt_att_status))) {
      logger_h10_note_security_failure(
          state,
          result == POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT
              ? LOGGER_H10_SECURITY_FAILURE_SECURE_TIMEOUT
              : LOGGER_H10_SECURITY_FAILURE_PMD_AUTH,
          btstack_run_loop_get_time_ms());
    }
    printf("[logger] h10 %s start failed result=%d att=0x%02x resp=0x%02x\n",
           is_acc ? "acc" : "ecg", (int)result,
           (unsigned)state->last_gatt_att_status,
           (unsigned)state->last_start_response_status);
    return false;
  }

  if (is_acc) {
    state->acc_start_success_count += 1u;
  } else {
    state->ecg_start_success_count += 1u;
  }
  printf("[logger] h10 %s start ok mtu=%u\n", is_acc ? "acc" : "ecg",
         (unsigned)state->att_mtu);
  return true;
}

static bool logger_h10_start_streams(logger_h10_state_t *state,
                                     uint32_t now_ms) {
  if (state == NULL) {
    return false;
  }

  state->start_in_progress = true;
  state->start_succeeded = false;
  state->last_start_response_status = 0xffu;

  const bool ecg_ok =
      logger_h10_start_measurement(state, LOGGER_H10_STREAM_KIND_ECG);
  const bool acc_ok =
      ecg_ok && logger_h10_start_measurement(state, LOGGER_H10_STREAM_KIND_ACC);
  state->start_in_progress = false;

  if (!ecg_ok || !acc_ok) {
    return false;
  }

  state->start_succeeded = true;
  state->start_deadline_mono_ms = now_ms + LOGGER_H10_STREAM_START_TIMEOUT_MS;
  logger_h10_note_secure_start_success(state);
  logger_h10_set_phase(state, LOGGER_H10_PHASE_STARTING);
  printf("[logger] h10 ecg+acc start ok mtu=%u\n", (unsigned)state->att_mtu);
  return true;
}

static void logger_h10_on_connected_ready(void *ctx,
                                          const polar_sdk_link_event_t *event) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  state->connected = true;
  state->disconnect_requested = false;
  state->connect_intent = false;
  state->scanning = false;
  state->connect_count += 1u;
  g_conn_handle = event->handle;
  polar_sdk_connect_init(&g_reconnect_state, btstack_run_loop_get_time_ms());
  if (g_peer_addr_type != BD_ADDR_TYPE_UNKNOWN) {
    logger_copy_string(state->connected_address,
                       sizeof(state->connected_address),
                       bd_addr_to_str(g_peer_addr));
  }
  logger_h10_reset_pmd_state(state);
  state->battery_read_due_connect = true;
  state->secure_deadline_mono_ms =
      btstack_run_loop_get_time_ms() + LOGGER_H10_SECURING_TIMEOUT_MS;
  logger_h10_set_phase(state, LOGGER_H10_PHASE_SECURING);
}

static void logger_h10_on_disconnected(void *ctx,
                                       const polar_sdk_link_event_t *event) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  state->disconnect_count += 1u;
  state->last_disconnect_reason = event->reason;
  logger_h10_clear_link_state(state);
  if (state->enabled && state->controller_ready &&
      state->target_address_valid) {
    logger_h10_schedule_retry(state, btstack_run_loop_get_time_ms());
  } else {
    logger_h10_set_phase(state, state->enabled ? LOGGER_H10_PHASE_WAITING
                                               : LOGGER_H10_PHASE_OFF);
  }
}

static void
logger_h10_on_conn_update_complete(void *ctx,
                                   const polar_sdk_link_event_t *event) {
  (void)ctx;
  (void)event;
}

static bool logger_h10_adv_runtime_is_scanning(void *ctx) {
  const logger_h10_state_t *state = (const logger_h10_state_t *)ctx;
  return state != NULL && state->scanning;
}

static void
logger_h10_adv_runtime_on_match(void *ctx,
                                const polar_sdk_btstack_adv_report_t *report) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  memcpy(g_peer_addr, report->addr, sizeof(g_peer_addr));
  g_peer_addr_type = report->addr_type;
  state->seen_bound_device = true;
  state->seen_count += 1u;
  state->last_seen_rssi = report->rssi;
  state->last_seen_mono_ms = btstack_run_loop_get_time_ms();
  logger_copy_string(state->last_seen_address, sizeof(state->last_seen_address),
                     bd_addr_to_str(g_peer_addr));
  state->scanning = false;
  state->connect_intent = true;
  logger_h10_set_phase(state, LOGGER_H10_PHASE_CONNECTING);
}

static int logger_h10_adv_runtime_stop_scan(void *ctx) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  logger_h10_stop_scan(state);
  return ERROR_CODE_SUCCESS;
}

static int logger_h10_adv_runtime_connect(void *ctx, const uint8_t *addr,
                                          uint8_t addr_type) {
  (void)ctx;
  bd_addr_t target;
  memcpy(target, addr, sizeof(target));
  return gap_connect(target, addr_type);
}

static void logger_h10_dispatch_on_adv_report(
    void *ctx, const polar_sdk_btstack_adv_report_t *adv_report) {
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
      &g_runtime_link, &filter, adv_report, ERROR_CODE_SUCCESS, &ops);
}

static void
logger_h10_dispatch_on_link_event(void *ctx,
                                  const polar_sdk_link_event_t *link_event) {
  logger_h10_state_t *state = (logger_h10_state_t *)ctx;
  polar_sdk_runtime_context_link_ops_t ops = {
      .ctx = state,
      .on_connected_ready = logger_h10_on_connected_ready,
      .on_disconnected = logger_h10_on_disconnected,
      .on_conn_update_complete = logger_h10_on_conn_update_complete,
  };
  const bool handled = polar_sdk_runtime_context_handle_link_event(
      &g_runtime_link, HCI_CON_HANDLE_INVALID, link_event,
      g_user_disconnect_requested, state->enabled, &ops);
  if (link_event->type == POLAR_SDK_LINK_EVENT_CONN_COMPLETE && handled &&
      link_event->status != ERROR_CODE_SUCCESS) {
    state->connect_intent = false;
    logger_h10_schedule_retry(state, btstack_run_loop_get_time_ms());
  }
  if (link_event->type == POLAR_SDK_LINK_EVENT_DISCONNECT) {
    g_user_disconnect_requested = false;
  }
}

static void
logger_h10_dispatch_on_sm_event(void *ctx,
                                const polar_sdk_sm_event_t *sm_event) {
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
      return;
    }
    logger_h10_note_security_failure(state,
                                     LOGGER_H10_SECURITY_FAILURE_PAIRING_FAILED,
                                     btstack_run_loop_get_time_ms());
    logger_h10_disconnect_for_restart(state);
  }
}

static void logger_h10_hci_packet_handler(uint8_t packet_type, uint16_t channel,
                                          uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;

  if (packet_type != HCI_EVENT_PACKET || g_h10 == NULL) {
    return;
  }

  polar_sdk_btstack_dispatch_ops_t dispatch_ops = {
      .ctx = g_h10,
      .on_adv_report = logger_h10_dispatch_on_adv_report,
      .on_link_event = logger_h10_dispatch_on_link_event,
      .on_sm_event = NULL,
  };
  if (polar_sdk_btstack_dispatch_event(packet_type, packet, &dispatch_ops)) {
    return;
  }

  switch (hci_event_packet_get_type(packet)) {
  case BTSTACK_EVENT_STATE: {
    const uint8_t btstack_state = btstack_event_state_get_state(packet);
    g_h10->controller_ready = btstack_state == HCI_STATE_WORKING;
    if (btstack_state == HCI_STATE_WORKING) {
      polar_sdk_connect_init(&g_reconnect_state,
                             btstack_run_loop_get_time_ms());
      logger_h10_set_phase(g_h10, LOGGER_H10_PHASE_WAITING);
    } else if (!g_h10->enabled) {
      logger_h10_set_phase(g_h10, LOGGER_H10_PHASE_OFF);
    }
    break;
  }

  case HCI_EVENT_ENCRYPTION_CHANGE: {
    if (hci_event_encryption_change_get_connection_handle(packet) !=
        g_conn_handle) {
      break;
    }
    const bool encrypted =
        hci_event_encryption_change_get_encryption_enabled(packet) != 0u;
    g_h10->encrypted = encrypted;
    logger_h10_refresh_link_security_state(g_h10);
    g_h10->bonded = g_h10->bonded || g_h10->secure;
    if (g_h10->secure) {
      g_h10->secure_deadline_mono_ms = 0u;
    }
    logger_h10_set_phase(g_h10, g_h10->secure ? LOGGER_H10_PHASE_STARTING
                                              : LOGGER_H10_PHASE_SECURING);
    break;
  }

  case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
    if (hci_event_encryption_change_v2_get_connection_handle(packet) !=
        g_conn_handle) {
      break;
    }
    const bool encrypted =
        hci_event_encryption_change_v2_get_encryption_enabled(packet) != 0u;
    g_h10->encrypted = encrypted;
    logger_h10_refresh_link_security_state(g_h10);
    g_h10->bonded = g_h10->bonded || g_h10->secure;
    if (g_h10->secure) {
      g_h10->secure_deadline_mono_ms = 0u;
    }
    logger_h10_set_phase(g_h10, g_h10->secure ? LOGGER_H10_PHASE_STARTING
                                              : LOGGER_H10_PHASE_SECURING);
    break;
  }

  default:
    break;
  }
}

static void logger_h10_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size) {
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

static void logger_h10_gatt_packet_handler(uint8_t packet_type,
                                           uint16_t channel, uint8_t *packet,
                                           uint16_t size) {
  (void)channel;
  (void)size;

  if (g_h10 == NULL) {
    return;
  }

  polar_sdk_btstack_mtu_event_t mtu_event;
  if (polar_sdk_btstack_decode_mtu_event(packet_type, packet, &mtu_event)) {
    g_h10->att_mtu = mtu_event.mtu;
    g_mtu_exchange_done = true;
    return;
  }

  uint8_t query_complete_att_status = ATT_ERROR_SUCCESS;
  const bool have_query_complete =
      polar_sdk_btstack_decode_query_complete_att_status(
          packet_type, packet, &query_complete_att_status);

  if (have_query_complete && g_pmd_cfg_pending) {
    g_pmd_cfg_att_status = query_complete_att_status;
    g_pmd_cfg_pending = false;
    g_pmd_cfg_done = true;
    return;
  }

  if (have_query_complete && g_pmd_write_pending) {
    g_pmd_write_att_status = query_complete_att_status;
    g_pmd_write_pending = false;
    g_pmd_write_done = true;
    return;
  }

  if (packet_type == HCI_EVENT_PACKET &&
      hci_event_packet_get_type(packet) ==
          GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT &&
      g_battery_read_active) {
    const uint16_t value_len =
        gatt_event_characteristic_value_query_result_get_value_length(packet);
    const uint8_t *value =
        gatt_event_characteristic_value_query_result_get_value(packet);
    if (value_len >= 1u && value != NULL) {
      g_h10->battery_percent = value[0];
      g_h10->last_battery_read_mono_ms = btstack_run_loop_get_time_ms();
      g_h10->battery_read_count += 1u;
      g_h10->battery_event_pending = true;
      g_h10->battery_event_reason = g_battery_pending_reason;
      g_battery_value_received = true;
    }
    return;
  }

  if (have_query_complete && g_battery_read_active) {
    g_h10->last_gatt_att_status = query_complete_att_status;
    g_battery_read_active = false;
    g_battery_value_received = false;
    g_battery_pending_reason = LOGGER_H10_BATTERY_REASON_NONE;
    return;
  }

  if (g_service_query_active) {
    gatt_client_service_t service;
    if (polar_sdk_btstack_decode_service_query_result(packet_type, packet,
                                                      &service)) {
      const polar_sdk_disc_service_kind_t service_kind =
          polar_sdk_btstack_classify_service(
              service.uuid16, service.uuid128, ORG_BLUETOOTH_SERVICE_HEART_RATE,
              0u, LOGGER_H10_UUID_PMD_SERVICE_BE);
      if (service_kind == POLAR_SDK_DISC_SERVICE_PMD) {
        g_pmd_service = service;
        polar_sdk_discovery_apply_service_kind(POLAR_SDK_DISC_SERVICE_PMD, NULL,
                                               &g_pmd_service_found, NULL);
      }
      return;
    }

    if (have_query_complete) {
      g_service_query_active = false;
      g_h10->last_gatt_att_status = query_complete_att_status;
      if (query_complete_att_status != ATT_ERROR_SUCCESS ||
          !g_pmd_service_found) {
        logger_h10_disconnect_for_restart(g_h10);
      }
      return;
    }
  }

  if (g_char_query_active) {
    gatt_client_characteristic_t chr;
    if (polar_sdk_btstack_decode_characteristic_query_result(packet_type,
                                                             packet, &chr)) {
      const polar_sdk_disc_char_kind_t char_kind =
          polar_sdk_btstack_classify_char(
              POLAR_SDK_DISC_STAGE_PMD_CHARS, chr.uuid16, chr.uuid128,
              ORG_BLUETOOTH_CHARACTERISTIC_HEART_RATE_MEASUREMENT,
              LOGGER_H10_UUID_PMD_CP_BE, LOGGER_H10_UUID_PMD_DATA_BE, NULL,
              NULL, NULL);
      if (char_kind == POLAR_SDK_DISC_CHAR_PMD_CP) {
        g_pmd_cp_char = chr;
      } else if (char_kind == POLAR_SDK_DISC_CHAR_PMD_DATA) {
        g_pmd_data_char = chr;
      }
      polar_sdk_discovery_apply_char_kind(
          char_kind, chr.value_handle, NULL, NULL, &g_pmd_cp_found, NULL,
          &g_pmd_data_found, NULL, NULL, NULL, NULL);
      return;
    }

    if (have_query_complete) {
      g_char_query_active = false;
      g_h10->last_gatt_att_status = query_complete_att_status;
      if (query_complete_att_status != ATT_ERROR_SUCCESS || !g_pmd_cp_found ||
          !g_pmd_data_found) {
        logger_h10_disconnect_for_restart(g_h10);
      }
      return;
    }
  }

  polar_sdk_btstack_value_event_t value_event;
  if (!polar_sdk_btstack_decode_value_event(packet_type, packet,
                                            &value_event) ||
      g_conn_handle == HCI_CON_HANDLE_INVALID) {
    return;
  }

  if (value_event.value_handle == g_pmd_cp_char.value_handle) {
    polar_sdk_pmd_cp_response_t response;
    if (!polar_sdk_pmd_parse_cp_response(value_event.value,
                                         value_event.value_len, &response)) {
      return;
    }
    if (g_pmd_cp_response_waiting &&
        response.opcode == g_pmd_cp_response_expected_opcode &&
        (g_pmd_cp_response_expected_type == 0xffu ||
         response.measurement_type == g_pmd_cp_response_expected_type)) {
      g_pmd_cp_response_type = response.measurement_type;
      g_pmd_cp_response_status = response.status;
      g_pmd_cp_response_waiting = false;
      g_pmd_cp_response_done = true;
    }
    return;
  }

  if (value_event.value_handle != g_pmd_data_char.value_handle ||
      value_event.value_len == 0u) {
    return;
  }

  logger_h10_stream_kind_t stream_kind;
  if (value_event.value[0] == POLAR_SDK_PMD_MEASUREMENT_ECG) {
    stream_kind = LOGGER_H10_STREAM_KIND_ECG;
  } else if (value_event.value[0] == POLAR_SDK_PMD_MEASUREMENT_ACC) {
    stream_kind = LOGGER_H10_STREAM_KIND_ACC;
  } else {
    return;
  }

  if (logger_h10_queue_push_packet(g_h10, stream_kind, time_us_64(),
                                   value_event.value, value_event.value_len)) {
    if (stream_kind == LOGGER_H10_STREAM_KIND_ACC) {
      g_h10->acc_packet_count += 1u;
    } else {
      g_h10->ecg_packet_count += 1u;
    }
    g_h10->streaming = true;
    g_h10->start_in_progress = false;
    g_h10->start_succeeded = true;
    g_h10->start_deadline_mono_ms = 0u;
    logger_h10_set_phase(g_h10, LOGGER_H10_PHASE_STREAMING);
  }
}

static void logger_h10_init_btstack_core(void) {
  if (g_btstack_core_initialized) {
    return;
  }

  l2cap_init();
  sm_init();
  gatt_client_init();
  polar_sdk_btstack_sm_configure_default_central_policy();

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
  state->battery_percent = -1;
  state->phase = LOGGER_H10_PHASE_OFF;
  state->last_start_response_status = 0xffu;
  state->att_mtu = ATT_DEFAULT_MTU;
  state->initialized = true;
  g_h10 = state;
  logger_h10_init_btstack_core();
}

void logger_h10_set_capture_stats(logger_h10_state_t *state,
                                  logger_capture_stats_t *stats) {
  (void)state;
  g_capture_stats = stats;
}

bool logger_h10_set_bound_address(logger_h10_state_t *state,
                                  const char *bound_address) {
  logger_copy_string(state->bound_address, sizeof(state->bound_address),
                     bound_address);
  if (bound_address == NULL || bound_address[0] == '\0') {
    state->target_address_valid = false;
    memset(g_target_addr, 0, sizeof(g_target_addr));
    return true;
  }

  state->target_address_valid =
      sscanf_bd_addr(bound_address, g_target_addr) != 0;
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
    logger_h10_reset_security_recovery_cycle(state);
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
    if (state->disconnect_requested) {
      return;
    }
    logger_h10_refresh_link_security_state(state);

    if (state->debug_stale_bond_injection_armed && !state->secure) {
      if (logger_h10_force_stale_bond_injection(state, now_ms)) {
        return;
      }
    }

    if (state->streaming) {
      logger_h10_maybe_schedule_battery_read(state, now_ms);
      logger_h10_set_phase(state, LOGGER_H10_PHASE_STREAMING);
      return;
    }

    if (!state->secure) {
      logger_h10_set_phase(state, LOGGER_H10_PHASE_SECURING);
      if (state->secure_deadline_mono_ms == 0u) {
        state->secure_deadline_mono_ms =
            now_ms + LOGGER_H10_SECURING_TIMEOUT_MS;
      }
      if (logger_h10_deadline_reached(now_ms, state->secure_deadline_mono_ms)) {
        logger_h10_note_security_failure(
            state, LOGGER_H10_SECURITY_FAILURE_SECURE_TIMEOUT, now_ms);
        logger_h10_disconnect_for_restart(state);
        return;
      }
      if (!state->pairing_requested &&
          g_conn_handle != HCI_CON_HANDLE_INVALID) {
        polar_sdk_btstack_security_request_pairing(g_conn_handle,
                                                   HCI_CON_HANDLE_INVALID);
        state->pairing_requested = true;
      }
      return;
    }

    state->secure_deadline_mono_ms = 0u;

    logger_h10_set_phase(state, LOGGER_H10_PHASE_STARTING);
    if (g_service_query_active || g_char_query_active ||
        state->start_in_progress) {
      return;
    }

    if (state->start_succeeded) {
      logger_h10_maybe_schedule_battery_read(state, now_ms);
      if (state->start_deadline_mono_ms != 0u &&
          (int32_t)(now_ms - state->start_deadline_mono_ms) >= 0) {
        printf("[logger] h10 pmd start timed out waiting for packets\n");
        logger_h10_disconnect_for_restart(state);
      }
      return;
    }

    if (!g_pmd_service_found) {
      if (!logger_h10_start_service_discovery(state)) {
        logger_h10_disconnect_for_restart(state);
      }
      return;
    }
    if (!g_pmd_cp_found || !g_pmd_data_found) {
      if (!logger_h10_start_characteristic_discovery(state)) {
        logger_h10_disconnect_for_restart(state);
      }
      return;
    }
    if (!logger_h10_start_streams(state, now_ms)) {
      logger_h10_disconnect_for_restart(state);
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
  if (state->next_retry_mono_ms != 0u &&
      (int32_t)(now_ms - state->next_retry_mono_ms) < 0) {
    return;
  }
  logger_h10_start_scan(state);
}
