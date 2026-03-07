// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_SESSION_H
#define POLAR_SDK_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SESSION_STATE_IDLE = 0,
    POLAR_SESSION_STATE_SCANNING,
    POLAR_SESSION_STATE_CONNECTING,
    POLAR_SESSION_STATE_DISCOVERING,
    POLAR_SESSION_STATE_READY,
    POLAR_SESSION_STATE_RECOVERING,
} polar_session_state_t;

typedef enum {
    POLAR_STREAM_HR = 0,
    POLAR_STREAM_ECG,
    POLAR_STREAM_ACC,
    POLAR_STREAM_PPG,
    POLAR_STREAM_PPI,
    POLAR_STREAM_GYRO,
    POLAR_STREAM_MAG,
} polar_stream_kind_t;

typedef uint32_t polar_stream_kind_mask_t;

enum {
    POLAR_STREAM_KIND_MASK_HR   = (1u << POLAR_STREAM_HR),
    POLAR_STREAM_KIND_MASK_ECG  = (1u << POLAR_STREAM_ECG),
    POLAR_STREAM_KIND_MASK_ACC  = (1u << POLAR_STREAM_ACC),
    POLAR_STREAM_KIND_MASK_PPG  = (1u << POLAR_STREAM_PPG),
    POLAR_STREAM_KIND_MASK_PPI  = (1u << POLAR_STREAM_PPI),
    POLAR_STREAM_KIND_MASK_GYRO = (1u << POLAR_STREAM_GYRO),
    POLAR_STREAM_KIND_MASK_MAG  = (1u << POLAR_STREAM_MAG),
};

typedef enum {
    POLAR_RECORDING_KIND_HR = 0,
    POLAR_RECORDING_KIND_ECG,
    POLAR_RECORDING_KIND_ACC,
    POLAR_RECORDING_KIND_PPG,
    POLAR_RECORDING_KIND_PPI,
    POLAR_RECORDING_KIND_GYRO,
    POLAR_RECORDING_KIND_MAG,
    POLAR_RECORDING_KIND_TEMPERATURE,
    POLAR_RECORDING_KIND_SKIN_TEMP,
} polar_recording_kind_t;

typedef uint32_t polar_recording_kind_mask_t;

enum {
    POLAR_RECORDING_KIND_MASK_HR          = (1u << POLAR_RECORDING_KIND_HR),
    POLAR_RECORDING_KIND_MASK_ECG         = (1u << POLAR_RECORDING_KIND_ECG),
    POLAR_RECORDING_KIND_MASK_ACC         = (1u << POLAR_RECORDING_KIND_ACC),
    POLAR_RECORDING_KIND_MASK_PPG         = (1u << POLAR_RECORDING_KIND_PPG),
    POLAR_RECORDING_KIND_MASK_PPI         = (1u << POLAR_RECORDING_KIND_PPI),
    POLAR_RECORDING_KIND_MASK_GYRO        = (1u << POLAR_RECORDING_KIND_GYRO),
    POLAR_RECORDING_KIND_MASK_MAG         = (1u << POLAR_RECORDING_KIND_MAG),
    POLAR_RECORDING_KIND_MASK_TEMPERATURE = (1u << POLAR_RECORDING_KIND_TEMPERATURE),
    POLAR_RECORDING_KIND_MASK_SKIN_TEMP   = (1u << POLAR_RECORDING_KIND_SKIN_TEMP),
};

typedef enum {
    POLAR_STREAM_FORMAT_DEFAULT = 0,
    POLAR_STREAM_FORMAT_DECODED,
    POLAR_STREAM_FORMAT_RAW,
} polar_stream_format_t;

typedef enum {
    POLAR_TIME_BASE_UNKNOWN = 0,
    POLAR_TIME_BASE_POLAR_DEVICE,
    POLAR_TIME_BASE_UNIX,
} polar_time_base_t;

typedef enum {
    POLAR_RECORDING_STATE_UNKNOWN = 0,
    POLAR_RECORDING_STATE_ACTIVE,
    POLAR_RECORDING_STATE_STOPPED,
} polar_recording_state_t;

typedef enum {
    POLAR_STREAM_CFG_SAMPLE_RATE_HZ  = (1u << 0),
    POLAR_STREAM_CFG_RESOLUTION_BITS = (1u << 1),
    POLAR_STREAM_CFG_RANGE           = (1u << 2),
    POLAR_STREAM_CFG_CHANNELS        = (1u << 3),
} polar_stream_config_field_t;

typedef struct {
    uint32_t fields;
    uint16_t sample_rate_hz;
    uint8_t resolution_bits;
    int16_t range;
    uint8_t channels;
} polar_stream_config_t;

typedef struct {
    size_t stream_ring_bytes;
    size_t transfer_bytes;
} polar_buffer_config_t;

typedef uint64_t polar_capability_mask_t;

enum {
    POLAR_CAP_STREAM_HR    = (1ull << 0),
    POLAR_CAP_STREAM_ECG   = (1ull << 1),
    POLAR_CAP_STREAM_ACC   = (1ull << 2),
    POLAR_CAP_STREAM_PPG   = (1ull << 3),
    POLAR_CAP_STREAM_PPI   = (1ull << 4),
    POLAR_CAP_STREAM_GYRO  = (1ull << 5),
    POLAR_CAP_STREAM_MAG   = (1ull << 6),
    POLAR_CAP_RECORDING    = (1ull << 16),
    POLAR_CAP_PSFTP_READ   = (1ull << 17),
    POLAR_CAP_PSFTP_DELETE = (1ull << 18),
};

#define POLAR_FS_NAME_MAX_BYTES      96
#define POLAR_PATH_MAX_BYTES         128
#define POLAR_RECORDING_ID_MAX_BYTES 128

typedef struct {
    const char *addr;
    const char *name_prefix;
    polar_capability_mask_t required_capabilities;
    polar_buffer_config_t buffers;
} polar_session_config_t;

typedef struct {
    polar_capability_mask_t supported;
    polar_capability_mask_t active_requirements;
    uint8_t max_parallel_pmd_streams;
} polar_capabilities_t;

typedef struct {
    polar_recording_kind_mask_t active_kinds;
} polar_recording_status_t;

typedef struct {
    char name[POLAR_FS_NAME_MAX_BYTES];
    uint64_t size;
    bool is_dir;
} polar_fs_entry_t;

typedef struct {
    char recording_id[POLAR_RECORDING_ID_MAX_BYTES];
    polar_recording_kind_t kind;
    polar_recording_state_t state;
    polar_time_base_t time_base;
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    uint64_t bytes_total;
    uint32_t sample_count;
    bool has_start_time;
    bool has_end_time;
    bool has_bytes_total;
    bool has_sample_count;
    char path[POLAR_PATH_MAX_BYTES];
} polar_recording_info_t;

typedef struct {
    uint32_t handle_id;
} polar_fs_download_handle_t;

typedef struct {
    uint32_t reserved;
} polar_stats_t;

typedef struct {
    polar_session_state_t state;
    polar_capability_mask_t required_capabilities;
    polar_buffer_config_t buffers;
} polar_session_t;

void polar_session_config_init(polar_session_config_t *cfg);
void polar_stream_config_init(polar_stream_config_t *cfg);

polar_status_t polar_session_init(polar_session_t *s, const polar_session_config_t *cfg);
void           polar_session_deinit(polar_session_t *s);

polar_session_state_t polar_session_state(const polar_session_t *s);
bool                  polar_session_is_connected(const polar_session_t *s);

polar_status_t polar_session_connect(polar_session_t *s, uint32_t timeout_ms);
polar_status_t polar_session_disconnect(polar_session_t *s, uint32_t timeout_ms);

polar_status_t polar_session_set_required_capabilities(
    polar_session_t *s,
    polar_capability_mask_t required_mask);

polar_capability_mask_t polar_session_get_required_capabilities(
    const polar_session_t *s);

polar_status_t polar_session_get_capabilities(
    const polar_session_t *s,
    polar_capabilities_t *out);

polar_status_t polar_stream_get_default_config(
    const polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_config_t *out);

polar_status_t polar_stream_start(
    polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_format_t format,
    const polar_stream_config_t *cfg);

polar_status_t polar_stream_stop(
    polar_session_t *s,
    polar_stream_kind_t kind);

polar_status_t polar_stream_read(
    polar_session_t *s,
    polar_stream_kind_t kind,
    uint8_t *out,
    size_t out_len,
    size_t *out_n,
    uint32_t timeout_ms);

polar_status_t polar_recording_get_default_config(
    polar_session_t *s,
    polar_recording_kind_t kind,
    polar_stream_config_t *out);

polar_status_t polar_recording_start(
    polar_session_t *s,
    polar_recording_kind_t kind,
    const polar_stream_config_t *cfg);

polar_status_t polar_recording_stop(
    polar_session_t *s,
    polar_recording_kind_t kind);

polar_status_t polar_recording_status(
    polar_session_t *s,
    polar_recording_status_t *out);

polar_status_t polar_recording_list(
    polar_session_t *s,
    polar_recording_info_t *out,
    size_t out_capacity,
    size_t *out_count);

polar_status_t polar_recording_delete(
    polar_session_t *s,
    const char *recording_id);

polar_status_t polar_fs_list_dir(
    polar_session_t *s,
    const char *path,
    polar_fs_entry_t *out,
    size_t out_capacity,
    size_t *out_count);

polar_status_t polar_fs_delete(
    polar_session_t *s,
    const char *path);

polar_status_t polar_fs_download(
    polar_session_t *s,
    const char *path,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_open(
    polar_session_t *s,
    const char *path,
    polar_fs_download_handle_t *out_handle,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_read(
    polar_session_t *s,
    polar_fs_download_handle_t *handle,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len,
    bool *out_eof,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_close(
    polar_session_t *s,
    polar_fs_download_handle_t *handle);

polar_status_t polar_stats_get(const polar_session_t *s, polar_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_SESSION_H