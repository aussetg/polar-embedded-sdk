#include "logger/net.h"

#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "logger/util.h"

bool logger_net_wifi_join(const logger_config_t *config, int *rc_out,
                          char ip_buf[48]) {
  if (rc_out != NULL) {
    *rc_out = PICO_ERROR_GENERIC;
  }
  if (ip_buf != NULL) {
    ip_buf[0] = '\0';
  }
  if (config == NULL || !logger_string_present(config->wifi_ssid)) {
    return false;
  }

  int rc = PICO_ERROR_CONNECT_FAILED;
  if (!logger_string_present(config->wifi_psk)) {
    cyw43_arch_enable_sta_mode();
    rc = cyw43_arch_wifi_connect_timeout_ms(config->wifi_ssid, NULL,
                                            CYW43_AUTH_OPEN, 30000u);
  } else {
    static const uint32_t auth_modes[] = {
        CYW43_AUTH_WPA2_AES_PSK,
        CYW43_AUTH_WPA2_MIXED_PSK,
        CYW43_AUTH_WPA3_WPA2_AES_PSK,
    };
    for (size_t i = 0u; i < sizeof(auth_modes) / sizeof(auth_modes[0]); ++i) {
      cyw43_arch_disable_sta_mode();
      sleep_ms(100);
      cyw43_arch_enable_sta_mode();
      rc = cyw43_arch_wifi_connect_timeout_ms(
          config->wifi_ssid, config->wifi_psk, auth_modes[i], 30000u);
      if (rc == 0 || rc == PICO_ERROR_BADAUTH) {
        break;
      }
    }
  }
  if (rc_out != NULL) {
    *rc_out = rc;
  }
  if (rc != 0) {
    return false;
  }

  const uint32_t dhcp_deadline = to_ms_since_boot(get_absolute_time()) + 15000u;
  while (true) {
    cyw43_arch_poll();
    if (netif_default != NULL && netif_is_up(netif_default) &&
        netif_is_link_up(netif_default) &&
        !ip4_addr_isany(netif_ip4_addr(netif_default))) {
      break;
    }
    if (to_ms_since_boot(get_absolute_time()) >= dhcp_deadline) {
      if (rc_out != NULL) {
        *rc_out = PICO_ERROR_TIMEOUT;
      }
      return false;
    }
    sleep_ms(25);
  }

  if (ip_buf != NULL && netif_default != NULL) {
    ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_buf, 48);
  }
  return true;
}

void logger_net_wifi_leave(void) { cyw43_arch_disable_sta_mode(); }
