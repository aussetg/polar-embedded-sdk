#ifndef LOGGER_FIRMWARE_H10_H
#define LOGGER_FIRMWARE_H10_H

#include <stdbool.h>
#include <stdint.h>

#define LOGGER_H10_ADDR_STR_LEN 17

typedef enum {
    LOGGER_H10_PHASE_OFF = 0,
    LOGGER_H10_PHASE_WAITING,
    LOGGER_H10_PHASE_SCANNING,
    LOGGER_H10_PHASE_CONNECTING,
    LOGGER_H10_PHASE_SECURING,
    LOGGER_H10_PHASE_READY,
} logger_h10_phase_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool controller_ready;
    bool target_address_valid;
    bool scanning;
    bool connect_intent;
    bool connected;
    bool encrypted;
    bool secure;
    bool bonded;
    bool pairing_requested;
    bool seen_bound_device;
    logger_h10_phase_t phase;
    char bound_address[LOGGER_H10_ADDR_STR_LEN + 1];
    char connected_address[LOGGER_H10_ADDR_STR_LEN + 1];
    char last_seen_address[LOGGER_H10_ADDR_STR_LEN + 1];
    int8_t last_seen_rssi;
    uint8_t encryption_key_size;
    uint8_t last_pairing_status;
    uint8_t last_pairing_reason;
    uint8_t last_disconnect_reason;
    uint32_t last_seen_mono_ms;
    uint32_t next_retry_mono_ms;
    uint32_t seen_count;
    uint32_t connect_count;
    uint32_t disconnect_count;
} logger_h10_state_t;

void logger_h10_init(logger_h10_state_t *state);
bool logger_h10_set_bound_address(logger_h10_state_t *state, const char *bound_address);
void logger_h10_set_enabled(logger_h10_state_t *state, bool enabled);
void logger_h10_poll(logger_h10_state_t *state, uint32_t now_ms);

const char *logger_h10_phase_name(logger_h10_phase_t phase);

#endif