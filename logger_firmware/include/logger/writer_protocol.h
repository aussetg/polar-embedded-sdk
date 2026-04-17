#ifndef LOGGER_FIRMWARE_WRITER_PROTOCOL_H
#define LOGGER_FIRMWARE_WRITER_PROTOCOL_H

/*
 * Writer protocol — the ordered command boundary between the control
 * plane (core 0) and the storage writer (currently inline, later core 1).
 *
 * Every durable action that produces journal records or mutates session
 * state flows through one of these commands.  The control side decides
 * *what happened*; the writer side decides *how it becomes durable*.
 *
 * For now, commands execute inline on the calling core.  The point is to
 * establish the API boundary cleanly so the later move to a core-1 worker
 * requires zero changes to the callers in app_main.c.
 *
 * Ownership (from logger_capture_pipeline_v1.md §4.3):
 *
 *   Control core (core 0) owns:
 *     session_id, span_id, study_day_local, packet timestamps,
 *     per-span seq_in_span
 *
 *   Writer side owns:
 *     record_seq, chunk_seq_in_session, durable journal_size_bytes,
 *     actual durable emission order
 *
 * All command structs are static, bounded, and hot-path safe.
 * No heap allocation anywhere in this path.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/battery.h"
#include "logger/clock.h"
#include "logger/config_store.h"
#include "logger/faults.h"
#include "logger/storage.h"
#include "logger/system_log.h"

/* ── Command types ─────────────────────────────────────────────── */

typedef enum {
  LOGGER_WRITER_SESSION_START = 0,
  LOGGER_WRITER_SPAN_START,
  LOGGER_WRITER_APPEND_PMD_PACKET,
  LOGGER_WRITER_WRITE_MARKER,
  LOGGER_WRITER_WRITE_STATUS_SNAPSHOT,
  LOGGER_WRITER_WRITE_H10_BATTERY,
  LOGGER_WRITER_WRITE_GAP,
  LOGGER_WRITER_WRITE_CLOCK_EVENT,
  LOGGER_WRITER_WRITE_RECOVERY,
  LOGGER_WRITER_SPAN_END,
  LOGGER_WRITER_SESSION_END,
  LOGGER_WRITER_FINALIZE_SESSION,
  LOGGER_WRITER_REFRESH_LIVE,
  LOGGER_WRITER_FLUSH_BARRIER,
} logger_writer_cmd_type_t;

/* ── Inline payload capacity ───────────────────────────────────── */

/*
 * Must hold the largest PMD notification value.
 * H10 PMD max is 244 bytes (LOGGER_H10_PACKET_MAX_BYTES).
 * A little headroom doesn't hurt.
 */
#define LOGGER_WRITER_PAYLOAD_MAX 256u

/* ── Command structs ───────────────────────────────────────────── */

/*
 * Each command type has a specific parameter struct.
 * They are laid out so the common header (type + boot_counter + now_ms)
 * is always first, making dispatch uniform.
 *
 * Strings use fixed-size char arrays rather than pointers.
 * No pointers to transient stack or ring data — everything is
 * copied in at command construction time.
 */

/* ── SESSION_START ──────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_SESSION_START */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns; /* observed UTC nanoseconds from control-core clock */
  /* Control-core owned identity */
  char session_id[33];
  char study_day_local[11];
  char session_start_utc[32];
  char session_start_reason[32];
  char clock_state[8];
  /* Persisted config needed for the JSON payload */
  char logger_id[LOGGER_CONFIG_LOGGER_ID_MAX];
  char subject_id[LOGGER_CONFIG_SUBJECT_ID_MAX];
  char timezone[LOGGER_CONFIG_TIMEZONE_MAX];
} logger_writer_session_start_t;

/* ── SPAN_START ─────────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_SPAN_START */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char span_id[33];
  uint32_t span_index_in_session;
  char start_reason[32];
  char h10_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
  bool encrypted;
  bool bonded;
} logger_writer_span_start_t;

/* ── APPEND_PMD_PACKET ─────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_APPEND_PMD_PACKET */
  uint32_t boot_counter;
  uint32_t now_ms;
  uint16_t stream_kind;
  uint8_t span_id_raw[16];
  uint32_t seq_in_span;
  uint64_t mono_us;
  int64_t utc_ns;
  uint16_t value_len;
  uint8_t value[LOGGER_WRITER_PAYLOAD_MAX];
} logger_writer_append_pmd_packet_t;

/* ── WRITE_MARKER ──────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_MARKER */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char span_id[33];
} logger_writer_write_marker_t;

/* ── WRITE_STATUS_SNAPSHOT ──────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_STATUS_SNAPSHOT */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char active_span_id[33]; /* "" when no active span */
  uint16_t battery_voltage_mv;
  int16_t battery_estimate_pct;
  bool vbus_present;
  uint64_t sd_free_bytes;
  uint32_t sd_reserve_bytes;
  bool quarantined;
  char fault_code[32]; /* "null" or a fault name */
  char now_utc[32];    /* RFC 3339 UTC for live.json last_flush_utc */
} logger_writer_write_status_snapshot_t;

/* ── WRITE_H10_BATTERY ─────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_H10_BATTERY */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char span_id[33]; /* "" when no active span */
  uint8_t battery_percent;
  char read_reason[16];
} logger_writer_write_h10_battery_t;

/* ── WRITE_GAP ─────────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_GAP */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char ended_span_id[33];
  char gap_reason[32];
} logger_writer_write_gap_t;

/* ── WRITE_CLOCK_EVENT ─────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_CLOCK_EVENT */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char event_kind[16];
  int64_t delta_ns;
  int64_t old_utc_ns;
  int64_t new_utc_ns;
} logger_writer_write_clock_event_t;

/* ── WRITE_RECOVERY ────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_WRITE_RECOVERY */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char recovery_reason[32];
} logger_writer_write_recovery_t;

/* ── SPAN_END ──────────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_SPAN_END */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char span_id[33];
  char end_reason[32];
  uint32_t packet_count;
  uint32_t first_seq_in_span;
  uint32_t last_seq_in_span;
} logger_writer_span_end_t;

/* ── SESSION_END ───────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_SESSION_END */
  uint32_t boot_counter;
  uint32_t now_ms;
  int64_t utc_ns;
  char session_id[33];
  char end_reason[32];
  uint32_t span_count;
  bool quarantined;
  char quarantine_reasons[128]; /* pre-serialized JSON array */
} logger_writer_session_end_t;

/*
 * FINALIZE_SESSION: close journal, compute SHA-256, write manifest,
 * remove live.json, refresh upload queue.
 *
 * All manifest data comes from session->manifest_ctx (snapshotted
 * at session creation by core 0).  The command carries only the
 * fields that core 0 decides at finalize time.
 */
typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_FINALIZE_SESSION */
  uint32_t boot_counter;
  uint32_t now_ms;
  char now_utc[32]; /* RFC 3339 UTC for upload_queue.json updated_at_utc */
} logger_writer_finalize_session_t;

/* ── REFRESH_LIVE ──────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_REFRESH_LIVE */
  uint32_t boot_counter;
  uint32_t now_ms;
  char now_utc[32]; /* RFC 3339 UTC for live.json last_flush_utc */
} logger_writer_refresh_live_t;

/* ── FLUSH_BARRIER ─────────────────────────────────────────────── */

typedef struct {
  logger_writer_cmd_type_t type; /* LOGGER_WRITER_FLUSH_BARRIER */
  uint32_t boot_counter;
  uint32_t now_ms;
} logger_writer_flush_barrier_t;

/* ── Tagged union ──────────────────────────────────────────────── */

/*
 * One command slot.  Large enough for any single command.
 * No heap.  No pointers to ephemeral data.
 *
 * For the current inline execution path, commands are constructed
 * on the stack, dispatched, and discarded — the same pattern the
 * later SPSC-ring path will use, just without the ring.
 */
typedef union {
  logger_writer_cmd_type_t type;
  logger_writer_session_start_t session_start;
  logger_writer_span_start_t span_start;
  logger_writer_append_pmd_packet_t append_pmd_packet;
  logger_writer_write_marker_t write_marker;
  logger_writer_write_status_snapshot_t write_status_snapshot;
  logger_writer_write_h10_battery_t write_h10_battery;
  logger_writer_write_gap_t write_gap;
  logger_writer_write_clock_event_t write_clock_event;
  logger_writer_write_recovery_t write_recovery;
  logger_writer_span_end_t span_end;
  logger_writer_session_end_t session_end;
  logger_writer_finalize_session_t finalize_session;
  logger_writer_refresh_live_t refresh_live;
  logger_writer_flush_barrier_t flush_barrier;
} logger_writer_cmd_t;

/* ── Writer context ────────────────────────────────────────────── */

/*
 * Forward declaration.  The full session state struct is defined in
 * session.h — we only need an opaque pointer here.
 */
struct logger_session_state;
typedef struct logger_session_state logger_session_context_t;

/*
 * Writer dispatch function.
 *
 * Executes one command against the session context.
 * For now this runs inline on the calling core.
 * Returns true on success, false on storage/write failure.
 *
 * The dispatch function IS the API boundary.  Everything behind it
 * is writer-side implementation detail.  Everything in front of it
 * is control-plane decision-making.
 */
bool logger_writer_dispatch(logger_session_context_t *ctx,
                            const logger_writer_cmd_t *cmd);

/* ── Helper: command type name (for diagnostics) ───────────────── */

static inline const char *
logger_writer_cmd_type_name(logger_writer_cmd_type_t t) {
  switch (t) {
  case LOGGER_WRITER_SESSION_START:
    return "SESSION_START";
  case LOGGER_WRITER_SPAN_START:
    return "SPAN_START";
  case LOGGER_WRITER_APPEND_PMD_PACKET:
    return "APPEND_PMD_PACKET";
  case LOGGER_WRITER_WRITE_MARKER:
    return "WRITE_MARKER";
  case LOGGER_WRITER_WRITE_STATUS_SNAPSHOT:
    return "WRITE_STATUS_SNAPSHOT";
  case LOGGER_WRITER_WRITE_H10_BATTERY:
    return "WRITE_H10_BATTERY";
  case LOGGER_WRITER_WRITE_GAP:
    return "WRITE_GAP";
  case LOGGER_WRITER_WRITE_CLOCK_EVENT:
    return "WRITE_CLOCK_EVENT";
  case LOGGER_WRITER_WRITE_RECOVERY:
    return "WRITE_RECOVERY";
  case LOGGER_WRITER_SPAN_END:
    return "SPAN_END";
  case LOGGER_WRITER_SESSION_END:
    return "SESSION_END";
  case LOGGER_WRITER_FINALIZE_SESSION:
    return "FINALIZE_SESSION";
  case LOGGER_WRITER_REFRESH_LIVE:
    return "REFRESH_LIVE";
  case LOGGER_WRITER_FLUSH_BARRIER:
    return "FLUSH_BARRIER";
  default:
    return "UNKNOWN";
  }
}

#endif /* LOGGER_FIRMWARE_WRITER_PROTOCOL_H */
