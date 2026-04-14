// SPDX-License-Identifier: MIT
#include "polar_sdk_session.h"

#include <string.h>

#ifndef POLAR_CFG_ENABLE_HR
#define POLAR_CFG_ENABLE_HR (1)
#endif

#ifndef POLAR_CFG_ENABLE_ECG
#define POLAR_CFG_ENABLE_ECG (1)
#endif

#ifndef POLAR_CFG_ENABLE_PSFTP
#define POLAR_CFG_ENABLE_PSFTP (0)
#endif

#ifndef POLAR_SDK_IMU_DEFAULT_RANGE_G
#define POLAR_SDK_IMU_DEFAULT_RANGE_G (8)
#endif

static polar_capability_mask_t polar_session_build_supported_mask(void) {
  polar_capability_mask_t mask = 0;
  if (POLAR_CFG_ENABLE_HR) {
    mask |= POLAR_CAP_STREAM_HR;
  }
  if (POLAR_CFG_ENABLE_ECG) {
    mask |= POLAR_CAP_STREAM_ECG;
    mask |= POLAR_CAP_STREAM_ACC;
  }
  if (POLAR_CFG_ENABLE_PSFTP) {
    mask |= POLAR_CAP_PSFTP_READ;
    mask |= POLAR_CAP_PSFTP_DELETE;
  }
  return mask;
}

static bool polar_stream_kind_is_build_supported(polar_stream_kind_t kind) {
  switch (kind) {
  case POLAR_STREAM_HR:
    return POLAR_CFG_ENABLE_HR;
  case POLAR_STREAM_ECG:
  case POLAR_STREAM_ACC:
    return POLAR_CFG_ENABLE_ECG;
  case POLAR_STREAM_PPG:
  case POLAR_STREAM_PPI:
  case POLAR_STREAM_GYRO:
  case POLAR_STREAM_MAG:
  default:
    return false;
  }
}

void polar_session_config_init(polar_session_config_t *cfg) {
  if (cfg == NULL) {
    return;
  }

  cfg->addr = NULL;
  cfg->name_prefix = NULL;
  cfg->required_capabilities = 0;
  cfg->buffers.stream_ring_bytes = 0;
  cfg->buffers.transfer_bytes = 0;
}

void polar_stream_config_init(polar_stream_config_t *cfg) {
  if (cfg == NULL) {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
}

polar_status_t polar_session_init(polar_session_t *s,
                                  const polar_session_config_t *cfg) {
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }

  memset(s, 0, sizeof(*s));
  s->state = POLAR_SESSION_STATE_IDLE;

  if (cfg != NULL) {
    s->required_capabilities = cfg->required_capabilities;
    s->buffers = cfg->buffers;
  }

  return POLAR_OK;
}

void polar_session_deinit(polar_session_t *s) {
  if (s == NULL) {
    return;
  }

  memset(s, 0, sizeof(*s));
  s->state = POLAR_SESSION_STATE_IDLE;
}

polar_session_state_t polar_session_state(const polar_session_t *s) {
  if (s == NULL) {
    return POLAR_SESSION_STATE_IDLE;
  }

  return s->state;
}

bool polar_session_is_connected(const polar_session_t *s) {
  return polar_session_state(s) == POLAR_SESSION_STATE_READY;
}

polar_status_t polar_session_connect(polar_session_t *s, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_session_disconnect(polar_session_t *s,
                                        uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  s->state = POLAR_SESSION_STATE_IDLE;
  return POLAR_OK;
}

polar_status_t
polar_session_set_required_capabilities(polar_session_t *s,
                                        polar_capability_mask_t required_mask) {
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  s->required_capabilities = required_mask;
  return POLAR_OK;
}

polar_capability_mask_t
polar_session_get_required_capabilities(const polar_session_t *s) {
  if (s == NULL) {
    return 0;
  }
  return s->required_capabilities;
}

polar_status_t polar_session_get_capabilities(const polar_session_t *s,
                                              polar_capabilities_t *out) {
  if (s == NULL || out == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }

  memset(out, 0, sizeof(*out));
  out->supported = polar_session_build_supported_mask();
  out->active_requirements = s->required_capabilities;
  out->max_parallel_pmd_streams = POLAR_CFG_ENABLE_ECG ? 2u : 0u;
  return POLAR_OK;
}

polar_status_t polar_stream_get_default_config(const polar_session_t *s,
                                               polar_stream_kind_t kind,
                                               polar_stream_config_t *out) {
  (void)s;
  if (out == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }

  polar_stream_config_init(out);
  if (!polar_stream_kind_is_build_supported(kind)) {
    return POLAR_ERR_UNSUPPORTED;
  }

  switch (kind) {
  case POLAR_STREAM_HR:
    return POLAR_OK;
  case POLAR_STREAM_ECG:
    out->fields = POLAR_STREAM_CFG_SAMPLE_RATE_HZ;
    out->sample_rate_hz = 130;
    return POLAR_OK;
  case POLAR_STREAM_ACC:
    out->fields = POLAR_STREAM_CFG_SAMPLE_RATE_HZ | POLAR_STREAM_CFG_RANGE;
    out->sample_rate_hz = 50;
    out->range = POLAR_SDK_IMU_DEFAULT_RANGE_G;
    return POLAR_OK;
  case POLAR_STREAM_PPG:
  case POLAR_STREAM_PPI:
  case POLAR_STREAM_GYRO:
  case POLAR_STREAM_MAG:
  default:
    return POLAR_ERR_UNSUPPORTED;
  }
}

polar_status_t polar_stream_start(polar_session_t *s, polar_stream_kind_t kind,
                                  polar_stream_format_t format,
                                  const polar_stream_config_t *cfg) {
  (void)kind;
  (void)format;
  (void)cfg;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_stream_stop(polar_session_t *s, polar_stream_kind_t kind) {
  (void)kind;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_stream_read(polar_session_t *s, polar_stream_kind_t kind,
                                 uint8_t *out, size_t out_len, size_t *out_n,
                                 uint32_t timeout_ms) {
  (void)kind;
  (void)out;
  (void)out_len;
  (void)timeout_ms;
  if (s == NULL || out == NULL || out_n == NULL || out_len == 0) {
    return POLAR_ERR_INVALID_ARG;
  }
  *out_n = 0;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_get_default_config(polar_session_t *s,
                                                  polar_recording_kind_t kind,
                                                  polar_stream_config_t *out) {
  (void)s;
  (void)kind;
  if (out == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  polar_stream_config_init(out);
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_start(polar_session_t *s,
                                     polar_recording_kind_t kind,
                                     const polar_stream_config_t *cfg) {
  (void)kind;
  (void)cfg;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_stop(polar_session_t *s,
                                    polar_recording_kind_t kind) {
  (void)kind;
  if (s == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_status(polar_session_t *s,
                                      polar_recording_status_t *out) {
  if (s == NULL || out == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_list(polar_session_t *s,
                                    polar_recording_info_t *out,
                                    size_t out_capacity, size_t *out_count) {
  (void)out;
  (void)out_capacity;
  if (s == NULL || out_count == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  *out_count = 0;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_recording_delete(polar_session_t *s,
                                      const char *recording_id) {
  if (s == NULL || recording_id == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_list_dir(polar_session_t *s, const char *path,
                                 polar_fs_entry_t *out, size_t out_capacity,
                                 size_t *out_count) {
  (void)out;
  (void)out_capacity;
  if (s == NULL || path == NULL || out_count == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  *out_count = 0;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_delete(polar_session_t *s, const char *path) {
  if (s == NULL || path == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_download(polar_session_t *s, const char *path,
                                 uint8_t *out, size_t out_capacity,
                                 size_t *out_len, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s == NULL || path == NULL || out == NULL || out_len == NULL ||
      out_capacity == 0) {
    return POLAR_ERR_INVALID_ARG;
  }
  *out_len = 0;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_download_open(polar_session_t *s, const char *path,
                                      polar_fs_download_handle_t *out_handle,
                                      uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s == NULL || path == NULL || out_handle == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  out_handle->handle_id = 0;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_download_read(polar_session_t *s,
                                      polar_fs_download_handle_t *handle,
                                      uint8_t *out, size_t out_capacity,
                                      size_t *out_len, bool *out_eof,
                                      uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s == NULL || handle == NULL || out == NULL || out_capacity == 0 ||
      out_len == NULL || out_eof == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  *out_len = 0;
  *out_eof = false;
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_fs_download_close(polar_session_t *s,
                                       polar_fs_download_handle_t *handle) {
  if (s == NULL || handle == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  return POLAR_ERR_UNSUPPORTED;
}

polar_status_t polar_stats_get(const polar_session_t *s, polar_stats_t *out) {
  if (s == NULL || out == NULL) {
    return POLAR_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  return POLAR_OK;
}