#include "logger/upload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

#include "logger/json.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"

#ifndef LOGGER_FIRMWARE_VERSION
#define LOGGER_FIRMWARE_VERSION "0.1.0-dev"
#endif

#ifndef LOGGER_BUILD_ID
#define LOGGER_BUILD_ID "logger-fw-dev"
#endif

#define LOGGER_UPLOAD_HTTP_REQUEST_MAX 1024u
#define LOGGER_UPLOAD_HTTP_RESPONSE_MAX 2048u
#define LOGGER_UPLOAD_TCP_POLL_INTERVAL 2u
#define LOGGER_UPLOAD_DNS_TIMEOUT_MS 10000u
#define LOGGER_UPLOAD_CONNECT_TIMEOUT_MS 15000u
#define LOGGER_UPLOAD_RESPONSE_TIMEOUT_MS 30000u

typedef struct {
    bool https;
    char host[LOGGER_UPLOAD_URL_HOST_MAX + 1];
    char path[LOGGER_UPLOAD_URL_PATH_MAX + 1];
    uint16_t port;
    bool host_is_literal;
    ip_addr_t literal_addr;
} logger_upload_url_t;

typedef struct {
    struct tcp_pcb *pcb;
    ip_addr_t remote_addr;
    bool dns_done;
    err_t dns_err;
    bool connect_started;
    bool connected;
    err_t connect_err;
    bool remote_closed;
    bool request_complete;
    bool response_complete;
    bool response_truncated;
    bool transport_failed;
    err_t transport_err;
    int http_status;
    int content_length;
    size_t header_len;
    char request[LOGGER_UPLOAD_HTTP_REQUEST_MAX + 1u];
    size_t request_len;
    size_t request_offset;
    bool body_enabled;
    logger_upload_bundle_stream_t *bundle_stream;
    uint8_t body_chunk[512];
    size_t body_chunk_len;
    size_t body_chunk_offset;
    bool body_eof;
    char response[LOGGER_UPLOAD_HTTP_RESPONSE_MAX + 1u];
    size_t response_len;
} logger_upload_http_request_t;

typedef struct {
    int http_status;
    bool body_truncated;
    char body[LOGGER_UPLOAD_HTTP_RESPONSE_MAX + 1u];
    char remote_address[48];
} logger_upload_http_response_t;

typedef struct {
    bool ok;
    bool retryable;
    bool deduplicated;
    char session_id[33];
    char sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1];
    uint64_t size_bytes;
    char receipt_id[LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX + 1];
    char received_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char error_code[48];
    char error_message[LOGGER_UPLOAD_MESSAGE_MAX + 1];
} logger_upload_server_reply_t;

static bool logger_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

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

static void logger_set_literal(char *dst, size_t dst_len, const char *value) {
    logger_copy_string(dst, dst_len, value);
}

static bool logger_path_join3(char *dst, size_t dst_len, const char *a, const char *b, const char *c) {
    const size_t a_len = strlen(a);
    const size_t b_len = strlen(b);
    const size_t c_len = strlen(c);
    if ((a_len + b_len + c_len + 1u) > dst_len) {
        return false;
    }
    memcpy(dst, a, a_len);
    memcpy(dst + a_len, b, b_len);
    memcpy(dst + a_len + b_len, c, c_len + 1u);
    return true;
}

static void logger_upload_net_test_result_fail_all(logger_upload_net_test_result_t *result, const char *message) {
    result->wifi_join_result = "fail";
    result->dns_result = "fail";
    result->tls_result = "fail";
    result->upload_endpoint_reachable_result = "fail";
    logger_set_literal(result->wifi_join_details, sizeof(result->wifi_join_details), message);
    logger_set_literal(result->dns_details, sizeof(result->dns_details), message);
    logger_set_literal(result->tls_details, sizeof(result->tls_details), message);
    logger_set_literal(result->upload_endpoint_reachable_details, sizeof(result->upload_endpoint_reachable_details), message);
}

void logger_upload_net_test_result_init(logger_upload_net_test_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->wifi_join_result = "fail";
    result->dns_result = "fail";
    result->tls_result = "fail";
    result->upload_endpoint_reachable_result = "fail";
}

void logger_upload_process_result_init(logger_upload_process_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NONE;
    result->http_status = -1;
}

static bool logger_upload_parse_url(const char *url, logger_upload_url_t *parsed) {
    memset(parsed, 0, sizeof(*parsed));
    if (!logger_string_present(url)) {
        return false;
    }

    const char *after_scheme = NULL;
    if (strncmp(url, "http://", 7u) == 0) {
        parsed->https = false;
        parsed->port = 80u;
        after_scheme = url + 7u;
    } else if (strncmp(url, "https://", 8u) == 0) {
        parsed->https = true;
        parsed->port = 443u;
        after_scheme = url + 8u;
    } else {
        return false;
    }

    const char *path_start = strchr(after_scheme, '/');
    const char *authority_end = path_start != NULL ? path_start : (after_scheme + strlen(after_scheme));
    if (authority_end == after_scheme) {
        return false;
    }

    const char *host_start = after_scheme;
    const char *host_end = authority_end;
    const char *port_start = NULL;
    if (*host_start == '[') {
        const char *close = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (close == NULL) {
            return false;
        }
        host_start += 1;
        host_end = close;
        if ((close + 1) < authority_end) {
            if (*(close + 1) != ':') {
                return false;
            }
            port_start = close + 2;
        }
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));
        if (colon != NULL) {
            host_end = colon;
            port_start = colon + 1;
        }
    }

    if (host_end <= host_start || (size_t)(host_end - host_start) > LOGGER_UPLOAD_URL_HOST_MAX) {
        return false;
    }
    memcpy(parsed->host, host_start, (size_t)(host_end - host_start));
    parsed->host[host_end - host_start] = '\0';

    if (port_start != NULL) {
        unsigned long port = 0u;
        for (const char *p = port_start; p < authority_end; ++p) {
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
            port = (port * 10u) + (unsigned long)(*p - '0');
            if (port > 65535u) {
                return false;
            }
        }
        if (port == 0u) {
            return false;
        }
        parsed->port = (uint16_t)port;
    }

    if (path_start == NULL) {
        logger_copy_string(parsed->path, sizeof(parsed->path), "/");
    } else {
        if (strlen(path_start) > LOGGER_UPLOAD_URL_PATH_MAX) {
            return false;
        }
        logger_copy_string(parsed->path, sizeof(parsed->path), path_start);
    }

    parsed->host_is_literal = ipaddr_aton(parsed->host, &parsed->literal_addr) != 0;
    return true;
}

static bool logger_wifi_join(const logger_config_t *config, int *rc_out, char ip_buf[48]) {
    if (rc_out != NULL) {
        *rc_out = PICO_ERROR_GENERIC;
    }
    if (ip_buf != NULL) {
        ip_buf[0] = '\0';
    }
    if (!logger_string_present(config->wifi_ssid)) {
        return false;
    }

    int rc = PICO_ERROR_CONNECT_FAILED;
    if (!logger_string_present(config->wifi_psk)) {
        cyw43_arch_enable_sta_mode();
        rc = cyw43_arch_wifi_connect_timeout_ms(config->wifi_ssid, NULL, CYW43_AUTH_OPEN, 30000u);
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
                config->wifi_ssid,
                config->wifi_psk,
                auth_modes[i],
                30000u);
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
        if (netif_default != NULL &&
            netif_is_up(netif_default) &&
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

static void logger_wifi_leave(void) {
    cyw43_arch_disable_sta_mode();
}

static void logger_upload_http_request_init(logger_upload_http_request_t *request) {
    memset(request, 0, sizeof(*request));
    request->http_status = -1;
    request->content_length = -1;
}

static void logger_upload_http_close_pcb(logger_upload_http_request_t *request) {
    if (request->pcb == NULL) {
        return;
    }
    tcp_arg(request->pcb, NULL);
    tcp_recv(request->pcb, NULL);
    tcp_sent(request->pcb, NULL);
    tcp_err(request->pcb, NULL);
    tcp_poll(request->pcb, NULL, 0u);
    if (tcp_close(request->pcb) != ERR_OK) {
        tcp_abort(request->pcb);
    }
    request->pcb = NULL;
}

static void logger_upload_http_parse_response_progress(logger_upload_http_request_t *request) {
    if (request->http_status < 0) {
        const char *line_end = strstr(request->response, "\r\n");
        if (line_end != NULL) {
            int http_status = -1;
            if (sscanf(request->response, "HTTP/%*u.%*u %d", &http_status) == 1) {
                request->http_status = http_status;
            }
        }
    }

    if (request->header_len == 0u) {
        const char *headers_end = strstr(request->response, "\r\n\r\n");
        if (headers_end != NULL) {
            request->header_len = (size_t)(headers_end - request->response) + 4u;
            const char *cl = strstr(request->response, "\r\nContent-Length: ");
            if (cl != NULL && cl < headers_end) {
                cl += strlen("\r\nContent-Length: ");
                request->content_length = atoi(cl);
            }
        }
    }

    if (request->header_len != 0u && request->content_length >= 0) {
        const size_t total_needed = request->header_len + (size_t)request->content_length;
        if (request->response_len >= total_needed) {
            request->response_complete = true;
        }
    }
}

static err_t logger_upload_http_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    logger_upload_http_request_t *request = (logger_upload_http_request_t *)arg;
    if (err == ERR_OK) {
        request->connected = true;
    } else {
        request->connect_err = err;
        request->transport_failed = true;
        request->transport_err = err;
    }
    return ERR_OK;
}

static err_t logger_upload_http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)arg;
    (void)pcb;
    (void)len;
    return ERR_OK;
}

static err_t logger_upload_http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    logger_upload_http_request_t *request = (logger_upload_http_request_t *)arg;
    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        request->transport_failed = true;
        request->transport_err = err;
        return err;
    }

    if (p == NULL) {
        request->remote_closed = true;
        request->response_complete = true;
        logger_upload_http_close_pcb(request);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    if ((request->response_len + p->tot_len) >= sizeof(request->response)) {
        request->response_truncated = true;
        request->transport_failed = true;
        request->transport_err = ERR_MEM;
        pbuf_free(p);
        logger_upload_http_close_pcb(request);
        return ERR_MEM;
    }

    pbuf_copy_partial(p, request->response + request->response_len, p->tot_len, 0u);
    request->response_len += p->tot_len;
    request->response[request->response_len] = '\0';
    pbuf_free(p);

    logger_upload_http_parse_response_progress(request);
    if (request->response_complete) {
        logger_upload_http_close_pcb(request);
    }
    return ERR_OK;
}

static void logger_upload_http_err_cb(void *arg, err_t err) {
    logger_upload_http_request_t *request = (logger_upload_http_request_t *)arg;
    request->pcb = NULL;
    request->transport_failed = true;
    request->transport_err = err;
}

static void logger_upload_http_dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    logger_upload_http_request_t *request = (logger_upload_http_request_t *)arg;
    if (ipaddr != NULL) {
        ip_addr_copy(request->remote_addr, *ipaddr);
        request->dns_err = ERR_OK;
    } else {
        request->dns_err = ERR_ARG;
    }
    request->dns_done = true;
}

static bool logger_upload_http_fill_body_chunk(logger_upload_http_request_t *request) {
    if (!request->body_enabled || request->body_eof) {
        return true;
    }
    if (request->body_chunk_offset < request->body_chunk_len) {
        return true;
    }

    size_t chunk_len = 0u;
    if (!logger_upload_bundle_stream_read(
            request->bundle_stream,
            request->body_chunk,
            sizeof(request->body_chunk),
            &chunk_len)) {
        request->transport_failed = true;
        request->transport_err = ERR_VAL;
        return false;
    }
    request->body_chunk_len = chunk_len;
    request->body_chunk_offset = 0u;
    if (chunk_len == 0u) {
        request->body_eof = true;
    }
    return true;
}

static bool logger_upload_http_send_more(logger_upload_http_request_t *request) {
    if (!request->connected || request->pcb == NULL) {
        return true;
    }

    while (!request->transport_failed) {
        const u16_t sndbuf = tcp_sndbuf(request->pcb);
        if (sndbuf == 0u) {
            break;
        }

        const void *src = NULL;
        size_t len = 0u;
        if (request->request_offset < request->request_len) {
            src = request->request + request->request_offset;
            len = request->request_len - request->request_offset;
        } else {
            if (!logger_upload_http_fill_body_chunk(request)) {
                return false;
            }
            if (request->body_eof) {
                request->request_complete = true;
                break;
            }
            src = request->body_chunk + request->body_chunk_offset;
            len = request->body_chunk_len - request->body_chunk_offset;
        }

        if (len > sndbuf) {
            len = sndbuf;
        }
        if (len == 0u) {
            break;
        }

        const err_t err = tcp_write(request->pcb, src, (u16_t)len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            if (request->request_offset < request->request_len) {
                request->request_offset += len;
            } else {
                request->body_chunk_offset += len;
            }
            continue;
        }
        if (err == ERR_MEM) {
            break;
        }
        request->transport_failed = true;
        request->transport_err = err;
        return false;
    }

    if (request->pcb != NULL) {
        (void)tcp_output(request->pcb);
    }
    return true;
}

static bool logger_upload_http_resolve(logger_upload_http_request_t *request, const logger_upload_url_t *url) {
    if (url->host_is_literal) {
        ip_addr_copy(request->remote_addr, url->literal_addr);
        request->dns_done = true;
        request->dns_err = ERR_OK;
        return true;
    }

    const err_t dns_err = dns_gethostbyname(url->host, &request->remote_addr, logger_upload_http_dns_found_cb, request);
    if (dns_err == ERR_OK) {
        request->dns_done = true;
        request->dns_err = ERR_OK;
        return true;
    }
    if (dns_err == ERR_INPROGRESS) {
        return true;
    }
    request->dns_done = true;
    request->dns_err = dns_err;
    return false;
}

static bool logger_upload_http_connect(logger_upload_http_request_t *request, const logger_upload_url_t *url) {
    request->pcb = tcp_new_ip_type(IP_GET_TYPE(&request->remote_addr));
    if (request->pcb == NULL) {
        request->transport_failed = true;
        request->transport_err = ERR_MEM;
        return false;
    }
    tcp_arg(request->pcb, request);
    tcp_recv(request->pcb, logger_upload_http_recv_cb);
    tcp_sent(request->pcb, logger_upload_http_sent_cb);
    tcp_err(request->pcb, logger_upload_http_err_cb);
    tcp_poll(request->pcb, NULL, LOGGER_UPLOAD_TCP_POLL_INTERVAL);
    const err_t err = tcp_connect(request->pcb, &request->remote_addr, url->port, logger_upload_http_connected_cb);
    request->connect_started = true;
    if (err != ERR_OK) {
        request->connect_err = err;
        request->transport_failed = true;
        request->transport_err = err;
        logger_upload_http_close_pcb(request);
        return false;
    }
    return true;
}

static bool logger_upload_http_execute(
    const logger_upload_url_t *url,
    const char *request_text,
    logger_upload_bundle_stream_t *bundle_stream,
    uint32_t timeout_ms,
    logger_upload_http_response_t *response) {
    logger_upload_http_request_t request;
    logger_upload_http_request_init(&request);
    logger_copy_string(request.request, sizeof(request.request), request_text);
    request.request_len = strlen(request.request);
    request.body_enabled = bundle_stream != NULL;
    request.bundle_stream = bundle_stream;
    request.body_eof = bundle_stream == NULL;

    (void)logger_upload_http_resolve(&request, url);
    const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    const uint32_t dns_deadline = start_ms + LOGGER_UPLOAD_DNS_TIMEOUT_MS;
    const uint32_t connect_deadline = start_ms + LOGGER_UPLOAD_CONNECT_TIMEOUT_MS;
    const uint32_t deadline = start_ms + timeout_ms;

    while (!request.response_complete && !request.transport_failed) {
        cyw43_arch_poll();

        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (!request.dns_done && now_ms >= dns_deadline) {
            request.transport_failed = true;
            request.transport_err = ERR_TIMEOUT;
            break;
        }
        if (request.dns_done && request.dns_err != ERR_OK) {
            request.transport_failed = true;
            request.transport_err = request.dns_err;
            break;
        }
        if (request.dns_done && !request.connect_started) {
            (void)logger_upload_http_connect(&request, url);
        }
        if (request.connect_started && !request.connected && now_ms >= connect_deadline) {
            request.transport_failed = true;
            request.transport_err = ERR_TIMEOUT;
            break;
        }
        if (request.connected) {
            (void)logger_upload_http_send_more(&request);
        }
        if (now_ms >= deadline) {
            request.transport_failed = true;
            request.transport_err = ERR_TIMEOUT;
            break;
        }
        sleep_ms(2);
    }

    if (!request.response_complete && request.transport_failed) {
        logger_upload_http_close_pcb(&request);
        return false;
    }

    if (request.header_len == 0u || request.http_status < 0) {
        return false;
    }

    memset(response, 0, sizeof(*response));
    response->http_status = request.http_status;
    response->body_truncated = request.response_truncated;
    if (request.header_len < request.response_len) {
        const size_t body_len = request.response_len - request.header_len;
        const size_t copy_len = body_len < sizeof(response->body) - 1u ? body_len : sizeof(response->body) - 1u;
        memcpy(response->body, request.response + request.header_len, copy_len);
        response->body[copy_len] = '\0';
    }
    ipaddr_ntoa_r(&request.remote_addr, response->remote_address, sizeof(response->remote_address));
    return true;
}

static bool logger_upload_parse_server_reply(const logger_upload_http_response_t *response, logger_upload_server_reply_t *reply) {
    memset(reply, 0, sizeof(*reply));
    reply->retryable = false;

    jsmntok_t tokens[32];
    logger_json_doc_t doc;
    if (!logger_json_parse(&doc, response->body, strlen(response->body), tokens, sizeof(tokens) / sizeof(tokens[0]))) {
        return false;
    }
    const jsmntok_t *root = logger_json_root(&doc);
    if (root == NULL || root->type != JSMN_OBJECT) {
        return false;
    }

    if (response->http_status >= 200 && response->http_status < 300) {
        const jsmntok_t *ok_tok = logger_json_object_get(&doc, root, "ok");
        reply->ok = logger_json_token_get_bool(&doc, ok_tok, &reply->ok) && reply->ok;
        if (!reply->ok) {
            return false;
        }

        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, root, "session_id"),
                                            reply->session_id,
                                            sizeof(reply->session_id));
        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, root, "sha256"),
                                            reply->sha256,
                                            sizeof(reply->sha256));
        (void)logger_json_token_get_uint64(&doc,
                                           logger_json_object_get(&doc, root, "size_bytes"),
                                           &reply->size_bytes);
        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, root, "receipt_id"),
                                            reply->receipt_id,
                                            sizeof(reply->receipt_id));
        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, root, "received_at_utc"),
                                            reply->received_at_utc,
                                            sizeof(reply->received_at_utc));
        (void)logger_json_token_get_bool(&doc,
                                         logger_json_object_get(&doc, root, "deduplicated"),
                                         &reply->deduplicated);
        return logger_string_present(reply->receipt_id);
    }

    reply->ok = false;
    const jsmntok_t *error_tok = logger_json_object_get(&doc, root, "error");
    if (error_tok != NULL && error_tok->type == JSMN_OBJECT) {
        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, error_tok, "code"),
                                            reply->error_code,
                                            sizeof(reply->error_code));
        (void)logger_json_token_copy_string(&doc,
                                            logger_json_object_get(&doc, error_tok, "message"),
                                            reply->error_message,
                                            sizeof(reply->error_message));
        (void)logger_json_token_get_bool(&doc,
                                         logger_json_object_get(&doc, error_tok, "retryable"),
                                         &reply->retryable);
    }
    return true;
}

static const char *logger_upload_failure_class_for_http(int http_status, const logger_upload_server_reply_t *reply) {
    if (http_status == 426 || strcmp(reply->error_code, "minimum_firmware") == 0) {
        return "min_firmware_rejected";
    }
    if (http_status == 422 || strcmp(reply->error_code, "validation_failed") == 0) {
        return "server_validation_failed";
    }
    return "http_rejected";
}

static void logger_upload_queue_set_updated_at(logger_upload_queue_t *queue, const char *now_utc_or_null) {
    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc), now_utc_or_null);
}

static logger_upload_queue_entry_t *logger_upload_queue_find_next_eligible(logger_upload_queue_t *queue) {
    for (size_t i = 0u; i < queue->session_count; ++i) {
        if (strcmp(queue->sessions[i].status, "pending") == 0 || strcmp(queue->sessions[i].status, "failed") == 0) {
            return &queue->sessions[i];
        }
    }
    return NULL;
}

static void logger_upload_append_event(
    logger_system_log_t *system_log,
    const char *now_utc_or_null,
    const char *kind,
    logger_system_log_severity_t severity,
    const char *session_id,
    const char *extra_json_tail) {
    if (system_log == NULL) {
        return;
    }
    char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
    if (logger_string_present(extra_json_tail)) {
        snprintf(details, sizeof(details), "{\"session_id\":\"%s\",%s}", session_id, extra_json_tail);
    } else {
        snprintf(details, sizeof(details), "{\"session_id\":\"%s\"}", session_id);
    }
    (void)logger_system_log_append(system_log, now_utc_or_null, kind, severity, details);
}

static bool logger_upload_recompute_entry_bundle(logger_upload_queue_entry_t *entry, char *message, size_t message_len) {
    char manifest_path[LOGGER_STORAGE_PATH_MAX];
    char journal_path[LOGGER_STORAGE_PATH_MAX];
    if (!logger_path_join3(manifest_path, sizeof(manifest_path), "0:/logger/sessions/", entry->dir_name, "/manifest.json") ||
        !logger_path_join3(journal_path, sizeof(journal_path), "0:/logger/sessions/", entry->dir_name, "/journal.bin")) {
        logger_copy_string(message, message_len, "session path too long");
        return false;
    }
    if (!logger_storage_file_exists(manifest_path) || !logger_storage_file_exists(journal_path)) {
        logger_copy_string(message, message_len, "session files missing");
        return false;
    }
    if (!logger_upload_bundle_compute(entry->dir_name, manifest_path, journal_path, entry->bundle_sha256, &entry->bundle_size_bytes)) {
        logger_copy_string(message, message_len, "failed to compute canonical bundle");
        return false;
    }
    return true;
}

bool logger_upload_net_test(
    const logger_config_t *config,
    logger_upload_net_test_result_t *result) {
    logger_upload_net_test_result_init(result);

    if (!logger_config_wifi_configured(config)) {
        logger_upload_net_test_result_fail_all(result, "wifi credentials are not configured");
        return false;
    }
    if (!logger_config_upload_configured(config)) {
        logger_upload_net_test_result_fail_all(result, "upload URL is not configured");
        return false;
    }

    logger_upload_url_t url;
    if (!logger_upload_parse_url(config->upload_url, &url)) {
        logger_upload_net_test_result_fail_all(result, "upload URL is not a valid absolute http(s) URL");
        return false;
    }

    char ip_buf[48] = {0};
    int wifi_rc = 0;
    if (!logger_wifi_join(config, &wifi_rc, ip_buf)) {
        result->wifi_join_result = "fail";
        snprintf(result->wifi_join_details, sizeof(result->wifi_join_details), "ssid=%s rc=%d", config->wifi_ssid, wifi_rc);
        result->dns_result = "fail";
        logger_copy_string(result->dns_details, sizeof(result->dns_details), "DNS not attempted because Wi-Fi join failed");
        result->tls_result = url.https ? "fail" : "not_applicable";
        logger_copy_string(result->tls_details,
                           sizeof(result->tls_details),
                           url.https ? "TLS not attempted because Wi-Fi join failed" : "HTTP URL does not require TLS");
        result->upload_endpoint_reachable_result = "fail";
        logger_copy_string(result->upload_endpoint_reachable_details,
                           sizeof(result->upload_endpoint_reachable_details),
                           "endpoint probe not attempted because Wi-Fi join failed");
        return false;
    }

    result->wifi_join_result = "pass";
    snprintf(result->wifi_join_details, sizeof(result->wifi_join_details), "ssid=%s ip=%s", config->wifi_ssid, ip_buf);

    if (url.https) {
        result->tls_result = "fail";
        logger_copy_string(result->tls_details, sizeof(result->tls_details), "HTTPS/TLS uploads are not implemented yet");
        result->dns_result = url.host_is_literal ? "pass" : "fail";
        if (url.host_is_literal) {
            snprintf(result->dns_details, sizeof(result->dns_details), "literal=%s", url.host);
        } else {
            logger_copy_string(result->dns_details, sizeof(result->dns_details), "DNS skipped because HTTPS uploads are not implemented yet");
        }
        result->upload_endpoint_reachable_result = "fail";
        logger_copy_string(result->upload_endpoint_reachable_details,
                           sizeof(result->upload_endpoint_reachable_details),
                           "endpoint probe skipped because HTTPS uploads are not implemented yet");
        logger_wifi_leave();
        return false;
    }

    result->tls_result = "not_applicable";
    logger_copy_string(result->tls_details, sizeof(result->tls_details), "HTTP upload URL does not use TLS");

    char probe_request[LOGGER_UPLOAD_HTTP_REQUEST_MAX + 1u];
    char host_header[LOGGER_UPLOAD_URL_HOST_MAX + 8];
    if (url.port == 80u) {
        snprintf(host_header, sizeof(host_header), "%s", url.host);
    } else {
        snprintf(host_header, sizeof(host_header), "%s:%u", url.host, (unsigned)url.port);
    }
    const int n = snprintf(
        probe_request,
        sizeof(probe_request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        url.path,
        host_header);
    if (n <= 0 || (size_t)n >= sizeof(probe_request)) {
        logger_wifi_leave();
        logger_upload_net_test_result_fail_all(result, "probe request buffer overflow");
        return false;
    }

    logger_upload_http_response_t probe_response;
    const bool reachable = logger_upload_http_execute(&url, probe_request, NULL, LOGGER_UPLOAD_RESPONSE_TIMEOUT_MS, &probe_response);
    if (url.host_is_literal) {
        result->dns_result = "pass";
        snprintf(result->dns_details, sizeof(result->dns_details), "literal=%s", url.host);
    } else if (reachable) {
        result->dns_result = "pass";
        snprintf(result->dns_details, sizeof(result->dns_details), "resolved=%s", probe_response.remote_address);
    } else {
        result->dns_result = "fail";
        logger_copy_string(result->dns_details, sizeof(result->dns_details), "failed to resolve or connect to upload host");
    }

    if (reachable) {
        result->upload_endpoint_reachable_result = "pass";
        snprintf(result->upload_endpoint_reachable_details,
                 sizeof(result->upload_endpoint_reachable_details),
                 "http_status=%d remote=%s",
                 probe_response.http_status,
                 probe_response.remote_address);
    } else {
        result->upload_endpoint_reachable_result = "fail";
        logger_copy_string(result->upload_endpoint_reachable_details,
                           sizeof(result->upload_endpoint_reachable_details),
                           "failed to receive an HTTP response from the upload endpoint");
    }

    logger_wifi_leave();
    return reachable;
}

bool logger_upload_process_one(
    logger_system_log_t *system_log,
    const logger_config_t *config,
    const char *hardware_id,
    const char *now_utc_or_null,
    logger_upload_process_result_t *result) {
    logger_upload_process_result_init(result);

    if (!logger_config_wifi_configured(config)) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "wifi credentials are not configured");
        return false;
    }
    if (!logger_config_upload_configured(config)) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "upload URL is not configured");
        return false;
    }

    logger_upload_url_t url;
    if (!logger_upload_parse_url(config->upload_url, &url)) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "upload URL is invalid");
        return false;
    }
    if (url.https) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->failure_class, sizeof(result->failure_class), "tls_failed");
        logger_copy_string(result->message, sizeof(result->message), "HTTPS/TLS uploads are not implemented yet");
        return false;
    }

    logger_upload_queue_t queue;
    if (!logger_upload_queue_load(&queue)) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "failed to load upload queue");
        return false;
    }

    logger_upload_queue_entry_t *entry = logger_upload_queue_find_next_eligible(&queue);
    if (entry == NULL) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK;
        logger_copy_string(result->message, sizeof(result->message), "no pending uploads");
        return true;
    }
    logger_copy_string(result->session_id, sizeof(result->session_id), entry->session_id);

    if (!logger_upload_recompute_entry_bundle(entry, result->message, sizeof(result->message))) {
        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "local_corrupt");
        logger_copy_string(entry->last_attempt_utc, sizeof(entry->last_attempt_utc), now_utc_or_null);
        entry->attempt_count += 1u;
        (void)logger_upload_queue_write(&queue);
        result->attempted = true;
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
        logger_upload_append_event(system_log,
                                   now_utc_or_null,
                                   "upload_failed",
                                   LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                   entry->session_id,
                                   "\"failure_class\":\"local_corrupt\"");
        return false;
    }

    char manifest_path[LOGGER_STORAGE_PATH_MAX];
    char journal_path[LOGGER_STORAGE_PATH_MAX];
    if (!logger_path_join3(manifest_path, sizeof(manifest_path), "0:/logger/sessions/", entry->dir_name, "/manifest.json") ||
        !logger_path_join3(journal_path, sizeof(journal_path), "0:/logger/sessions/", entry->dir_name, "/journal.bin")) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "session path is too long");
        return false;
    }

    logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
    logger_copy_string(entry->status, sizeof(entry->status), "uploading");
    entry->attempt_count += 1u;
    logger_copy_string(entry->last_attempt_utc, sizeof(entry->last_attempt_utc), now_utc_or_null);
    entry->last_failure_class[0] = '\0';
    entry->verified_upload_utc[0] = '\0';
    entry->receipt_id[0] = '\0';
    if (!logger_upload_queue_write(&queue)) {
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "failed to persist uploading queue state");
        return false;
    }

    char ip_buf[48] = {0};
    int wifi_rc = 0;
    if (!logger_wifi_join(config, &wifi_rc, ip_buf)) {
        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "wifi_join_failed");
        (void)logger_upload_queue_write(&queue);
        result->attempted = true;
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
        snprintf(result->message, sizeof(result->message), "Wi-Fi join failed rc=%d", wifi_rc);
        logger_upload_append_event(system_log,
                                   now_utc_or_null,
                                   "upload_failed",
                                   LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                   entry->session_id,
                                   "\"failure_class\":\"wifi_join_failed\"");
        return false;
    }

    char auth_header[LOGGER_CONFIG_UPLOAD_TOKEN_MAX + 32];
    auth_header[0] = '\0';
    if (logger_string_present(config->upload_token)) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s\r\n", config->upload_token);
    }

    char request_text[LOGGER_UPLOAD_HTTP_REQUEST_MAX + 1u];
    char host_header[LOGGER_UPLOAD_URL_HOST_MAX + 8];
    if (url.port == 80u) {
        snprintf(host_header, sizeof(host_header), "%s", url.host);
    } else {
        snprintf(host_header, sizeof(host_header), "%s:%u", url.host, (unsigned)url.port);
    }

    const int request_n = snprintf(
        request_text,
        sizeof(request_text),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/x-tar\r\n"
        "Content-Length: %llu\r\n"
        "X-Logger-Api-Version: 1\r\n"
        "X-Logger-Session-Id: %s\r\n"
        "X-Logger-Hardware-Id: %s\r\n"
        "X-Logger-Logger-Id: %s\r\n"
        "X-Logger-Subject-Id: %s\r\n"
        "X-Logger-Study-Day: %s\r\n"
        "X-Logger-SHA256: %s\r\n"
        "X-Logger-Tar-Canonicalization-Version: 1\r\n"
        "X-Logger-Manifest-Schema-Version: 1\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        url.path,
        host_header,
        (unsigned long long)entry->bundle_size_bytes,
        entry->session_id,
        hardware_id,
        config->logger_id,
        config->subject_id,
        entry->study_day_local,
        entry->bundle_sha256,
        auth_header);
    if (request_n <= 0 || (size_t)request_n >= sizeof(request_text)) {
        logger_wifi_leave();
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
        logger_copy_string(result->message, sizeof(result->message), "HTTP request buffer overflow");
        return false;
    }

    logger_upload_bundle_stream_t bundle_stream;
    if (!logger_upload_bundle_stream_open(&bundle_stream, entry->dir_name, manifest_path, journal_path)) {
        logger_wifi_leave();
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "local_corrupt");
        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        (void)logger_upload_queue_write(&queue);
        result->attempted = true;
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
        logger_copy_string(result->message, sizeof(result->message), "failed to open canonical bundle stream");
        return false;
    }

    logger_upload_http_response_t http_response;
    const bool http_ok = logger_upload_http_execute(
        &url,
        request_text,
        &bundle_stream,
        LOGGER_UPLOAD_RESPONSE_TIMEOUT_MS,
        &http_response);
    logger_upload_bundle_stream_close(&bundle_stream);
    logger_wifi_leave();

    if (!http_ok) {
        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "tcp_failed");
        (void)logger_upload_queue_write(&queue);
        result->attempted = true;
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
        logger_copy_string(result->message, sizeof(result->message), "HTTP upload transport failed");
        logger_upload_append_event(system_log,
                                   now_utc_or_null,
                                   "upload_failed",
                                   LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                   entry->session_id,
                                   "\"failure_class\":\"tcp_failed\"");
        return false;
    }

    logger_upload_server_reply_t reply;
    if (!logger_upload_parse_server_reply(&http_response, &reply)) {
        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "server_validation_failed");
        (void)logger_upload_queue_write(&queue);
        result->attempted = true;
        result->http_status = http_response.http_status;
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
        snprintf(result->message,
                 sizeof(result->message),
                 "server reply parse failed: %.104s",
                 http_response.body);
        logger_upload_append_event(system_log,
                                   now_utc_or_null,
                                   "upload_failed",
                                   LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                   entry->session_id,
                                   "\"failure_class\":\"server_validation_failed\"");
        return false;
    }

    result->attempted = true;
    result->http_status = http_response.http_status;
    if (http_response.http_status >= 200 && http_response.http_status < 300) {
        if ((logger_string_present(reply.session_id) && strcmp(reply.session_id, entry->session_id) != 0) ||
            (logger_string_present(reply.sha256) && strcmp(reply.sha256, entry->bundle_sha256) != 0) ||
            (reply.size_bytes != 0u && reply.size_bytes != entry->bundle_size_bytes)) {
            logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
            logger_copy_string(entry->status, sizeof(entry->status), "failed");
            logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), "hash_mismatch");
            (void)logger_upload_queue_write(&queue);
            result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
            logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
            logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
            logger_copy_string(result->message, sizeof(result->message), "server acknowledgment did not match uploaded bundle");
            logger_upload_append_event(system_log,
                                       now_utc_or_null,
                                       "upload_failed",
                                       LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                       entry->session_id,
                                       "\"failure_class\":\"hash_mismatch\"");
            return false;
        }

        logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
        logger_copy_string(entry->status, sizeof(entry->status), "verified");
        entry->last_failure_class[0] = '\0';
        logger_copy_string(entry->verified_upload_utc,
                           sizeof(entry->verified_upload_utc),
                           logger_string_present(reply.received_at_utc) ? reply.received_at_utc : now_utc_or_null);
        logger_copy_string(entry->receipt_id, sizeof(entry->receipt_id), reply.receipt_id);
        if (!logger_upload_queue_write(&queue)) {
            result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
            logger_copy_string(result->message, sizeof(result->message), "upload succeeded but queue write failed");
            return false;
        }

        result->code = LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED;
        logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
        logger_copy_string(result->receipt_id, sizeof(result->receipt_id), entry->receipt_id);
        logger_copy_string(result->verified_upload_utc, sizeof(result->verified_upload_utc), entry->verified_upload_utc);
        snprintf(result->message, sizeof(result->message), "verified via %s", reply.deduplicated ? "deduplicated ack" : "server ack");
        char extra[96];
        snprintf(extra,
                 sizeof(extra),
                 "\"receipt_id\":\"%s\",\"deduplicated\":%s",
                 entry->receipt_id,
                 reply.deduplicated ? "true" : "false");
        logger_upload_append_event(system_log,
                                   now_utc_or_null,
                                   "upload_verified",
                                   LOGGER_SYSTEM_LOG_SEVERITY_INFO,
                                   entry->session_id,
                                   extra);
        return true;
    }

    const char *failure_class = logger_upload_failure_class_for_http(http_response.http_status, &reply);
    logger_upload_queue_set_updated_at(&queue, now_utc_or_null);
    if (strcmp(failure_class, "min_firmware_rejected") == 0) {
        logger_copy_string(entry->status, sizeof(entry->status), "blocked_min_firmware");
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
    } else {
        logger_copy_string(entry->status, sizeof(entry->status), "failed");
        result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    }
    logger_copy_string(entry->last_failure_class, sizeof(entry->last_failure_class), failure_class);
    entry->verified_upload_utc[0] = '\0';
    entry->receipt_id[0] = '\0';
    (void)logger_upload_queue_write(&queue);

    logger_copy_string(result->final_status, sizeof(result->final_status), entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class), entry->last_failure_class);
    logger_copy_string(result->message,
                       sizeof(result->message),
                       logger_string_present(reply.error_message) ? reply.error_message : "server rejected upload");
    logger_upload_append_event(system_log,
                               now_utc_or_null,
                               result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE ? "upload_blocked_min_firmware" : "upload_failed",
                               LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                               entry->session_id,
                               result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE
                                   ? "\"failure_class\":\"min_firmware_rejected\""
                                   : "\"failure_class\":\"http_rejected\"");
    return result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
}