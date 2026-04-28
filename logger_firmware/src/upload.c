#include "logger/upload.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "mbedtls/ssl.h"

#include "logger/json.h"
#include "logger/json_writer.h"
#include "logger/net.h"
#include "logger/psram_layout.h"
#include "logger/storage.h"
#include "logger/storage_service.h"
#include "logger/upload_bundle.h"
#include "logger/util.h"

#include "logger/version.h"
#include "upload_tls_roots.h"

#define LOGGER_UPLOAD_HTTP_REQUEST_MAX 1536u
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
  struct altcp_pcb *pcb;
  struct altcp_tls_config *tls_config;
  bool https;
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
  bool bundle_stream_active; /* true when service-based bundle stream is open */
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
  char transport_failure_class[LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX + 1u];
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

typedef struct {
  const char *mode;
  const char *root_profile;
  const uint8_t *ca_data;
  size_t ca_len;
  const char *anchor_sha256_or_null;
} logger_upload_tls_trust_t;

typedef struct {
  logger_upload_http_request_t request;
  bool in_use;
} logger_upload_http_request_workspace_t;

typedef struct {
  logger_upload_http_response_t response;
  bool in_use;
} logger_upload_http_response_workspace_t;

typedef struct {
  logger_upload_url_t url;
  logger_upload_server_reply_t reply;
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char request_text[LOGGER_UPLOAD_HTTP_REQUEST_MAX + 1u];
  char auth_header[LOGGER_CONFIG_UPLOAD_TOKEN_MAX + 32u];
  char api_key_header[LOGGER_CONFIG_UPLOAD_API_KEY_MAX + 24u];
  char host_header[LOGGER_UPLOAD_URL_HOST_MAX + 8u];
  char ip_buf[48u];
  bool in_use;
} logger_upload_process_workspace_t;

#define LOGGER_UPLOAD_PSRAM_WORKSPACE_BASE (PSRAM_UPLOAD_REGION_BASE)

#define LOGGER_UPLOAD_HTTP_REQUEST_WORKSPACE_ADDR                              \
  (LOGGER_UPLOAD_PSRAM_WORKSPACE_BASE)
#define LOGGER_UPLOAD_HTTP_RESPONSE_WORKSPACE_ADDR                             \
  (LOGGER_UPLOAD_HTTP_REQUEST_WORKSPACE_ADDR +                                 \
   sizeof(logger_upload_http_request_workspace_t))
#define LOGGER_UPLOAD_PROCESS_WORKSPACE_ADDR                                   \
  (LOGGER_UPLOAD_HTTP_RESPONSE_WORKSPACE_ADDR +                                \
   sizeof(logger_upload_http_response_workspace_t))
#define LOGGER_UPLOAD_PSRAM_WORKSPACE_END                                      \
  (LOGGER_UPLOAD_PROCESS_WORKSPACE_ADDR +                                      \
   sizeof(logger_upload_process_workspace_t))

_Static_assert(LOGGER_UPLOAD_PSRAM_WORKSPACE_END <=
                   PSRAM_UPLOAD_REGION_BASE + PSRAM_UPLOAD_REGION_SIZE,
               "upload PSRAM workspace exceeds reserved upload region");

static logger_upload_http_request_workspace_t *
logger_upload_http_request_workspace_ptr(void) {
  return (logger_upload_http_request_workspace_t *)
      LOGGER_UPLOAD_HTTP_REQUEST_WORKSPACE_ADDR;
}

static logger_upload_http_response_workspace_t *
logger_upload_http_response_workspace_ptr(void) {
  return (logger_upload_http_response_workspace_t *)
      LOGGER_UPLOAD_HTTP_RESPONSE_WORKSPACE_ADDR;
}

static logger_upload_process_workspace_t *
logger_upload_process_workspace_ptr(void) {
  return (
      logger_upload_process_workspace_t *)LOGGER_UPLOAD_PROCESS_WORKSPACE_ADDR;
}

static void
logger_upload_http_request_init(logger_upload_http_request_t *request);
static void
logger_upload_http_response_init(logger_upload_http_response_t *response);

static logger_upload_http_request_t *
logger_upload_http_request_workspace_acquire(void) {
  logger_upload_http_request_workspace_t *workspace =
      logger_upload_http_request_workspace_ptr();
  assert(!workspace->in_use);
  workspace->in_use = true;
  logger_upload_http_request_init(&workspace->request);
  return &workspace->request;
}

static void logger_upload_http_request_workspace_release(
    logger_upload_http_request_t *request) {
  (void)request;
  logger_upload_http_request_workspace_t *workspace =
      logger_upload_http_request_workspace_ptr();
  assert(request == &workspace->request);
  assert(workspace->in_use);
  workspace->in_use = false;
}

static logger_upload_http_response_t *
logger_upload_http_response_workspace_acquire(void) {
  logger_upload_http_response_workspace_t *workspace =
      logger_upload_http_response_workspace_ptr();
  assert(!workspace->in_use);
  workspace->in_use = true;
  logger_upload_http_response_init(&workspace->response);
  return &workspace->response;
}

static void logger_upload_http_response_workspace_release(
    logger_upload_http_response_t *response) {
  (void)response;
  logger_upload_http_response_workspace_t *workspace =
      logger_upload_http_response_workspace_ptr();
  assert(response == &workspace->response);
  assert(workspace->in_use);
  workspace->in_use = false;
}

static logger_upload_process_workspace_t *
logger_upload_process_workspace_acquire(void) {
  logger_upload_process_workspace_t *workspace =
      logger_upload_process_workspace_ptr();
  assert(!workspace->in_use);
  memset(workspace, 0, sizeof(*workspace));
  workspace->in_use = true;
  return workspace;
}

static void logger_upload_process_workspace_release(
    logger_upload_process_workspace_t *workspace) {
  (void)workspace;
  logger_upload_process_workspace_t *const expected =
      logger_upload_process_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

static bool logger_upload_auth_configured(const logger_config_t *config) {
  return config != NULL && logger_string_present(config->upload_api_key) &&
         logger_string_present(config->upload_token);
}

static void
logger_upload_net_test_result_fail_all(logger_upload_net_test_result_t *result,
                                       const char *message) {
  result->wifi_join_result = "fail";
  result->dns_result = "fail";
  result->tls_result = "fail";
  result->upload_endpoint_reachable_result = "fail";
  logger_copy_string(result->wifi_join_details,
                     sizeof(result->wifi_join_details), message);
  logger_copy_string(result->dns_details, sizeof(result->dns_details), message);
  logger_copy_string(result->tls_details, sizeof(result->tls_details), message);
  logger_copy_string(result->upload_endpoint_reachable_details,
                     sizeof(result->upload_endpoint_reachable_details),
                     message);
}

static void
logger_upload_net_test_result_init(logger_upload_net_test_result_t *result) {
  memset(result, 0, sizeof(*result));
  result->wifi_join_result = "fail";
  result->dns_result = "fail";
  result->tls_result = "fail";
  result->upload_endpoint_reachable_result = "fail";
}

static void
logger_upload_process_result_init(logger_upload_process_result_t *result) {
  memset(result, 0, sizeof(*result));
  result->code = LOGGER_UPLOAD_PROCESS_RESULT_NONE;
  result->http_status = -1;
}

static bool logger_upload_parse_url(const char *url,
                                    logger_upload_url_t *parsed) {
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
  const char *authority_end =
      path_start != NULL ? path_start : (after_scheme + strlen(after_scheme));
  if (authority_end == after_scheme) {
    return false;
  }

  const char *host_start = after_scheme;
  const char *host_end = authority_end;
  const char *port_start = NULL;
  if (*host_start == '[') {
    const char *close =
        memchr(host_start, ']', (size_t)(authority_end - host_start));
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
    const char *colon =
        memchr(host_start, ':', (size_t)(authority_end - host_start));
    if (colon != NULL) {
      host_end = colon;
      port_start = colon + 1;
    }
  }

  if (host_end <= host_start ||
      (size_t)(host_end - host_start) > LOGGER_UPLOAD_URL_HOST_MAX) {
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

  parsed->host_is_literal =
      ipaddr_aton(parsed->host, &parsed->literal_addr) != 0;
  return true;
}

static bool logger_upload_format_host_header(const logger_upload_url_t *url,
                                             char *out, size_t out_len) {
  if (url == NULL || out == NULL || out_len == 0u) {
    return false;
  }
  const bool default_port =
      (!url->https && url->port == 80u) || (url->https && url->port == 443u);
  const int n = snprintf(out, out_len, default_port ? "%s" : "%s:%u", url->host,
                         (unsigned)url->port);
  return n > 0 && (size_t)n < out_len;
}

static bool logger_upload_select_tls_trust(const logger_config_t *config,
                                           logger_upload_tls_trust_t *trust,
                                           char *message, size_t message_len) {
  memset(trust, 0, sizeof(*trust));
  const char *tls_mode = logger_config_upload_tls_mode(config);
  if (tls_mode == NULL ||
      strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS) == 0) {
    trust->mode = LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS;
    trust->root_profile = LOGGER_UPLOAD_TLS_PUBLIC_ROOT_PROFILE;
    trust->ca_data = (const uint8_t *)logger_upload_tls_ca_bundle_pem;
    trust->ca_len = sizeof(logger_upload_tls_ca_bundle_pem);
    return true;
  }
  if (strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
    if (!logger_config_upload_has_provisioned_anchor(config)) {
      logger_copy_string(message, message_len,
                         "provisioned upload TLS anchor is not configured");
      return false;
    }
    trust->mode = LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR;
    trust->root_profile = NULL;
    trust->ca_data = config->upload_tls_anchor_der;
    trust->ca_len = config->upload_tls_anchor_der_len;
    trust->anchor_sha256_or_null =
        logger_string_present(config->upload_tls_anchor_sha256)
            ? config->upload_tls_anchor_sha256
            : NULL;
    return true;
  }

  logger_copy_string(message, message_len, "upload TLS mode is unsupported");
  return false;
}

static void
logger_upload_format_tls_details(const logger_upload_tls_trust_t *trust,
                                 const char *suffix, char *out,
                                 size_t out_len) {
  if (strcmp(trust->mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
    snprintf(out, out_len,
             logger_string_present(trust->anchor_sha256_or_null)
                 ? "mode=%s sha256=%s %s"
                 : "mode=%s %s",
             trust->mode,
             logger_string_present(trust->anchor_sha256_or_null)
                 ? trust->anchor_sha256_or_null
                 : suffix,
             logger_string_present(trust->anchor_sha256_or_null) ? suffix : "");
    return;
  }
  snprintf(out, out_len, "mode=%s profile=%s %s", trust->mode,
           trust->root_profile, suffix);
}

static struct altcp_tls_config *
logger_upload_tls_create_client_config(const logger_upload_tls_trust_t *trust) {
  return altcp_tls_create_config_client((const u8_t *)trust->ca_data,
                                        trust->ca_len);
}

static void
logger_upload_http_request_init(logger_upload_http_request_t *request) {
  memset(request, 0, sizeof(*request));
  request->http_status = -1;
  request->content_length = -1;
}

static void
logger_upload_http_response_init(logger_upload_http_response_t *response) {
  memset(response, 0, sizeof(*response));
  response->http_status = -1;
}

static void
logger_upload_http_close_pcb(logger_upload_http_request_t *request) {
  if (request->pcb == NULL) {
    return;
  }
  altcp_arg(request->pcb, NULL);
  altcp_recv(request->pcb, NULL);
  altcp_sent(request->pcb, NULL);
  altcp_err(request->pcb, NULL);
  altcp_poll(request->pcb, NULL, 0u);
  if (altcp_close(request->pcb) != ERR_OK) {
    altcp_abort(request->pcb);
  }
  request->pcb = NULL;
}

static void
logger_upload_http_free_tls_config(logger_upload_http_request_t *request) {
  if (request->tls_config == NULL) {
    return;
  }
  altcp_tls_free_config(request->tls_config);
  altcp_tls_free_entropy();
  request->tls_config = NULL;
}

static void logger_upload_http_cleanup(logger_upload_http_request_t *request) {
  logger_upload_http_close_pcb(request);
  logger_upload_http_free_tls_config(request);
}

static void logger_upload_http_parse_response_progress(
    logger_upload_http_request_t *request) {
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
        char *end = NULL;
        long val = strtol(cl, &end, 10);
        if (end != cl && val >= 0 && val <= (long)INT_MAX) {
          request->content_length = (int)val;
        }
      }
    }
  }

  if (request->header_len != 0u && request->content_length >= 0) {
    const size_t total_needed =
        request->header_len + (size_t)request->content_length;
    if (request->response_len >= total_needed) {
      request->response_complete = true;
    }
  }
}

static err_t logger_upload_http_connected_cb(void *arg, struct altcp_pcb *pcb,
                                             err_t err) {
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

static err_t logger_upload_http_sent_cb(void *arg, struct altcp_pcb *pcb,
                                        u16_t len) {
  (void)arg;
  (void)pcb;
  (void)len;
  return ERR_OK;
}

static err_t logger_upload_http_recv_cb(void *arg, struct altcp_pcb *pcb,
                                        struct pbuf *p, err_t err) {
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

  altcp_recved(pcb, p->tot_len);
  if ((request->response_len + p->tot_len) >= sizeof(request->response)) {
    request->response_truncated = true;
    request->transport_failed = true;
    request->transport_err = ERR_MEM;
    pbuf_free(p);
    logger_upload_http_close_pcb(request);
    return ERR_MEM;
  }

  pbuf_copy_partial(p, request->response + request->response_len, p->tot_len,
                    0u);
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

static void logger_upload_http_dns_found_cb(const char *name,
                                            const ip_addr_t *ipaddr,
                                            void *arg) {
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

static bool
logger_upload_http_fill_body_chunk(logger_upload_http_request_t *request) {
  if (!request->body_enabled || request->body_eof) {
    return true;
  }
  if (request->body_chunk_offset < request->body_chunk_len) {
    return true;
  }

  size_t chunk_len = 0u;
  if (!logger_storage_svc_bundle_read(
          request->body_chunk, sizeof(request->body_chunk), &chunk_len)) {
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

static bool
logger_upload_http_send_more(logger_upload_http_request_t *request) {
  if (!request->connected || request->pcb == NULL) {
    return true;
  }

  while (!request->transport_failed) {
    const u16_t sndbuf = altcp_sndbuf(request->pcb);
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

    const err_t err =
        altcp_write(request->pcb, src, (u16_t)len, TCP_WRITE_FLAG_COPY);
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
    (void)altcp_output(request->pcb);
  }
  return true;
}

static bool logger_upload_http_resolve(logger_upload_http_request_t *request,
                                       const logger_upload_url_t *url) {
  if (url->host_is_literal) {
    ip_addr_copy(request->remote_addr, url->literal_addr);
    request->dns_done = true;
    request->dns_err = ERR_OK;
    return true;
  }

  const err_t dns_err =
      dns_gethostbyname(url->host, &request->remote_addr,
                        logger_upload_http_dns_found_cb, request);
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

static bool logger_upload_http_connect(logger_upload_http_request_t *request,
                                       const logger_upload_url_t *url,
                                       const logger_config_t *config) {
  request->https = url->https;
  if (url->https) {
    logger_upload_tls_trust_t trust;
    char tls_error[LOGGER_UPLOAD_MESSAGE_MAX + 1u] = {0};
    if (!logger_upload_select_tls_trust(config, &trust, tls_error,
                                        sizeof(tls_error))) {
      request->transport_failed = true;
      request->transport_err = ERR_ARG;
      return false;
    }
    request->tls_config = logger_upload_tls_create_client_config(&trust);
    if (request->tls_config == NULL) {
      request->transport_failed = true;
      request->transport_err = ERR_MEM;
      return false;
    }
    request->pcb =
        altcp_tls_new(request->tls_config, IP_GET_TYPE(&request->remote_addr));
  } else {
    request->pcb = altcp_new_ip_type(NULL, IP_GET_TYPE(&request->remote_addr));
  }
  if (request->pcb == NULL) {
    request->transport_failed = true;
    request->transport_err = ERR_MEM;
    logger_upload_http_free_tls_config(request);
    return false;
  }
  if (url->https) {
    mbedtls_ssl_context *ssl =
        (mbedtls_ssl_context *)altcp_tls_context(request->pcb);
    if (ssl == NULL || mbedtls_ssl_set_hostname(ssl, url->host) != 0) {
      request->transport_failed = true;
      request->transport_err = ERR_ARG;
      logger_upload_http_cleanup(request);
      return false;
    }
  }
  altcp_arg(request->pcb, request);
  altcp_recv(request->pcb, logger_upload_http_recv_cb);
  altcp_sent(request->pcb, logger_upload_http_sent_cb);
  altcp_err(request->pcb, logger_upload_http_err_cb);
  altcp_poll(request->pcb, NULL, LOGGER_UPLOAD_TCP_POLL_INTERVAL);
  const err_t err = altcp_connect(request->pcb, &request->remote_addr,
                                  url->port, logger_upload_http_connected_cb);
  request->connect_started = true;
  if (err != ERR_OK) {
    request->connect_err = err;
    request->transport_failed = true;
    request->transport_err = err;
    logger_upload_http_cleanup(request);
    return false;
  }
  return true;
}

static bool logger_upload_http_execute(
    const logger_config_t *config, const logger_upload_url_t *url,
    const char *request_text, bool use_bundle_stream, uint32_t timeout_ms,
    logger_upload_http_response_t *response) {
  logger_upload_http_request_t *request =
      logger_upload_http_request_workspace_acquire();
  logger_upload_http_response_init(response);
  logger_copy_string(request->request, sizeof(request->request), request_text);
  request->request_len = strlen(request->request);
  request->body_enabled = use_bundle_stream;
  request->bundle_stream_active = use_bundle_stream;
  request->body_eof = !use_bundle_stream;

  (void)logger_upload_http_resolve(request, url);
  const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  const uint32_t dns_deadline = start_ms + LOGGER_UPLOAD_DNS_TIMEOUT_MS;
  const uint32_t connect_deadline = start_ms + LOGGER_UPLOAD_CONNECT_TIMEOUT_MS;
  const uint32_t deadline = start_ms + timeout_ms;

  while (!request->response_complete && !request->transport_failed) {
    watchdog_update();
    cyw43_arch_poll();

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (!request->dns_done &&
        logger_mono_ms_deadline_reached(now_ms, dns_deadline)) {
      request->transport_failed = true;
      request->transport_err = ERR_TIMEOUT;
      break;
    }
    if (request->dns_done && request->dns_err != ERR_OK) {
      request->transport_failed = true;
      request->transport_err = request->dns_err;
      break;
    }
    if (request->dns_done && !request->connect_started) {
      (void)logger_upload_http_connect(request, url, config);
    }
    if (request->connect_started && !request->connected &&
        logger_mono_ms_deadline_reached(now_ms, connect_deadline)) {
      request->transport_failed = true;
      request->transport_err = ERR_TIMEOUT;
      break;
    }
    if (request->connected) {
      (void)logger_upload_http_send_more(request);
    }
    if (logger_mono_ms_deadline_reached(now_ms, deadline)) {
      request->transport_failed = true;
      request->transport_err = ERR_TIMEOUT;
      break;
    }
    sleep_ms(2);
  }

  if (request->dns_done && request->dns_err == ERR_OK) {
    ipaddr_ntoa_r(&request->remote_addr, response->remote_address,
                  sizeof(response->remote_address));
  }

  if (!request->response_complete && request->transport_failed) {
    logger_copy_string(response->transport_failure_class,
                       sizeof(response->transport_failure_class),
                       (request->dns_done && request->dns_err != ERR_OK)
                           ? "dns_failed"
                           : (url->https ? "tls_failed" : "tcp_failed"));
    logger_upload_http_cleanup(request);
    logger_upload_http_request_workspace_release(request);
    return false;
  }

  if (request->header_len == 0u || request->http_status < 0) {
    logger_copy_string(response->transport_failure_class,
                       sizeof(response->transport_failure_class),
                       url->https ? "tls_failed" : "tcp_failed");
    logger_upload_http_cleanup(request);
    logger_upload_http_request_workspace_release(request);
    return false;
  }

  response->http_status = request->http_status;
  response->body_truncated = request->response_truncated;
  if (request->header_len < request->response_len) {
    const size_t body_len = request->response_len - request->header_len;
    const size_t copy_len = body_len < sizeof(response->body) - 1u
                                ? body_len
                                : sizeof(response->body) - 1u;
    memcpy(response->body, request->response + request->header_len, copy_len);
    response->body[copy_len] = '\0';
  }
  logger_upload_http_cleanup(request);
  logger_upload_http_request_workspace_release(request);
  return true;
}

static bool
logger_upload_parse_server_reply(const logger_upload_http_response_t *response,
                                 logger_upload_server_reply_t *reply) {
  memset(reply, 0, sizeof(*reply));
  reply->retryable = false;

  jsmntok_t tokens[32];
  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, response->body, strlen(response->body), tokens,
                         sizeof(tokens) / sizeof(tokens[0]))) {
    return false;
  }
  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    return false;
  }

  if (response->http_status >= 200 && response->http_status < 300) {
    const jsmntok_t *ok_tok = logger_json_object_get(&doc, root, "ok");
    reply->ok =
        logger_json_token_get_bool(&doc, ok_tok, &reply->ok) && reply->ok;
    if (!reply->ok) {
      return false;
    }

    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, root, "session_id"),
        reply->session_id, sizeof(reply->session_id));
    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, root, "sha256"), reply->sha256,
        sizeof(reply->sha256));
    (void)logger_json_token_get_uint64(
        &doc, logger_json_object_get(&doc, root, "size_bytes"),
        &reply->size_bytes);
    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, root, "receipt_id"),
        reply->receipt_id, sizeof(reply->receipt_id));
    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, root, "received_at_utc"),
        reply->received_at_utc, sizeof(reply->received_at_utc));
    (void)logger_json_token_get_bool(
        &doc, logger_json_object_get(&doc, root, "deduplicated"),
        &reply->deduplicated);
    return logger_string_present(reply->receipt_id);
  }

  reply->ok = false;
  const jsmntok_t *error_parent = root;
  const jsmntok_t *detail_tok = logger_json_object_get(&doc, root, "detail");
  if (detail_tok != NULL && detail_tok->type == JSMN_OBJECT) {
    error_parent = detail_tok;
  }
  const jsmntok_t *error_tok =
      logger_json_object_get(&doc, error_parent, "error");
  if (error_tok != NULL && error_tok->type == JSMN_OBJECT) {
    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, error_tok, "code"),
        reply->error_code, sizeof(reply->error_code));
    (void)logger_json_token_copy_string(
        &doc, logger_json_object_get(&doc, error_tok, "message"),
        reply->error_message, sizeof(reply->error_message));
    (void)logger_json_token_get_bool(
        &doc, logger_json_object_get(&doc, error_tok, "retryable"),
        &reply->retryable);
  }
  return true;
}

static void
logger_upload_clear_entry_diagnostics(logger_upload_queue_entry_t *entry) {
  if (entry == NULL) {
    return;
  }
  entry->last_http_status = 0u;
  entry->last_server_error_code[0] = '\0';
  entry->last_server_error_message[0] = '\0';
  entry->last_response_excerpt[0] = '\0';
}

static void logger_upload_copy_response_excerpt(char *dst, size_t dst_len,
                                                const char *src) {
  if (dst == NULL || dst_len == 0u) {
    return;
  }
  dst[0] = '\0';
  if (src == NULL) {
    return;
  }
  size_t out = 0u;
  for (size_t in = 0u; src[in] != '\0' && (out + 1u) < dst_len; ++in) {
    char ch = src[in];
    if ((unsigned char)ch < 0x20u && ch != '\t') {
      ch = ' ';
    }
    dst[out++] = ch;
  }
  dst[out] = '\0';
}

static void logger_upload_set_entry_http_diagnostics(
    logger_upload_queue_entry_t *entry,
    const logger_upload_http_response_t *response,
    const logger_upload_server_reply_t *reply) {
  if (entry == NULL) {
    return;
  }
  logger_upload_clear_entry_diagnostics(entry);
  if (response != NULL && response->http_status > 0 &&
      response->http_status <= 999) {
    entry->last_http_status = (uint16_t)response->http_status;
    logger_upload_copy_response_excerpt(entry->last_response_excerpt,
                                        sizeof(entry->last_response_excerpt),
                                        response->body);
  }
  if (reply != NULL) {
    logger_copy_string(entry->last_server_error_code,
                       sizeof(entry->last_server_error_code),
                       reply->error_code);
    logger_copy_string(entry->last_server_error_message,
                       sizeof(entry->last_server_error_message),
                       reply->error_message);
  }
}

static void
logger_upload_set_entry_local_diagnostics(logger_upload_queue_entry_t *entry,
                                          const char *detail) {
  if (entry == NULL) {
    return;
  }
  logger_upload_clear_entry_diagnostics(entry);
  logger_upload_copy_response_excerpt(entry->last_response_excerpt,
                                      sizeof(entry->last_response_excerpt),
                                      detail);
}

static void logger_upload_copy_entry_diagnostics_to_result(
    const logger_upload_queue_entry_t *entry,
    logger_upload_process_result_t *result) {
  if (entry == NULL || result == NULL) {
    return;
  }
  result->http_status = entry->last_http_status > 0u
                            ? (int)entry->last_http_status
                            : result->http_status;
  logger_copy_string(result->server_error_code,
                     sizeof(result->server_error_code),
                     entry->last_server_error_code);
  logger_copy_string(result->server_error_message,
                     sizeof(result->server_error_message),
                     entry->last_server_error_message);
  logger_copy_string(result->response_excerpt, sizeof(result->response_excerpt),
                     entry->last_response_excerpt);
}

static const char *logger_upload_failure_class_for_http(
    int http_status, const logger_upload_server_reply_t *reply) {
  if (http_status == 426 ||
      strcmp(reply->error_code, "minimum_firmware") == 0) {
    return "min_firmware_rejected";
  }
  if (http_status == 422 ||
      strcmp(reply->error_code, "validation_failed") == 0) {
    return "server_validation_failed";
  }
  if (http_status == 401 || http_status == 403 ||
      strcmp(reply->error_code, "auth_failed") == 0 ||
      strcmp(reply->error_code, "unauthorized") == 0 ||
      strcmp(reply->error_code, "forbidden") == 0) {
    return "auth_failed";
  }
  if (http_status == 409 || strcmp(reply->error_code, "duplicate") == 0) {
    return "duplicate_rejected";
  }
  if (http_status >= 500) {
    return "server_error";
  }
  return "http_rejected";
}

static void logger_upload_queue_set_updated_at(logger_upload_queue_t *queue,
                                               const char *now_utc_or_null) {
  logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                     now_utc_or_null);
}

static logger_upload_queue_entry_t *
logger_upload_queue_find_next_eligible(logger_upload_queue_t *queue) {
  for (size_t i = 0u; i < queue->session_count; ++i) {
    if (strcmp(queue->sessions[i].status, "pending") == 0 ||
        strcmp(queue->sessions[i].status, "failed") == 0) {
      return &queue->sessions[i];
    }
  }
  return NULL;
}

static logger_upload_queue_entry_t *
logger_upload_queue_find_eligible_session(logger_upload_queue_t *queue,
                                          const char *session_id_or_null) {
  if (!logger_string_present(session_id_or_null)) {
    return logger_upload_queue_find_next_eligible(queue);
  }

  for (size_t i = 0u; i < queue->session_count; ++i) {
    logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (strcmp(entry->session_id, session_id_or_null) != 0) {
      continue;
    }
    if (strcmp(entry->status, "pending") == 0 ||
        strcmp(entry->status, "failed") == 0) {
      return entry;
    }
    return NULL;
  }
  return NULL;
}

static void logger_upload_append_failure_event(
    logger_system_log_t *system_log, const char *now_utc_or_null,
    const char *kind, logger_system_log_severity_t severity,
    const char *session_id, const char *failure_class) {
  if (system_log == NULL) {
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "session_id",
                                              session_id) ||
      !logger_json_object_writer_string_field(&writer, "failure_class",
                                              failure_class) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }

  (void)logger_system_log_append(system_log, now_utc_or_null, kind, severity,
                                 logger_json_object_writer_data(&writer));
}

static void logger_upload_append_verified_event(logger_system_log_t *system_log,
                                                const char *now_utc_or_null,
                                                const char *session_id,
                                                const char *receipt_id,
                                                bool deduplicated) {
  if (system_log == NULL) {
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "session_id",
                                              session_id) ||
      !logger_json_object_writer_string_field(&writer, "receipt_id",
                                              receipt_id) ||
      !logger_json_object_writer_bool_field(&writer, "deduplicated",
                                            deduplicated) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }

  (void)logger_system_log_append(system_log, now_utc_or_null, "upload_verified",
                                 LOGGER_SYSTEM_LOG_SEVERITY_INFO,
                                 logger_json_object_writer_data(&writer));
}

static bool
logger_upload_recompute_entry_bundle(logger_upload_queue_entry_t *entry,
                                     char *message, size_t message_len) {
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_path_join3(manifest_path, sizeof(manifest_path),
                         "0:/logger/sessions/", entry->dir_name,
                         "/manifest.json") ||
      !logger_path_join3(journal_path, sizeof(journal_path),
                         "0:/logger/sessions/", entry->dir_name,
                         "/journal.bin")) {
    logger_copy_string(message, message_len, "session path too long");
    return false;
  }
  if (!logger_storage_svc_file_exists(manifest_path) ||
      !logger_storage_svc_file_exists(journal_path)) {
    logger_copy_string(message, message_len, "session files missing");
    return false;
  }
  if (!logger_storage_svc_bundle_compute(entry->dir_name, manifest_path,
                                         journal_path, entry->bundle_sha256,
                                         &entry->bundle_size_bytes)) {
    logger_copy_string(message, message_len,
                       "failed to compute canonical bundle");
    return false;
  }
  return true;
}

bool logger_upload_net_test(const logger_config_t *config,
                            logger_upload_net_test_result_t *result) {
  logger_upload_net_test_result_init(result);

  if (!logger_config_wifi_configured(config)) {
    logger_upload_net_test_result_fail_all(
        result, "wifi credentials are not configured");
    return false;
  }
  if (!logger_config_upload_configured(config)) {
    logger_upload_net_test_result_fail_all(result,
                                           "upload URL is not configured");
    return false;
  }
  if (!logger_config_upload_ready(config)) {
    logger_upload_net_test_result_fail_all(result,
                                           "upload TLS trust is not ready");
    return false;
  }

  logger_upload_process_workspace_t *process_workspace =
      logger_upload_process_workspace_acquire();

  if (!logger_upload_parse_url(config->upload_url, &process_workspace->url)) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_net_test_result_fail_all(
        result, "upload URL is not a valid absolute http(s) URL");
    return false;
  }

  logger_upload_tls_trust_t tls_trust;
  char tls_select_message[LOGGER_UPLOAD_MESSAGE_MAX + 1u] = {0};
  if (process_workspace->url.https &&
      !logger_upload_select_tls_trust(config, &tls_trust, tls_select_message,
                                      sizeof(tls_select_message))) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_net_test_result_fail_all(result, tls_select_message);
    return false;
  }

  process_workspace->ip_buf[0] = '\0';
  int wifi_rc = 0;
  if (!logger_net_wifi_join(config, &wifi_rc, process_workspace->ip_buf)) {
    logger_upload_process_workspace_release(process_workspace);
    result->wifi_join_result = "fail";
    snprintf(result->wifi_join_details, sizeof(result->wifi_join_details),
             "ssid=%s rc=%d", config->wifi_ssid, wifi_rc);
    result->dns_result = "fail";
    logger_copy_string(result->dns_details, sizeof(result->dns_details),
                       "DNS not attempted because Wi-Fi join failed");
    result->tls_result =
        process_workspace->url.https ? "fail" : "not_applicable";
    logger_copy_string(result->tls_details, sizeof(result->tls_details),
                       process_workspace->url.https
                           ? "TLS not attempted because Wi-Fi join failed"
                           : "HTTP URL does not require TLS");
    result->upload_endpoint_reachable_result = "fail";
    logger_copy_string(
        result->upload_endpoint_reachable_details,
        sizeof(result->upload_endpoint_reachable_details),
        "endpoint probe not attempted because Wi-Fi join failed");
    return false;
  }

  result->wifi_join_result = "pass";
  snprintf(result->wifi_join_details, sizeof(result->wifi_join_details),
           "ssid=%s ip=%s", config->wifi_ssid, process_workspace->ip_buf);

  result->tls_result = process_workspace->url.https ? "fail" : "not_applicable";
  logger_copy_string(result->tls_details, sizeof(result->tls_details),
                     process_workspace->url.https
                         ? "TLS handshake not attempted yet"
                         : "HTTP upload URL does not use TLS");

  if (!logger_upload_format_host_header(
          &process_workspace->url, process_workspace->host_header,
          sizeof(process_workspace->host_header))) {
    logger_net_wifi_leave();
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_net_test_result_fail_all(result,
                                           "Host header buffer overflow");
    return false;
  }
  const int n = snprintf(
      process_workspace->request_text, sizeof(process_workspace->request_text),
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Connection: close\r\n"
      "\r\n",
      process_workspace->url.path, process_workspace->host_header);
  if (n <= 0 || (size_t)n >= sizeof(process_workspace->request_text)) {
    logger_net_wifi_leave();
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_net_test_result_fail_all(result,
                                           "probe request buffer overflow");
    return false;
  }

  logger_upload_http_response_t *probe_response =
      logger_upload_http_response_workspace_acquire();
  const bool reachable = logger_upload_http_execute(
      config, &process_workspace->url, process_workspace->request_text, NULL,
      LOGGER_UPLOAD_RESPONSE_TIMEOUT_MS, probe_response);
  if (process_workspace->url.host_is_literal) {
    result->dns_result = "pass";
    snprintf(result->dns_details, sizeof(result->dns_details), "literal=%s",
             process_workspace->url.host);
  } else if (logger_string_present(probe_response->remote_address)) {
    result->dns_result = "pass";
    snprintf(result->dns_details, sizeof(result->dns_details), "resolved=%s",
             probe_response->remote_address);
  } else {
    result->dns_result = "fail";
    logger_copy_string(result->dns_details, sizeof(result->dns_details),
                       "failed to resolve or connect to upload host");
  }

  if (process_workspace->url.https) {
    if (reachable) {
      result->tls_result = "pass";
      char suffix[96];
      snprintf(suffix, sizeof(suffix), "verified peer via TLS remote=%s",
               logger_string_present(probe_response->remote_address)
                   ? probe_response->remote_address
                   : "unknown");
      logger_upload_format_tls_details(&tls_trust, suffix, result->tls_details,
                                       sizeof(result->tls_details));
    } else {
      result->tls_result = "fail";
      logger_upload_format_tls_details(
          &tls_trust,
          strcmp(probe_response->transport_failure_class, "dns_failed") == 0
              ? "TLS handshake was not attempted because DNS resolution failed"
              : "TLS handshake or certificate verification failed",
          result->tls_details, sizeof(result->tls_details));
    }
  }

  if (reachable) {
    result->upload_endpoint_reachable_result = "pass";
    snprintf(result->upload_endpoint_reachable_details,
             sizeof(result->upload_endpoint_reachable_details),
             "%s_status=%d remote=%s",
             process_workspace->url.https ? "https" : "http",
             probe_response->http_status, probe_response->remote_address);
  } else {
    result->upload_endpoint_reachable_result = "fail";
    logger_copy_string(
        result->upload_endpoint_reachable_details,
        sizeof(result->upload_endpoint_reachable_details),
        process_workspace->url.https
            ? "failed to receive an HTTPS response from the upload endpoint"
            : "failed to receive an HTTP response from the upload endpoint");
  }

  logger_upload_http_response_workspace_release(probe_response);
  logger_upload_process_workspace_release(process_workspace);
  logger_net_wifi_leave();
  return reachable;
}

static bool logger_upload_process_selected(
    logger_system_log_t *system_log, const logger_config_t *config,
    const char *hardware_id, const char *now_utc_or_null,
    const char *target_session_id_or_null,
    logger_upload_process_result_t *result) {
  logger_upload_process_result_init(result);

  if (!logger_config_wifi_configured(config)) {
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "wifi credentials are not configured");
    return false;
  }
  if (!logger_config_upload_configured(config)) {
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "upload URL is not configured");
    return false;
  }
  if (!logger_config_upload_ready(config)) {
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "upload TLS trust is not ready");
    return false;
  }
  if (!logger_upload_auth_configured(config)) {
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "upload auth requires both api key and bearer token");
    return false;
  }

  logger_upload_process_workspace_t *process_workspace =
      logger_upload_process_workspace_acquire();

  if (!logger_upload_parse_url(config->upload_url, &process_workspace->url)) {
    logger_upload_process_workspace_release(process_workspace);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "upload URL is invalid");
    return false;
  }
  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "failed to load upload queue");
    return false;
  }

  logger_upload_queue_entry_t *entry =
      logger_upload_queue_find_eligible_session(queue,
                                                target_session_id_or_null);
  if (entry == NULL) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    logger_copy_string(result->session_id, sizeof(result->session_id),
                       target_session_id_or_null);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK;
    logger_copy_string(result->message, sizeof(result->message),
                       logger_string_present(target_session_id_or_null)
                           ? "session is no longer eligible for upload"
                           : "no pending uploads");
    return true;
  }
  logger_copy_string(result->session_id, sizeof(result->session_id),
                     entry->session_id);

  if (!logger_upload_recompute_entry_bundle(entry, result->message,
                                            sizeof(result->message))) {
    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    logger_copy_string(entry->status, sizeof(entry->status), "failed");
    logger_copy_string(entry->last_failure_class,
                       sizeof(entry->last_failure_class), "local_corrupt");
    logger_upload_set_entry_local_diagnostics(entry, result->message);
    logger_copy_string(entry->last_attempt_utc, sizeof(entry->last_attempt_utc),
                       now_utc_or_null);
    entry->attempt_count += 1u;
    (void)logger_storage_svc_queue_write(queue);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->attempted = true;
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class),
                       entry->last_failure_class);
    logger_upload_copy_entry_diagnostics_to_result(entry, result);
    logger_upload_append_failure_event(
        system_log, now_utc_or_null, "upload_failed",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id, "local_corrupt");
    return false;
  }

  if (!logger_path_join3(process_workspace->manifest_path,
                         sizeof(process_workspace->manifest_path),
                         "0:/logger/sessions/", entry->dir_name,
                         "/manifest.json") ||
      !logger_path_join3(process_workspace->journal_path,
                         sizeof(process_workspace->journal_path),
                         "0:/logger/sessions/", entry->dir_name,
                         "/journal.bin")) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "session path is too long");
    return false;
  }

  logger_upload_queue_set_updated_at(queue, now_utc_or_null);
  logger_copy_string(entry->status, sizeof(entry->status), "uploading");
  entry->attempt_count += 1u;
  logger_copy_string(entry->last_attempt_utc, sizeof(entry->last_attempt_utc),
                     now_utc_or_null);
  entry->last_failure_class[0] = '\0';
  logger_upload_clear_entry_diagnostics(entry);
  entry->verified_upload_utc[0] = '\0';
  entry->verified_bundle_sha256[0] = '\0';
  entry->receipt_id[0] = '\0';
  if (!logger_storage_svc_queue_write(queue)) {
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "failed to persist uploading queue state");
    return false;
  }

  process_workspace->ip_buf[0] = '\0';
  int wifi_rc = 0;
  if (!logger_net_wifi_join(config, &wifi_rc, process_workspace->ip_buf)) {
    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    logger_copy_string(entry->status, sizeof(entry->status), "failed");
    logger_copy_string(entry->last_failure_class,
                       sizeof(entry->last_failure_class), "wifi_join_failed");
    snprintf(result->message, sizeof(result->message),
             "Wi-Fi join failed rc=%d", wifi_rc);
    logger_upload_set_entry_local_diagnostics(entry, result->message);
    (void)logger_storage_svc_queue_write(queue);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->attempted = true;
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class),
                       entry->last_failure_class);
    logger_upload_copy_entry_diagnostics_to_result(entry, result);
    logger_upload_append_failure_event(
        system_log, now_utc_or_null, "upload_failed",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id, "wifi_join_failed");
    return false;
  }

  process_workspace->auth_header[0] = '\0';
  process_workspace->api_key_header[0] = '\0';
  snprintf(process_workspace->auth_header,
           sizeof(process_workspace->auth_header),
           "Authorization: Bearer %s\r\n", config->upload_token);
  snprintf(process_workspace->api_key_header,
           sizeof(process_workspace->api_key_header), "x-api-key: %s\r\n",
           config->upload_api_key);

  if (!logger_upload_format_host_header(
          &process_workspace->url, process_workspace->host_header,
          sizeof(process_workspace->host_header))) {
    logger_net_wifi_leave();
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "Host header buffer overflow");
    return false;
  }

  const int request_n = snprintf(
      process_workspace->request_text, sizeof(process_workspace->request_text),
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
      "%s"
      "Connection: close\r\n"
      "\r\n",
      process_workspace->url.path, process_workspace->host_header,
      (unsigned long long)entry->bundle_size_bytes, entry->session_id,
      hardware_id, config->logger_id, config->subject_id,
      entry->study_day_local, entry->bundle_sha256,
      process_workspace->auth_header, process_workspace->api_key_header);
  if (request_n <= 0 ||
      (size_t)request_n >= sizeof(process_workspace->request_text)) {
    logger_net_wifi_leave();
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED;
    logger_copy_string(result->message, sizeof(result->message),
                       "HTTP request buffer overflow");
    return false;
  }

  if (!logger_storage_svc_bundle_open(entry->dir_name,
                                      process_workspace->manifest_path,
                                      process_workspace->journal_path)) {
    logger_net_wifi_leave();
    logger_copy_string(entry->status, sizeof(entry->status), "failed");
    logger_copy_string(entry->last_failure_class,
                       sizeof(entry->last_failure_class), "local_corrupt");
    logger_upload_set_entry_local_diagnostics(
        entry, "failed to open canonical bundle stream");
    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    (void)logger_storage_svc_queue_write(queue);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->attempted = true;
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class),
                       entry->last_failure_class);
    logger_upload_copy_entry_diagnostics_to_result(entry, result);
    logger_copy_string(result->message, sizeof(result->message),
                       "failed to open canonical bundle stream");
    return false;
  }

  logger_upload_http_response_t *http_response =
      logger_upload_http_response_workspace_acquire();
  const bool http_ok = logger_upload_http_execute(
      config, &process_workspace->url, process_workspace->request_text, true,
      LOGGER_UPLOAD_RESPONSE_TIMEOUT_MS, http_response);
  logger_storage_svc_bundle_close();
  logger_net_wifi_leave();

  if (!http_ok) {
    const char *transport_failure_class =
        logger_string_present(http_response->transport_failure_class)
            ? http_response->transport_failure_class
            : (process_workspace->url.https ? "tls_failed" : "tcp_failed");
    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    logger_copy_string(entry->status, sizeof(entry->status), "failed");
    logger_copy_string(entry->last_failure_class,
                       sizeof(entry->last_failure_class),
                       transport_failure_class);
    logger_upload_set_entry_local_diagnostics(entry, transport_failure_class);
    (void)logger_storage_svc_queue_write(queue);
    logger_upload_http_response_workspace_release(http_response);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    result->attempted = true;
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class),
                       entry->last_failure_class);
    logger_upload_copy_entry_diagnostics_to_result(entry, result);
    logger_copy_string(result->message, sizeof(result->message),
                       process_workspace->url.https
                           ? "HTTPS upload transport failed"
                           : "HTTP upload transport failed");
    logger_upload_append_failure_event(
        system_log, now_utc_or_null, "upload_failed",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id,
        entry->last_failure_class);
    return false;
  }

  if (!logger_upload_parse_server_reply(http_response,
                                        &process_workspace->reply)) {
    const char *failure_class = logger_upload_failure_class_for_http(
        http_response->http_status, &process_workspace->reply);
    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    if (strcmp(failure_class, "min_firmware_rejected") == 0) {
      logger_copy_string(entry->status, sizeof(entry->status),
                         "blocked_min_firmware");
      result->code = LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
    } else {
      logger_copy_string(entry->status, sizeof(entry->status), "failed");
      result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
    }
    logger_copy_string(entry->last_failure_class,
                       sizeof(entry->last_failure_class), failure_class);
    logger_upload_set_entry_http_diagnostics(entry, http_response, NULL);
    (void)logger_storage_svc_queue_write(queue);
    result->attempted = true;
    result->http_status = http_response->http_status;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->failure_class, sizeof(result->failure_class),
                       entry->last_failure_class);
    logger_upload_copy_entry_diagnostics_to_result(entry, result);
    snprintf(result->message, sizeof(result->message),
             "server reply parse failed: %.104s", http_response->body);
    logger_upload_append_failure_event(
        system_log, now_utc_or_null,
        result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE
            ? "upload_blocked_min_firmware"
            : "upload_failed",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id, failure_class);
    logger_upload_http_response_workspace_release(http_response);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    return result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
  }

  result->attempted = true;
  result->http_status = http_response->http_status;
  if (http_response->http_status >= 200 && http_response->http_status < 300) {
    if ((logger_string_present(process_workspace->reply.session_id) &&
         strcmp(process_workspace->reply.session_id, entry->session_id) != 0) ||
        (logger_string_present(process_workspace->reply.sha256) &&
         strcmp(process_workspace->reply.sha256, entry->bundle_sha256) != 0) ||
        (process_workspace->reply.size_bytes != 0u &&
         process_workspace->reply.size_bytes != entry->bundle_size_bytes)) {
      logger_upload_queue_set_updated_at(queue, now_utc_or_null);
      logger_copy_string(entry->status, sizeof(entry->status), "failed");
      logger_copy_string(entry->last_failure_class,
                         sizeof(entry->last_failure_class), "hash_mismatch");
      logger_upload_set_entry_http_diagnostics(entry, http_response,
                                               &process_workspace->reply);
      (void)logger_storage_svc_queue_write(queue);
      logger_upload_http_response_workspace_release(http_response);
      logger_upload_process_workspace_release(process_workspace);
      logger_upload_queue_tmp_release(queue);
      result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
      logger_copy_string(result->final_status, sizeof(result->final_status),
                         entry->status);
      logger_copy_string(result->failure_class, sizeof(result->failure_class),
                         entry->last_failure_class);
      logger_upload_copy_entry_diagnostics_to_result(entry, result);
      logger_copy_string(result->message, sizeof(result->message),
                         "server acknowledgment did not match uploaded bundle");
      logger_upload_append_failure_event(
          system_log, now_utc_or_null, "upload_failed",
          LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id, "hash_mismatch");
      return false;
    }

    logger_upload_queue_set_updated_at(queue, now_utc_or_null);
    logger_copy_string(entry->status, sizeof(entry->status), "verified");
    entry->last_failure_class[0] = '\0';
    logger_upload_clear_entry_diagnostics(entry);
    logger_copy_string(
        entry->verified_upload_utc, sizeof(entry->verified_upload_utc),
        logger_string_present(process_workspace->reply.received_at_utc)
            ? process_workspace->reply.received_at_utc
            : now_utc_or_null);
    logger_copy_string(entry->receipt_id, sizeof(entry->receipt_id),
                       process_workspace->reply.receipt_id);
    logger_copy_string(entry->verified_bundle_sha256,
                       sizeof(entry->verified_bundle_sha256),
                       logger_string_present(process_workspace->reply.sha256)
                           ? process_workspace->reply.sha256
                           : entry->bundle_sha256);
    if (!logger_storage_svc_queue_write(queue)) {
      logger_upload_http_response_workspace_release(http_response);
      logger_upload_process_workspace_release(process_workspace);
      logger_upload_queue_tmp_release(queue);
      result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
      logger_copy_string(result->message, sizeof(result->message),
                         "upload succeeded but queue write failed");
      return false;
    }

    result->code = LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED;
    logger_copy_string(result->final_status, sizeof(result->final_status),
                       entry->status);
    logger_copy_string(result->receipt_id, sizeof(result->receipt_id),
                       entry->receipt_id);
    logger_copy_string(result->verified_upload_utc,
                       sizeof(result->verified_upload_utc),
                       entry->verified_upload_utc);
    snprintf(result->message, sizeof(result->message), "verified via %s",
             process_workspace->reply.deduplicated ? "deduplicated ack"
                                                   : "server ack");
    logger_upload_append_verified_event(system_log, now_utc_or_null,
                                        entry->session_id, entry->receipt_id,
                                        process_workspace->reply.deduplicated);
    logger_upload_http_response_workspace_release(http_response);
    logger_upload_process_workspace_release(process_workspace);
    logger_upload_queue_tmp_release(queue);
    return true;
  }

  const char *failure_class = logger_upload_failure_class_for_http(
      http_response->http_status, &process_workspace->reply);
  logger_upload_queue_set_updated_at(queue, now_utc_or_null);
  if (strcmp(failure_class, "min_firmware_rejected") == 0) {
    logger_copy_string(entry->status, sizeof(entry->status),
                       "blocked_min_firmware");
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
  } else {
    logger_copy_string(entry->status, sizeof(entry->status), "failed");
    result->code = LOGGER_UPLOAD_PROCESS_RESULT_FAILED;
  }
  logger_copy_string(entry->last_failure_class,
                     sizeof(entry->last_failure_class), failure_class);
  logger_upload_set_entry_http_diagnostics(entry, http_response,
                                           &process_workspace->reply);
  entry->verified_upload_utc[0] = '\0';
  entry->verified_bundle_sha256[0] = '\0';
  entry->receipt_id[0] = '\0';
  (void)logger_storage_svc_queue_write(queue);

  logger_copy_string(result->final_status, sizeof(result->final_status),
                     entry->status);
  logger_copy_string(result->failure_class, sizeof(result->failure_class),
                     entry->last_failure_class);
  logger_upload_copy_entry_diagnostics_to_result(entry, result);
  logger_copy_string(
      result->message, sizeof(result->message),
      logger_string_present(process_workspace->reply.error_message)
          ? process_workspace->reply.error_message
          : "server rejected upload");
  logger_upload_append_failure_event(
      system_log, now_utc_or_null,
      result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE
          ? "upload_blocked_min_firmware"
          : "upload_failed",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, entry->session_id,
      result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE
          ? "min_firmware_rejected"
          : failure_class);
  logger_upload_http_response_workspace_release(http_response);
  logger_upload_process_workspace_release(process_workspace);
  logger_upload_queue_tmp_release(queue);
  return result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE;
}

bool logger_upload_process_one(logger_system_log_t *system_log,
                               const logger_config_t *config,
                               const char *hardware_id,
                               const char *now_utc_or_null,
                               logger_upload_process_result_t *result) {
  return logger_upload_process_selected(system_log, config, hardware_id,
                                        now_utc_or_null, NULL, result);
}

bool logger_upload_process_session(logger_system_log_t *system_log,
                                   const logger_config_t *config,
                                   const char *hardware_id,
                                   const char *now_utc_or_null,
                                   const char *session_id,
                                   logger_upload_process_result_t *result) {
  return logger_upload_process_selected(system_log, config, hardware_id,
                                        now_utc_or_null, session_id, result);
}
