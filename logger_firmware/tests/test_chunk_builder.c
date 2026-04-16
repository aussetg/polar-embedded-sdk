/*
 * Host-side tests for chunk_builder.
 *
 * Compile:
 *   gcc -Wall -Wextra -I include/ tests/test_chunk_builder.c \
 *       src/chunk_builder.c -o test_chunk_builder
 *
 * Run:
 *   ./test_chunk_builder
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logger/chunk_builder.h"

/* ── little-endian readers (for verifying sealed payloads) ─────── */

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= (uint64_t)p[i] << (8 * i);
  return v;
}

/* ── helpers ───────────────────────────────────────────────────── */

static const uint8_t span_a[16] = {0x8d, 0x3c, 0xf8, 0xd4, 0xd4, 0x56,
                                   0x4d, 0x0f, 0x83, 0xb3, 0xd2, 0xd6,
                                   0xbb, 0x39, 0x8d, 0x2a};

static const uint8_t span_b[16] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                                   0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                   0x66, 0x77, 0x88, 0x99};

/* ── test: init / append / seal / clear lifecycle ──────────────── */

static void test_basic_lifecycle(void) {
  printf("  basic_lifecycle...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[] = {0x01, 0x02, 0x03, 0x04};
  logger_chunk_result_t r = logger_chunk_builder_append(
      &cb, 1, span_a, 0, 1000000, 1000000000LL, value, 4, 1000);
  assert(r == LOGGER_CHUNK_OK);
  assert(logger_chunk_builder_has_data(&cb));
  assert(cb.packet_count == 1);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &payload, &plen));
  assert(payload != NULL);

  /* entry: 28 + 4 = 32, aligned = 32 */
  const size_t entry_len = 32u;
  assert(plen == LOGGER_CHUNK_HEADER_BYTES + entry_len);

  /* header spot-checks */
  assert(rd16(payload + 0) == 1); /* stream_kind ECG */
  assert(rd16(payload + 2) == 1); /* encoding raw_pmd_v1 */
  assert(rd32(payload + 4) == 0); /* chunk_seq */
  assert(memcmp(payload + 8, span_a, 16) == 0);
  assert(rd32(payload + 24) == 1); /* packet_count */
  assert(rd32(payload + 28) == 0); /* first_seq */
  assert(rd32(payload + 32) == 0); /* last_seq */
  assert(rd64(payload + 40) == 1000000);
  assert(rd64(payload + 56) == 1000000000LL);
  assert(rd32(payload + 72) == entry_len); /* entries_bytes */

  /* entry spot-checks */
  assert(rd32(payload + 80 + 0) == 0); /* seq_in_span */
  assert(rd64(payload + 80 + 8) == 1000000);
  assert(rd16(payload + 80 + 24) == 4); /* value_len */
  assert(memcmp(payload + 80 + 28, value, 4) == 0);

  /* clear preserves stream/span */
  logger_chunk_builder_clear(&cb);
  assert(!logger_chunk_builder_has_data(&cb));
  assert(cb.stream_kind == 1);
  assert(memcmp(cb.span_id_raw, span_a, 16) == 0);

  printf(" PASS\n");
}

/* ── test: multiple packets in one chunk ───────────────────────── */

static void test_multi_packet(void) {
  printf("  multi_packet...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[] = {0xDE, 0xAD};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 10, 1000, 1000000LL, value,
                                     2, 5000) == LOGGER_CHUNK_OK);
  assert(logger_chunk_builder_append(&cb, 1, span_a, 11, 2000, 2000000LL, value,
                                     2, 5001) == LOGGER_CHUNK_OK);
  assert(logger_chunk_builder_append(&cb, 1, span_a, 12, 3000, 3000000LL, value,
                                     2, 5002) == LOGGER_CHUNK_OK);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 3, &payload, &plen));

  /* 3 entries of (28+2)=30, aligned to 32 each = 96 bytes entries */
  assert(rd32(payload + 24) == 3);         /* packet_count */
  assert(rd32(payload + 28) == 10);        /* first_seq */
  assert(rd32(payload + 32) == 12);        /* last_seq */
  assert(rd32(payload + 4) == 3);          /* chunk_seq */
  assert(rd64(payload + 40) == 1000);      /* first_mono */
  assert(rd64(payload + 48) == 3000);      /* last_mono */
  assert(rd64(payload + 56) == 1000000LL); /* first_utc */
  assert(rd64(payload + 64) == 3000000LL); /* last_utc */
  assert(rd32(payload + 72) == 96);        /* entries_bytes */
  assert(plen == 80 + 96);

  /* verify packet order: seq_in_span strictly increasing */
  assert(rd32(payload + 80 + 0) == 10);
  assert(rd32(payload + 80 + 32 + 0) == 11);
  assert(rd32(payload + 80 + 64 + 0) == 12);

  printf(" PASS\n");
}

/* ── test: size seal trigger ───────────────────────────────────── */

static void test_size_seal(void) {
  printf("  size_seal...");

  uint8_t buf[LOGGER_CHUNK_TARGET_SIZE + 1024];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {0xAA, 0xBB, 0xCC, 0xDD};

  logger_chunk_result_t r = LOGGER_CHUNK_OK;
  uint32_t count = 0u;
  while (r == LOGGER_CHUNK_OK) {
    r = logger_chunk_builder_append(
        &cb, 1, span_a, count, (uint64_t)count * 1000u,
        (int64_t)count * 1000000LL, value, sizeof(value), 1000);
    count++;
  }
  assert(r == LOGGER_CHUNK_SEAL);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &payload, &plen));
  assert(plen >= LOGGER_CHUNK_TARGET_SIZE);
  assert(plen < LOGGER_CHUNK_TARGET_SIZE + 64); /* shouldn't overshoot much */

  /* packet_count matches total appends (SEAL still adds the packet) */
  assert(rd32(payload + 24) == count);

  printf(" PASS (sealed at %zu bytes, %u packets)\n", plen, count - 1);
}

/* ── test: time seal trigger ───────────────────────────────────── */

static void test_time_seal(void) {
  printf("  time_seal...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value,
                                     4, 1000) == LOGGER_CHUNK_OK);

  /* Exactly at boundary: not exceeded yet (>= required) */
  assert(!logger_chunk_builder_age_exceeded(&cb, 60999));
  assert(logger_chunk_builder_age_exceeded(&cb, 61000));
  assert(logger_chunk_builder_age_exceeded(&cb, 120000));

  /* No data → age never exceeded */
  logger_chunk_builder_clear(&cb);
  assert(!logger_chunk_builder_age_exceeded(&cb, 999999));

  printf(" PASS\n");
}

/* ── test: barrier seal (manual seal with data) ────────────────── */

static void test_barrier_seal(void) {
  printf("  barrier_seal...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value,
                                     4, 1000) == LOGGER_CHUNK_OK);
  assert(logger_chunk_builder_append(&cb, 1, span_a, 1, 2000, 2000000LL, value,
                                     4, 1001) == LOGGER_CHUNK_OK);

  /* Manual barrier seal */
  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 5, &payload, &plen));
  assert(rd32(payload + 24) == 2);
  assert(rd32(payload + 4) == 5); /* chunk_seq */
  assert(plen == 80 + 64);        /* 2 × 32-byte entries */

  logger_chunk_builder_clear(&cb);
  assert(!logger_chunk_builder_has_data(&cb));

  printf(" PASS\n");
}

/* ── test: empty seal returns false ────────────────────────────── */

static void test_empty_seal(void) {
  printf("  empty_seal...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(!logger_chunk_builder_seal(&cb, 0, &payload, &plen));

  printf(" PASS\n");
}

/* ── test: FULL when buffer is too small ───────────────────────── */

static void test_full(void) {
  printf("  full...");

  /* header(80) + 2 entries of 32 bytes = 144 bytes */
  uint8_t buf[144];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};

  /* First two entries fit: 80 + 32 + 32 = 144 */
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value,
                                     4, 1000) == LOGGER_CHUNK_OK);
  assert(logger_chunk_builder_append(&cb, 1, span_a, 1, 2000, 2000000LL, value,
                                     4, 1001) == LOGGER_CHUNK_OK);

  /* Third entry would push to 176 > 144 */
  assert(logger_chunk_builder_append(&cb, 1, span_a, 2, 3000, 3000000LL, value,
                                     4, 1002) == LOGGER_CHUNK_FULL);

  /* Seal + clear + retry succeeds */
  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &payload, &plen));
  assert(plen == 144);
  logger_chunk_builder_clear(&cb);

  assert(logger_chunk_builder_append(&cb, 1, span_a, 2, 3000, 3000000LL, value,
                                     4, 1002) == LOGGER_CHUNK_OK);

  printf(" PASS\n");
}

/* ── test: stream/span change triggers FULL ────────────────────── */

static void test_stream_change(void) {
  printf("  stream_change...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};

  /* Start with ECG */
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value,
                                     4, 1000) == LOGGER_CHUNK_OK);

  /* ACC on same span → FULL (different stream) */
  assert(logger_chunk_builder_append(&cb, 2, span_a, 1, 2000, 2000000LL, value,
                                     4, 1001) == LOGGER_CHUNK_FULL);

  /* Same stream, different span → FULL */
  assert(logger_chunk_builder_append(&cb, 1, span_b, 0, 3000, 3000000LL, value,
                                     4, 1002) == LOGGER_CHUNK_FULL);

  /* Seal + clear + append with new stream works */
  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &payload, &plen));
  logger_chunk_builder_clear(&cb);

  assert(logger_chunk_builder_append(&cb, 2, span_b, 0, 3000, 3000000LL, value,
                                     4, 1003) == LOGGER_CHUNK_OK);
  assert(cb.stream_kind == 2);
  assert(memcmp(cb.span_id_raw, span_b, 16) == 0);

  printf(" PASS\n");
}

/* ── test: sealed payload matches old single-packet format ─────── */

static void test_format_match(void) {
  printf("  format_match...");

  uint8_t buf[512];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 42, 999888777ULL,
                                     1234567890123456789LL, value,
                                     sizeof(value), 5000) == LOGGER_CHUNK_OK);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 7, &payload, &plen));

  /* Build reference payload the old way */
  uint8_t ref[512];
  memset(ref, 0, sizeof(ref));

  /* chunk header */
  ref[0] = 1;
  ref[1] = 0; /* stream_kind = 1 */
  ref[2] = 1;
  ref[3] = 0; /* encoding = 1 */
  ref[4] = 7;
  ref[5] = 0;
  ref[6] = 0;
  ref[7] = 0; /* chunk_seq = 7 */
  memcpy(ref + 8, span_a, 16);
  ref[24] = 1;
  ref[25] = 0;
  ref[26] = 0;
  ref[27] = 0; /* packet_count = 1 */
  ref[28] = 42;
  ref[29] = 0;
  ref[30] = 0;
  ref[31] = 0; /* first_seq */
  ref[32] = 42;
  ref[33] = 0;
  ref[34] = 0;
  ref[35] = 0; /* last_seq */
  /* first_mono_us */
  uint64_t mono = 999888777ULL;
  for (int i = 0; i < 8; i++)
    ref[40 + i] = (uint8_t)(mono >> (8 * i));
  /* last_mono_us */
  for (int i = 0; i < 8; i++)
    ref[48 + i] = (uint8_t)(mono >> (8 * i));
  /* first_utc_ns */
  uint64_t utc = 1234567890123456789ULL;
  for (int i = 0; i < 8; i++)
    ref[56 + i] = (uint8_t)(utc >> (8 * i));
  /* last_utc_ns */
  for (int i = 0; i < 8; i++)
    ref[64 + i] = (uint8_t)(utc >> (8 * i));
  /* entries_bytes: 28 + 6 = 34, aligned = 36 */
  ref[72] = 36;
  ref[73] = 0;
  ref[74] = 0;
  ref[75] = 0;

  /* entry header */
  ref[80 + 0] = 42;
  ref[81] = 0;
  ref[82] = 0;
  ref[83] = 0; /* seq */
  /* flags = 0 */
  for (int i = 0; i < 8; i++)
    ref[80 + 8 + i] = (uint8_t)(mono >> (8 * i));
  for (int i = 0; i < 8; i++)
    ref[80 + 16 + i] = (uint8_t)(utc >> (8 * i));
  ref[80 + 24] = 6;
  ref[80 + 25] = 0; /* value_len */
  memcpy(ref + 80 + 28, value, 6);
  /* padding at 80+34..80+35 already zeroed */

  assert(plen == 80 + 36);
  assert(memcmp(payload, ref, plen) == 0);

  printf(" PASS\n");
}

/* ── test: reset discards data and stream/span identity ────────── */

static void test_reset(void) {
  printf("  reset...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value,
                                     4, 1000) == LOGGER_CHUNK_OK);

  logger_chunk_builder_reset(&cb);
  assert(!logger_chunk_builder_has_data(&cb));
  assert(cb.stream_kind == 0);
  assert(cb.packet_count == 0);

  /* After reset, new identity is captured from first append */
  assert(logger_chunk_builder_append(&cb, 2, span_b, 10, 2000, 2000000LL, value,
                                     4, 2000) == LOGGER_CHUNK_OK);
  assert(cb.stream_kind == 2);
  assert(memcmp(cb.span_id_raw, span_b, 16) == 0);
  assert(cb.first_seq_in_span == 10);

  printf(" PASS\n");
}

/* ── test: value with non-aligned length gets padded correctly ── */

static void test_entry_padding(void) {
  printf("  entry_padding...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  /* 5-byte value → entry = 28+5=33, aligned to 36 */
  uint8_t value5[] = {0x11, 0x22, 0x33, 0x44, 0x55};
  assert(logger_chunk_builder_append(&cb, 1, span_a, 0, 1000, 1000000LL, value5,
                                     5, 1000) == LOGGER_CHUNK_OK);

  const uint8_t *payload = NULL;
  size_t plen = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &payload, &plen));
  assert(plen == 80 + 36);

  /* entries_bytes should be 36 */
  assert(rd32(payload + 72) == 36);

  /* value is at offset 80+28=108, 5 bytes, then 3 bytes padding (zero) */
  assert(memcmp(payload + 108, value5, 5) == 0);
  assert(payload[113] == 0);
  assert(payload[114] == 0);
  assert(payload[115] == 0);

  printf(" PASS\n");
}

/* ── test: clear after seal allows continuation in same span ──── */

static void test_seal_clear_continue(void) {
  printf("  seal_clear_continue...");

  uint8_t buf[4096];
  logger_chunk_builder_t cb;
  logger_chunk_builder_init(&cb, buf, sizeof(buf));
  logger_chunk_builder_reset(&cb);

  uint8_t value[4] = {1, 2, 3, 4};

  /* First chunk: seq 0..4 */
  for (uint32_t i = 0; i < 5; i++) {
    assert(logger_chunk_builder_append(&cb, 1, span_a, i, (uint64_t)i * 1000,
                                       (int64_t)i * 1000000LL, value, 4,
                                       1000 + i) == LOGGER_CHUNK_OK);
  }
  const uint8_t *p1 = NULL;
  size_t l1 = 0;
  assert(logger_chunk_builder_seal(&cb, 0, &p1, &l1));
  assert(rd32(p1 + 24) == 5);
  assert(rd32(p1 + 28) == 0);
  assert(rd32(p1 + 32) == 4);

  logger_chunk_builder_clear(&cb);

  /* Second chunk: seq 5..9 — same span, builder still knows it */
  for (uint32_t i = 5; i < 10; i++) {
    assert(logger_chunk_builder_append(&cb, 1, span_a, i, (uint64_t)i * 1000,
                                       (int64_t)i * 1000000LL, value, 4,
                                       2000 + i) == LOGGER_CHUNK_OK);
  }
  const uint8_t *p2 = NULL;
  size_t l2 = 0;
  assert(logger_chunk_builder_seal(&cb, 1, &p2, &l2));
  assert(rd32(p2 + 24) == 5);
  assert(rd32(p2 + 28) == 5);
  assert(rd32(p2 + 32) == 9);
  assert(rd32(p2 + 4) == 1);               /* chunk_seq = 1 */
  assert(memcmp(p2 + 8, span_a, 16) == 0); /* same span */

  printf(" PASS\n");
}

/* ── main ──────────────────────────────────────────────────────── */

int main(void) {
  printf("chunk_builder tests:\n");
  test_basic_lifecycle();
  test_multi_packet();
  test_size_seal();
  test_time_seal();
  test_barrier_seal();
  test_empty_seal();
  test_full();
  test_stream_change();
  test_format_match();
  test_reset();
  test_entry_padding();
  test_seal_clear_continue();
  printf("all tests passed.\n");
  return 0;
}
