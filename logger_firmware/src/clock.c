#include "logger/civil_date.h"
#include "logger/clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "board_config.h"

#define LOGGER_PCF8523_REG_CONTROL1 0x00
#define LOGGER_PCF8523_REG_CONTROL3 0x02
#define LOGGER_PCF8523_REG_SECONDS 0x03
#define LOGGER_PCF8523_REG_CONTROL1_STOP 0x20
#define LOGGER_PCF8523_REG_CONTROL3_PM_MASK 0xe0
#define LOGGER_PCF8523_REG_CONTROL3_BLF 0x04
#define LOGGER_PCF8523_SECONDS_OS 0x80

#define LOGGER_CLOCK_NTP_PORT 123u
#define LOGGER_CLOCK_NTP_PACKET_LEN 48u
#define LOGGER_CLOCK_NTP_CLIENT_MODE 3u
#define LOGGER_CLOCK_NTP_SERVER_MODE 4u
#define LOGGER_CLOCK_NTP_VERSION 4u
#define LOGGER_CLOCK_NTP_UNIX_EPOCH_OFFSET 2208988800ull
#define LOGGER_CLOCK_NTP_DNS_TIMEOUT_MS 5000u
#define LOGGER_CLOCK_NTP_RESPONSE_TIMEOUT_MS 5000u

static const char *const LOGGER_CLOCK_NTP_SERVERS[] = {
    "0.pool.ntp.org",
    "1.pool.ntp.org",
    "2.pool.ntp.org",
};

static bool g_clock_initialized = false;

static bool logger_clock_datetime_reasonable_parts(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second);

typedef struct {
    bool done;
    err_t err;
    ip_addr_t address;
} logger_clock_dns_resolution_t;

typedef struct {
    bool received;
    bool truncated;
    size_t len;
    ip_addr_t expected_address;
    ip_addr_t source_address;
    u16_t source_port;
    uint8_t data[LOGGER_CLOCK_NTP_PACKET_LEN + 32u];
} logger_clock_ntp_response_t;

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

static uint32_t logger_clock_read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void logger_clock_write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)((value >> 24) & 0xffu);
    data[1] = (uint8_t)((value >> 16) & 0xffu);
    data[2] = (uint8_t)((value >> 8) & 0xffu);
    data[3] = (uint8_t)(value & 0xffu);
}

static void logger_clock_dns_found_cb(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    (void)hostname;
    logger_clock_dns_resolution_t *resolution = (logger_clock_dns_resolution_t *)arg;
    if (resolution == NULL) {
        return;
    }
    resolution->done = true;
    if (ipaddr == NULL) {
        resolution->err = ERR_VAL;
        return;
    }
    resolution->address = *ipaddr;
    resolution->err = ERR_OK;
}

static void logger_clock_ntp_recv_cb(
    void *arg,
    struct udp_pcb *pcb,
    struct pbuf *p,
    const ip_addr_t *addr,
    u16_t port) {
    (void)pcb;
    logger_clock_ntp_response_t *response = (logger_clock_ntp_response_t *)arg;
    if (response == NULL || p == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return;
    }
    if (addr == NULL || !ip_addr_cmp(addr, &response->expected_address) || port != LOGGER_CLOCK_NTP_PORT) {
        pbuf_free(p);
        return;
    }

    response->received = true;
    response->source_address = *addr;
    response->source_port = port;
    response->len = p->tot_len;
    if (response->len > sizeof(response->data)) {
        response->len = sizeof(response->data);
        response->truncated = true;
    }
    (void)pbuf_copy_partial(p, response->data, response->len, 0u);
    pbuf_free(p);
}

static bool logger_clock_resolve_ntp_server(
    const char *hostname,
    ip_addr_t *address_out,
    char remote_address_out[48],
    char message_out[LOGGER_CLOCK_NTP_MESSAGE_MAX + 1]) {
    if (address_out == NULL || remote_address_out == NULL || message_out == NULL) {
        return false;
    }
    remote_address_out[0] = '\0';
    message_out[0] = '\0';

    if (ipaddr_aton(hostname, address_out) != 0) {
        ipaddr_ntoa_r(address_out, remote_address_out, 48);
        return true;
    }

    logger_clock_dns_resolution_t resolution;
    memset(&resolution, 0, sizeof(resolution));
    err_t dns_err = dns_gethostbyname(hostname, address_out, logger_clock_dns_found_cb, &resolution);
    if (dns_err == ERR_OK) {
        ipaddr_ntoa_r(address_out, remote_address_out, 48);
        return true;
    }
    if (dns_err != ERR_INPROGRESS) {
        snprintf(message_out,
                 LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                 "DNS lookup failed err=%d",
                 (int)dns_err);
        return false;
    }

    const uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + LOGGER_CLOCK_NTP_DNS_TIMEOUT_MS;
    while (!resolution.done) {
        cyw43_arch_poll();
        if (to_ms_since_boot(get_absolute_time()) >= deadline_ms) {
            logger_copy_string(message_out,
                               LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                               "DNS lookup timed out");
            return false;
        }
        sleep_ms(25);
    }
    if (resolution.err != ERR_OK) {
        snprintf(message_out,
                 LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                 "DNS lookup failed err=%d",
                 (int)resolution.err);
        return false;
    }

    *address_out = resolution.address;
    ipaddr_ntoa_r(address_out, remote_address_out, 48);
    return true;
}

static bool logger_clock_ntp_exchange(
    const char *server,
    uint32_t originate_sec,
    uint32_t originate_frac,
    int64_t *unix_ns_out,
    uint8_t *stratum_out,
    char remote_address_out[48],
    char message_out[LOGGER_CLOCK_NTP_MESSAGE_MAX + 1]) {
    if (unix_ns_out == NULL || stratum_out == NULL || remote_address_out == NULL || message_out == NULL) {
        return false;
    }
    remote_address_out[0] = '\0';
    message_out[0] = '\0';

    ip_addr_t server_address;
    if (!logger_clock_resolve_ntp_server(server, &server_address, remote_address_out, message_out)) {
        return false;
    }
    if (!IP_IS_V4(&server_address)) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP server resolved to an unsupported address family");
        return false;
    }

    struct udp_pcb *pcb = udp_new_ip_type(IP_GET_TYPE(&server_address));
    if (pcb == NULL) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "failed to allocate an NTP UDP control block");
        return false;
    }

    logger_clock_ntp_response_t response_state;
    memset(&response_state, 0, sizeof(response_state));
    response_state.expected_address = server_address;
    udp_recv(pcb, logger_clock_ntp_recv_cb, &response_state);
    if (udp_bind(pcb, IP_ANY_TYPE, 0u) != ERR_OK) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "failed to bind the NTP UDP control block");
        udp_remove(pcb);
        return false;
    }

    uint8_t request[LOGGER_CLOCK_NTP_PACKET_LEN] = {0};
    request[0] = (uint8_t)((LOGGER_CLOCK_NTP_VERSION << 3) | LOGGER_CLOCK_NTP_CLIENT_MODE);
    logger_clock_write_be32(request + 40u, originate_sec);
    logger_clock_write_be32(request + 44u, originate_frac);

    struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, sizeof(request), PBUF_RAM);
    if (packet == NULL) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "failed to allocate an NTP request packet");
        udp_remove(pcb);
        return false;
    }
    if (pbuf_take(packet, request, sizeof(request)) != ERR_OK) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "failed to populate the NTP request packet");
        pbuf_free(packet);
        udp_remove(pcb);
        return false;
    }
    const err_t send_err = udp_sendto(pcb, packet, &server_address, LOGGER_CLOCK_NTP_PORT);
    pbuf_free(packet);
    if (send_err != ERR_OK) {
        snprintf(message_out,
                 LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                 "failed to send NTP request err=%d",
                 (int)send_err);
        udp_remove(pcb);
        return false;
    }

    const uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + LOGGER_CLOCK_NTP_RESPONSE_TIMEOUT_MS;
    while (!response_state.received) {
        cyw43_arch_poll();
        if (to_ms_since_boot(get_absolute_time()) >= deadline_ms) {
            logger_copy_string(message_out,
                               LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                               "NTP response timed out");
            udp_remove(pcb);
            return false;
        }
        sleep_ms(25);
    }

    if (response_state.truncated || response_state.len < LOGGER_CLOCK_NTP_PACKET_LEN) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "received truncated NTP response");
        udp_remove(pcb);
        return false;
    }

    ipaddr_ntoa_r(&response_state.source_address, remote_address_out, 48);

    const uint8_t li = (uint8_t)(response_state.data[0] >> 6);
    const uint8_t mode = (uint8_t)(response_state.data[0] & 0x07u);
    const uint8_t stratum = response_state.data[1];
    const uint32_t echoed_sec = logger_clock_read_be32(response_state.data + 24u);
    const uint32_t echoed_frac = logger_clock_read_be32(response_state.data + 28u);
    const uint32_t transmit_sec = logger_clock_read_be32(response_state.data + 40u);
    const uint32_t transmit_frac = logger_clock_read_be32(response_state.data + 44u);

    if (li == 3u) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP server reported unsynchronized time");
        udp_remove(pcb);
        return false;
    }
    if (mode != LOGGER_CLOCK_NTP_SERVER_MODE) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP response had an invalid mode");
        udp_remove(pcb);
        return false;
    }
    if (stratum == 0u || stratum > 15u) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP response had an invalid stratum");
        udp_remove(pcb);
        return false;
    }
    if (echoed_sec != originate_sec || echoed_frac != originate_frac) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP response did not echo the client transmit timestamp");
        udp_remove(pcb);
        return false;
    }
    if (transmit_sec == 0u && transmit_frac == 0u) {
        logger_copy_string(message_out,
                           LOGGER_CLOCK_NTP_MESSAGE_MAX + 1u,
                           "NTP response did not include a transmit timestamp");
        udp_remove(pcb);
        return false;
    }

    const int64_t unix_seconds = (int64_t)transmit_sec - (int64_t)LOGGER_CLOCK_NTP_UNIX_EPOCH_OFFSET;
    *stratum_out = stratum;
    *unix_ns_out = unix_seconds * 1000000000ll +
                   (int64_t)((((uint64_t)transmit_frac) * 1000000000ull) >> 32);
    udp_remove(pcb);
    return true;
}

static bool logger_clock_format_unix_seconds_rfc3339(
    int64_t unix_seconds,
    char out_rfc3339[LOGGER_CLOCK_RFC3339_UTC_LEN + 1]) {
    const int64_t days = unix_seconds / 86400ll;
    int64_t day_seconds = unix_seconds % 86400ll;
    if (day_seconds < 0) {
        day_seconds += 86400ll;
    }

    int year = 0;
    unsigned month = 0u;
    unsigned day = 0u;
    logger_civil_from_days(days, &year, &month, &day);

    const int hour = (int)(day_seconds / 3600ll);
    const int minute = (int)((day_seconds % 3600ll) / 60ll);
    const int second = (int)(day_seconds % 60ll);
    if (!logger_clock_datetime_reasonable_parts(year, (int)month, (int)day, hour, minute, second)) {
        return false;
    }

    const int n = snprintf(out_rfc3339,
                           LOGGER_CLOCK_RFC3339_UTC_LEN + 1u,
                           "%04d-%02u-%02uT%02d:%02d:%02dZ",
                           year,
                           month,
                           day,
                           hour,
                           minute,
                           second);
    return n > 0 && (size_t)n < (LOGGER_CLOCK_RFC3339_UTC_LEN + 1u);
}

void logger_clock_ntp_sync_result_init(logger_clock_ntp_sync_result_t *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
}

static int logger_bcd_decode(uint8_t value) {
    return ((value >> 4) * 10) + (value & 0x0f);
}

static uint8_t logger_bcd_encode(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool logger_is_leap_year(int year) {
    if ((year % 400) == 0) {
        return true;
    }
    if ((year % 100) == 0) {
        return false;
    }
    return (year % 4) == 0;
}

static int logger_days_in_month(int year, int month) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && logger_is_leap_year(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

static bool logger_clock_datetime_reasonable_parts(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second) {
    if (year < 2024 || year > 2099) {
        return false;
    }
    if (month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > logger_days_in_month(year, month)) {
        return false;
    }
    if (hour < 0 || hour > 23) {
        return false;
    }
    if (minute < 0 || minute > 59) {
        return false;
    }
    if (second < 0 || second > 59) {
        return false;
    }
    return true;
}

static bool logger_clock_datetime_reasonable(const logger_clock_status_t *status) {
    return logger_clock_datetime_reasonable_parts(
        status->year,
        status->month,
        status->day,
        status->hour,
        status->minute,
        status->second);
}

static bool logger_timezone_is_utc_like(const char *timezone) {
    if (timezone == NULL) {
        return false;
    }
    return strcmp(timezone, "UTC") == 0 || strcmp(timezone, "Etc/UTC") == 0;
}

static bool logger_timezone_present(const char *timezone) {
    return timezone != NULL && timezone[0] != '\0';
}

static int logger_weekday_from_date(int year, int month, int day) {
    static const int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) {
        year -= 1;
    }
    int sunday_based = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
    return (sunday_based + 6) % 7;
}


static bool logger_clock_status_to_observed_utc_ns(const logger_clock_status_t *status, int64_t *utc_ns_out) {
    if (!logger_clock_datetime_reasonable(status)) {
        return false;
    }
    const int64_t days = logger_days_from_civil(status->year, status->month, status->day);
    const int64_t seconds = days * 86400ll +
        ((int64_t)status->hour * 3600ll) +
        ((int64_t)status->minute * 60ll) +
        (int64_t)status->second;
    *utc_ns_out = seconds * 1000000000ll;
    return true;
}

static void logger_clock_sync_posix_time(const logger_clock_status_t *status) {
    if (status == NULL || !status->valid || !logger_clock_datetime_reasonable(status)) {
        return;
    }
    const int64_t days = logger_days_from_civil(status->year, status->month, status->day);
    const int64_t seconds = days * 86400ll +
                            ((int64_t)status->hour * 3600ll) +
                            ((int64_t)status->minute * 60ll) +
                            (int64_t)status->second;
    struct timeval tv;
    tv.tv_sec = (time_t)seconds;
    tv.tv_usec = 0;
    (void)settimeofday(&tv, NULL);
}

static bool logger_clock_read_reg(uint8_t reg, uint8_t *value) {
    if (i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(i2c0, LOGGER_RTC_I2C_ADDR, value, 1, false) == 1;
}

static bool logger_clock_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, buf, sizeof(buf), false) == (int)sizeof(buf);
}

static bool logger_clock_update_reg(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0u;
    if (!logger_clock_read_reg(reg, &current)) {
        return false;
    }
    current = (uint8_t)((current & ~mask) | (value & mask));
    return logger_clock_write_reg(reg, current);
}

static void logger_clock_format_now(logger_clock_status_t *status) {
    snprintf(status->now_utc, sizeof(status->now_utc),
             "%04d-%02d-%02dT%02d:%02d:%02dZ",
             status->year,
             status->month,
             status->day,
             status->hour,
             status->minute,
             status->second);
}

void logger_clock_init(void) {
    if (g_clock_initialized) {
        return;
    }

    i2c_init(i2c0, 400000u);
    gpio_set_function(LOGGER_RTC_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(LOGGER_RTC_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(LOGGER_RTC_SDA_PIN);
    gpio_pull_up(LOGGER_RTC_SCL_PIN);
    g_clock_initialized = true;
}

void logger_clock_sample(logger_clock_status_t *status) {
    memset(status, 0, sizeof(*status));
    logger_clock_init();
    status->initialized = true;

    uint8_t reg = LOGGER_PCF8523_REG_SECONDS;
    uint8_t data[7];
    int rc = i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &reg, 1, true);
    if (rc != 1) {
        status->rtc_present = false;
        return;
    }
    rc = i2c_read_blocking(i2c0, LOGGER_RTC_I2C_ADDR, data, sizeof(data), false);
    if (rc != (int)sizeof(data)) {
        status->rtc_present = false;
        return;
    }

    uint8_t control3 = 0xffu;
    if (logger_clock_read_reg(LOGGER_PCF8523_REG_CONTROL3, &control3)) {
        status->battery_low = (control3 & LOGGER_PCF8523_REG_CONTROL3_BLF) != 0;
    }

    status->rtc_present = true;
    status->lost_power = (data[0] & LOGGER_PCF8523_SECONDS_OS) != 0;
    status->second = logger_bcd_decode((uint8_t)(data[0] & 0x7f));
    status->minute = logger_bcd_decode((uint8_t)(data[1] & 0x7f));
    status->hour = logger_bcd_decode((uint8_t)(data[2] & 0x3f));
    status->day = logger_bcd_decode((uint8_t)(data[3] & 0x3f));
    status->weekday = data[4] & 0x07;
    status->month = logger_bcd_decode((uint8_t)(data[5] & 0x1f));
    status->year = 2000 + logger_bcd_decode(data[6]);

    status->valid = !status->lost_power && logger_clock_datetime_reasonable(status);
    if (logger_clock_datetime_reasonable(status)) {
        logger_clock_format_now(status);
    }
    logger_clock_sync_posix_time(status);
}

bool logger_clock_set_utc(const char *rfc3339_utc, logger_clock_status_t *status_out) {
    if (rfc3339_utc == NULL || strlen(rfc3339_utc) != 20u) {
        return false;
    }

    logger_clock_datetime_t dt;
    memset(&dt, 0, sizeof(dt));
    if (sscanf(rfc3339_utc,
               "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &dt.year,
               &dt.month,
               &dt.day,
               &dt.hour,
               &dt.minute,
               &dt.second) != 6) {
        return false;
    }
    if (!logger_clock_datetime_reasonable_parts(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)) {
        return false;
    }

    logger_clock_init();
    dt.weekday = logger_weekday_from_date(dt.year, dt.month, dt.day);

    uint8_t buf[8];
    buf[0] = LOGGER_PCF8523_REG_SECONDS;
    buf[1] = logger_bcd_encode(dt.second) & 0x7fu;
    buf[2] = logger_bcd_encode(dt.minute) & 0x7fu;
    buf[3] = logger_bcd_encode(dt.hour) & 0x3fu;
    buf[4] = logger_bcd_encode(dt.day) & 0x3fu;
    buf[5] = (uint8_t)(dt.weekday & 0x07);
    buf[6] = logger_bcd_encode(dt.month) & 0x1fu;
    buf[7] = logger_bcd_encode(dt.year - 2000);

    if (!logger_clock_update_reg(LOGGER_PCF8523_REG_CONTROL3, LOGGER_PCF8523_REG_CONTROL3_PM_MASK, 0u)) {
        return false;
    }
    if (i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, buf, sizeof(buf), false) != (int)sizeof(buf)) {
        return false;
    }
    if (!logger_clock_update_reg(LOGGER_PCF8523_REG_CONTROL1, LOGGER_PCF8523_REG_CONTROL1_STOP, 0u)) {
        return false;
    }

    if (status_out != NULL) {
        logger_clock_sample(status_out);
    } else {
        logger_clock_status_t sampled;
        logger_clock_sample(&sampled);
    }
    return true;
}

bool logger_clock_ntp_sync(
    const logger_clock_status_t *current_status,
    logger_clock_ntp_sync_result_t *result,
    logger_clock_status_t *status_out) {
    if (result == NULL) {
        return false;
    }
    logger_clock_ntp_sync_result_init(result);

    logger_clock_status_t sampled_before;
    if (current_status == NULL) {
        logger_clock_sample(&sampled_before);
        current_status = &sampled_before;
    }

    result->previous_valid = current_status->valid;
    logger_copy_string(result->previous_utc, sizeof(result->previous_utc), current_status->now_utc);

    const uint64_t nonce = time_us_64();
    const uint32_t originate_sec = (uint32_t)(nonce >> 32);
    const uint32_t originate_frac = (uint32_t)nonce;

    bool have_previous_utc = false;
    int64_t previous_utc_ns = 0ll;
    if (logger_clock_status_to_observed_utc_ns(current_status, &previous_utc_ns)) {
        have_previous_utc = true;
    }

    for (size_t i = 0u; i < sizeof(LOGGER_CLOCK_NTP_SERVERS) / sizeof(LOGGER_CLOCK_NTP_SERVERS[0]); ++i) {
        const char *server = LOGGER_CLOCK_NTP_SERVERS[i];
        logger_copy_string(result->server, sizeof(result->server), server);
        result->attempted = true;

        int64_t ntp_utc_ns = 0ll;
        if (!logger_clock_ntp_exchange(
                server,
                originate_sec,
                originate_frac,
                &ntp_utc_ns,
                &result->stratum,
                result->remote_address,
                result->message)) {
            continue;
        }

        int64_t applied_unix_seconds = (int64_t)(ntp_utc_ns / 1000000000ull);
        if ((ntp_utc_ns % 1000000000ull) >= 500000000ull) {
            applied_unix_seconds += 1ll;
        }
        if (!logger_clock_format_unix_seconds_rfc3339(applied_unix_seconds, result->applied_utc)) {
            logger_copy_string(result->message,
                               sizeof(result->message),
                               "NTP response was outside the supported date range");
            continue;
        }

        if (!logger_clock_set_utc(result->applied_utc, status_out)) {
            logger_copy_string(result->message,
                               sizeof(result->message),
                               "failed to apply NTP time to the RTC");
            return false;
        }

        if (status_out != NULL && status_out->now_utc[0] != '\0') {
            logger_copy_string(result->applied_utc, sizeof(result->applied_utc), status_out->now_utc);
        }
        if (have_previous_utc) {
            result->correction_seconds = applied_unix_seconds - (previous_utc_ns / 1000000000ll);
            result->large_correction = result->correction_seconds < -300ll || result->correction_seconds > 300ll;
        }

        result->applied = true;
        snprintf(result->message,
                 sizeof(result->message),
                 "applied NTP time from %s",
                 result->server);
        return true;
    }

    if (result->message[0] == '\0') {
        logger_copy_string(result->message,
                           sizeof(result->message),
                           "no NTP server produced a usable response");
    }
    return false;
}

const char *logger_clock_state_name(const logger_clock_status_t *status) {
    return status->valid ? "valid" : "invalid";
}

bool logger_clock_observed_utc_ns(const logger_clock_status_t *status, int64_t *utc_ns_out) {
    if (status == NULL || utc_ns_out == NULL) {
        return false;
    }
    return logger_clock_status_to_observed_utc_ns(status, utc_ns_out);
}

bool logger_clock_format_utc_ns_rfc3339(int64_t utc_ns, char out_rfc3339[LOGGER_CLOCK_RFC3339_UTC_LEN + 1]) {
    if (out_rfc3339 == NULL) {
        return false;
    }

    int64_t seconds = utc_ns / 1000000000ll;
    int32_t nanos = (int32_t)(utc_ns % 1000000000ll);
    if (nanos < 0) {
        nanos += 1000000000;
        seconds -= 1;
    }

    const int64_t days = seconds / 86400ll;
    int64_t day_seconds = seconds % 86400ll;
    if (day_seconds < 0) {
        day_seconds += 86400ll;
    }

    int year = 0;
    unsigned month = 0u;
    unsigned day = 0u;
    logger_civil_from_days(days, &year, &month, &day);

    const int hour = (int)(day_seconds / 3600ll);
    const int minute = (int)((day_seconds % 3600ll) / 60ll);
    const int second = (int)(day_seconds % 60ll);

    const int base_n = snprintf(out_rfc3339,
                                LOGGER_CLOCK_RFC3339_UTC_LEN + 1,
                                "%04d-%02u-%02uT%02d:%02d:%02d",
                                year,
                                month,
                                day,
                                hour,
                                minute,
                                second);
    if (base_n <= 0 || (size_t)base_n >= (LOGGER_CLOCK_RFC3339_UTC_LEN + 1u)) {
        return false;
    }
    size_t out_len = (size_t)base_n;

    if (nanos == 0) {
        out_rfc3339[out_len++] = 'Z';
        out_rfc3339[out_len] = '\0';
        return true;
    }

    char frac[10];
    snprintf(frac, sizeof(frac), "%09d", (int)nanos);
    size_t frac_len = 9u;
    while (frac_len > 0u && frac[frac_len - 1u] == '0') {
        frac[--frac_len] = '\0';
    }

    out_rfc3339[out_len++] = '.';
    memcpy(out_rfc3339 + out_len, frac, frac_len);
    out_len += frac_len;
    out_rfc3339[out_len++] = 'Z';
    out_rfc3339[out_len] = '\0';
    return true;
}

static bool logger_clock_derive_study_day_from_fields(
    int year,
    int month,
    int day,
    int hour,
    char out_study_day[11]) {
    if (!logger_clock_datetime_reasonable_parts(year, month, day, hour, 0, 0)) {
        return false;
    }

    if (hour < LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL) {
        day -= 1;
        if (day < 1) {
            month -= 1;
            if (month < 1) {
                month = 12;
                year -= 1;
            }
            day = logger_days_in_month(year, month);
        }
    }

    snprintf(out_study_day, 11, "%04d-%02d-%02d", year, month, day);
    return true;
}

bool logger_clock_derive_study_day_local(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]) {
    if (!status->valid || !logger_timezone_is_utc_like(timezone)) {
        return false;
    }
    return logger_clock_derive_study_day_from_fields(
        status->year,
        status->month,
        status->day,
        status->hour,
        out_study_day);
}

bool logger_clock_derive_study_day_local_observed(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]) {
    if (status == NULL || out_study_day == NULL) {
        return false;
    }
    if (!logger_timezone_is_utc_like(timezone) && !logger_timezone_present(timezone)) {
        return false;
    }
    if (!logger_clock_datetime_reasonable(status)) {
        return false;
    }
    return logger_clock_derive_study_day_from_fields(
        status->year,
        status->month,
        status->day,
        status->hour,
        out_study_day);
}
