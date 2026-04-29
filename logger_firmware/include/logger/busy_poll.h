#ifndef LOGGER_FIRMWARE_BUSY_POLL_H
#define LOGGER_FIRMWARE_BUSY_POLL_H

typedef enum {
  LOGGER_BUSY_POLL_PHASE_NONE = 0,
  LOGGER_BUSY_POLL_PHASE_WIFI_JOIN,
  LOGGER_BUSY_POLL_PHASE_WIFI_DHCP,
  LOGGER_BUSY_POLL_PHASE_NTP_DNS,
  LOGGER_BUSY_POLL_PHASE_NTP_RESPONSE,
  LOGGER_BUSY_POLL_PHASE_UPLOAD_DNS,
  LOGGER_BUSY_POLL_PHASE_UPLOAD_CONNECT,
  LOGGER_BUSY_POLL_PHASE_UPLOAD_HTTP,
} logger_busy_poll_phase_t;

typedef struct {
  void *ctx;
  void (*poll)(void *ctx, logger_busy_poll_phase_t phase);
} logger_busy_poll_t;

static inline const char *
logger_busy_poll_phase_name(logger_busy_poll_phase_t phase) {
  switch (phase) {
  case LOGGER_BUSY_POLL_PHASE_WIFI_JOIN:
    return "wifi_join";
  case LOGGER_BUSY_POLL_PHASE_WIFI_DHCP:
    return "wifi_dhcp";
  case LOGGER_BUSY_POLL_PHASE_NTP_DNS:
    return "ntp_dns";
  case LOGGER_BUSY_POLL_PHASE_NTP_RESPONSE:
    return "ntp_response";
  case LOGGER_BUSY_POLL_PHASE_UPLOAD_DNS:
    return "upload_dns";
  case LOGGER_BUSY_POLL_PHASE_UPLOAD_CONNECT:
    return "upload_connect";
  case LOGGER_BUSY_POLL_PHASE_UPLOAD_HTTP:
    return "upload_http";
  case LOGGER_BUSY_POLL_PHASE_NONE:
  default:
    return "none";
  }
}

static inline void logger_busy_poll_run(const logger_busy_poll_t *busy_poll,
                                        logger_busy_poll_phase_t phase) {
  if (busy_poll != 0 && busy_poll->poll != 0) {
    busy_poll->poll(busy_poll->ctx, phase);
  }
}

#endif
