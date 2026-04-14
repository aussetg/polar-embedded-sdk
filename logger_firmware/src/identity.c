#include "logger/identity.h"

#include <stdint.h>

#include "pico/unique_id.h"

static uint64_t logger_fnv1a64_with_salt(const uint8_t *data, size_t len,
                                         uint64_t salt) {
  uint64_t hash = 14695981039346656037ull ^ salt;
  for (size_t i = 0; i < len; ++i) {
    hash ^= (uint64_t)data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static char logger_hex_nibble(unsigned value) {
  value &= 0x0fu;
  return (char)(value < 10u ? ('0' + value) : ('a' + (value - 10u)));
}

void logger_identity_read_hardware_id_hex(
    char out_hex[LOGGER_HARDWARE_ID_HEX_LEN + 1]) {
  pico_unique_board_id_t board_id;
  pico_get_unique_board_id(&board_id);

  const uint64_t h0 = logger_fnv1a64_with_salt(board_id.id, sizeof(board_id.id),
                                               0x6c6f676765723031ull);
  const uint64_t h1 = logger_fnv1a64_with_salt(board_id.id, sizeof(board_id.id),
                                               0x6c6f676765723032ull);

  const uint64_t parts[2] = {h0, h1};
  size_t out_ix = 0;
  for (size_t part_ix = 0; part_ix < 2; ++part_ix) {
    for (int byte_ix = 7; byte_ix >= 0; --byte_ix) {
      uint8_t byte = (uint8_t)((parts[part_ix] >> (byte_ix * 8)) & 0xffu);
      out_hex[out_ix++] = logger_hex_nibble((unsigned)(byte >> 4));
      out_hex[out_ix++] = logger_hex_nibble((unsigned)byte);
    }
  }
  out_hex[out_ix] = '\0';
}
