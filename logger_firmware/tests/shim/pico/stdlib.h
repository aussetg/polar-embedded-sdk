/* Minimal pico-sdk stdlib shim for host-side capture_pipe tests.
 * Provides just enough types to compile capture_pipe.c on host. */
#ifndef _PICO_STDLIB_H
#define _PICO_STDLIB_H
#include <stdint.h>
#include <time.h>

typedef struct {
  uint64_t _us;
} absolute_time_t;

static inline uint64_t to_us_since_boot(absolute_time_t t) { return t._us; }

static inline absolute_time_t get_absolute_time(void) {
  absolute_time_t t = {._us = 1000};
  return t;
}

/* Monotonic microsecond clock for host tests. */
static inline uint64_t time_us_64(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#endif
