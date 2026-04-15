#ifndef LOGGER_FIRMWARE_H10_H
#define LOGGER_FIRMWARE_H10_H

#include <stdbool.h>
#include <stdint.h>

#define LOGGER_H10_ADDR_STR_LEN 17
#define LOGGER_H10_PACKET_MAX_BYTES 244
#define LOGGER_H10_PACKET_QUEUE_DEPTH 32

typedef enum {
  LOGGER_H10_STREAM_KIND_ECG = 1,
  LOGGER_H10_STREAM_KIND_ACC = 2,
} logger_h10_stream_kind_t;

typedef enum {
  LOGGER_H10_PHASE_OFF = 0,
  LOGGER_H10_PHASE_WAITING,
  LOGGER_H10_PHASE_SCANNING,
  LOGGER_H10_PHASE_CONNECTING,
  LOGGER_H10_PHASE_SECURING,
  LOGGER_H10_PHASE_STARTING,
  LOGGER_H10_PHASE_STREAMING,
} logger_h10_phase_t;

typedef enum {
  LOGGER_H10_SECURITY_FAILURE_NONE = 0,
  LOGGER_H10_SECURITY_FAILURE_PAIRING_FAILED,
  LOGGER_H10_SECURITY_FAILURE_SECURE_TIMEOUT,
  LOGGER_H10_SECURITY_FAILURE_PMD_AUTH,
} logger_h10_security_failure_t;

typedef enum {
  LOGGER_H10_RECOVERY_EVENT_NONE = 0,
  LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_CLEARED,
  LOGGER_H10_RECOVERY_EVENT_BOND_AUTO_REPAIRED,
} logger_h10_recovery_event_t;

typedef struct {
  uint16_t stream_kind;
  uint64_t mono_us;
  uint16_t value_len;
  uint8_t value[LOGGER_H10_PACKET_MAX_BYTES];
} logger_h10_packet_t;

typedef struct {
  uint8_t battery_percent;
  const char *read_reason;
} logger_h10_battery_event_t;

typedef struct {
  bool initialized;
  bool enabled;
  bool controller_ready;
  bool target_address_valid;
  bool scanning;
  bool connect_intent;
  bool connected;
  bool disconnect_requested;
  bool encrypted;
  bool secure;
  bool bonded;
  bool pairing_requested;
  bool start_in_progress;
  bool start_succeeded;
  bool streaming;
  bool packet_overflow;
  bool seen_bound_device;
  bool bond_repair_in_progress;
  bool debug_stale_bond_injection_armed;
  logger_h10_phase_t phase;
  logger_h10_security_failure_t last_security_failure;
  logger_h10_recovery_event_t recovery_event;
  char bound_address[LOGGER_H10_ADDR_STR_LEN + 1];
  char connected_address[LOGGER_H10_ADDR_STR_LEN + 1];
  char last_seen_address[LOGGER_H10_ADDR_STR_LEN + 1];
  int8_t last_seen_rssi;
  uint8_t encryption_key_size;
  uint8_t last_pairing_status;
  uint8_t last_pairing_reason;
  uint8_t last_disconnect_reason;
  uint8_t last_start_response_status;
  uint8_t last_gatt_att_status;
  int16_t battery_percent;
  uint16_t att_mtu;
  uint32_t last_seen_mono_ms;
  uint32_t next_retry_mono_ms;
  uint32_t secure_deadline_mono_ms;
  uint32_t start_deadline_mono_ms;
  uint32_t last_battery_read_mono_ms;
  uint32_t bond_clear_cooldown_until_mono_ms;
  uint32_t seen_count;
  uint32_t connect_count;
  uint32_t disconnect_count;
  uint32_t security_failure_count;
  uint32_t bond_auto_clear_count;
  uint32_t bond_auto_repair_count;
  uint32_t battery_read_count;
  uint32_t ecg_start_attempt_count;
  uint32_t ecg_start_success_count;
  uint32_t ecg_packet_count;
  uint32_t ecg_packet_drop_count;
  uint32_t acc_start_attempt_count;
  uint32_t acc_start_success_count;
  uint32_t acc_packet_count;
  uint32_t acc_packet_drop_count;
  uint8_t packet_read_index;
  uint8_t packet_write_index;
  uint8_t packet_count;
  uint8_t battery_event_reason;
  bool battery_event_pending;
  bool battery_read_due_connect;
  logger_h10_packet_t packets[LOGGER_H10_PACKET_QUEUE_DEPTH];
} logger_h10_state_t;

void logger_h10_init(logger_h10_state_t *state);
bool logger_h10_set_bound_address(logger_h10_state_t *state,
                                  const char *bound_address);
void logger_h10_set_enabled(logger_h10_state_t *state, bool enabled);
void logger_h10_poll(logger_h10_state_t *state, uint32_t now_ms);
bool logger_h10_pop_packet(logger_h10_state_t *state, logger_h10_packet_t *out);
bool logger_h10_take_battery_event(logger_h10_state_t *state,
                                   logger_h10_battery_event_t *out);
bool logger_h10_take_recovery_event(logger_h10_state_t *state,
                                    logger_h10_recovery_event_t *out);
bool logger_h10_debug_arm_stale_bond_injection(logger_h10_state_t *state,
                                               uint32_t now_ms,
                                               bool *restart_requested_out);

const char *logger_h10_phase_name(logger_h10_phase_t phase);
const char *
logger_h10_security_failure_name(logger_h10_security_failure_t failure);
const char *logger_h10_recovery_event_name(logger_h10_recovery_event_t event);

#endif