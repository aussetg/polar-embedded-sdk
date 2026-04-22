#include "logger/net.h"

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "logger/util.h"

#define LOGGER_WIFI_JOIN_TIMEOUT_MS 30000u
#define LOGGER_WIFI_JOIN_POLL_MS 25u
#define LOGGER_WIFI_STA_RESET_DELAY_MS 100u

static int logger_net_wifi_join_auth_mode(const char *ssid, const char *psk,
                                          uint32_t auth_mode) {
  const int err = cyw43_arch_wifi_connect_async(ssid, psk, auth_mode);
  if (err != PICO_OK) {
    return err;
  }

  const uint32_t deadline =
      to_ms_since_boot(get_absolute_time()) + LOGGER_WIFI_JOIN_TIMEOUT_MS;
  int status = CYW43_LINK_UP + 1;

  while (true) {
    watchdog_update();
    cyw43_arch_poll();

    int new_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (new_status == CYW43_LINK_NONET) {
      new_status = CYW43_LINK_JOIN;
      const int retry_err = cyw43_arch_wifi_connect_async(ssid, psk, auth_mode);
      if (retry_err != PICO_OK) {
        return retry_err;
      }
    }
    status = new_status;

    if (status == CYW43_LINK_UP) {
      return PICO_OK;
    }
    if (status == CYW43_LINK_BADAUTH) {
      return PICO_ERROR_BADAUTH;
    }
    if (status == CYW43_LINK_FAIL) {
      return PICO_ERROR_CONNECT_FAILED;
    }
    if ((int32_t)(to_ms_since_boot(get_absolute_time()) - deadline) >= 0) {
      return PICO_ERROR_TIMEOUT;
    }

    sleep_ms(LOGGER_WIFI_JOIN_POLL_MS);
  }
}

bool logger_net_wifi_join(const logger_config_t *config, int *rc_out,
                          char ip_buf[48]) {
  bool sta_mode_active = false;
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
    sta_mode_active = true;
    rc = logger_net_wifi_join_auth_mode(config->wifi_ssid, NULL,
                                        CYW43_AUTH_OPEN);
  } else {
    static const uint32_t auth_modes[] = {
        CYW43_AUTH_WPA2_AES_PSK,
        CYW43_AUTH_WPA2_MIXED_PSK,
        CYW43_AUTH_WPA3_WPA2_AES_PSK,
    };
    for (size_t i = 0u; i < sizeof(auth_modes) / sizeof(auth_modes[0]); ++i) {
      cyw43_arch_disable_sta_mode();
      watchdog_update();
      sleep_ms(LOGGER_WIFI_STA_RESET_DELAY_MS);
      cyw43_arch_enable_sta_mode();
      sta_mode_active = true;
      rc = logger_net_wifi_join_auth_mode(config->wifi_ssid, config->wifi_psk,
                                          auth_modes[i]);
      if (rc == 0 || rc == PICO_ERROR_BADAUTH) {
        break;
      }
    }
  }
  if (rc_out != NULL) {
    *rc_out = rc;
  }
  if (rc != 0) {
    goto fail;
  }

  const uint32_t dhcp_deadline = to_ms_since_boot(get_absolute_time()) + 15000u;
  while (true) {
    watchdog_update();
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
      goto fail;
    }
    sleep_ms(25);
  }

  if (ip_buf != NULL && netif_default != NULL) {
    ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_buf, 48);
  }
  return true;

fail:
  if (sta_mode_active) {
    logger_net_wifi_leave();
  }
  return false;
}

void logger_net_wifi_leave(void) { cyw43_arch_disable_sta_mode(); }
