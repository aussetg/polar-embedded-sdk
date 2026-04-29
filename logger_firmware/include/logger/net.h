#ifndef LOGGER_FIRMWARE_NET_H
#define LOGGER_FIRMWARE_NET_H

#include <stdbool.h>

#include "logger/busy_poll.h"
#include "logger/config_store.h"

bool logger_net_wifi_join(const logger_config_t *config, int *rc_out,
                          char ip_buf[48], const logger_busy_poll_t *busy_poll);
void logger_net_wifi_leave(void);

#endif