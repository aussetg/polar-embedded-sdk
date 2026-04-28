// SPDX-License-Identifier: MIT
#ifndef LOGGER_FIRMWARE_IPC_ATOMIC_H
#define LOGGER_FIRMWARE_IPC_ATOMIC_H

/*
 * Tiny typed wrappers for firmware-owned inter-core words.
 *
 * Why not volatile + hand-placed DMBs?
 *   volatile stops some compiler elision, but it does not define an atomic
 *   load/store operation and it does not attach acquire/release ordering to
 *   the publication word itself.  The GCC/Clang __atomic builtins do both, and
 *   arm-none-eabi emits the right barriers for Cortex-M33.
 *
 * Why not C11 _Atomic fields directly?
 *   These structs are embedded in larger zeroed firmware state blocks.  A tiny
 *   wrapper over normal scalar storage keeps init/memset simple, while making
 *   plain arithmetic/assignment on IPC fields a compile error unless someone
 *   explicitly reaches through .value.  All production code should use the
 *   helpers below.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t value;
} logger_ipc_u32_t;

typedef struct {
  bool value;
} logger_ipc_bool_t;

static inline void logger_ipc_u32_store_relaxed(logger_ipc_u32_t *cell,
                                                uint32_t value) {
  __atomic_store_n(&cell->value, value, __ATOMIC_RELAXED);
}

static inline void logger_ipc_u32_store_release(logger_ipc_u32_t *cell,
                                                uint32_t value) {
  __atomic_store_n(&cell->value, value, __ATOMIC_RELEASE);
}

static inline uint32_t
logger_ipc_u32_load_relaxed(const logger_ipc_u32_t *cell) {
  return __atomic_load_n(&cell->value, __ATOMIC_RELAXED);
}

static inline uint32_t
logger_ipc_u32_load_acquire(const logger_ipc_u32_t *cell) {
  return __atomic_load_n(&cell->value, __ATOMIC_ACQUIRE);
}

static inline uint32_t logger_ipc_u32_add_fetch_release(logger_ipc_u32_t *cell,
                                                        uint32_t delta) {
  return __atomic_add_fetch(&cell->value, delta, __ATOMIC_RELEASE);
}

static inline void logger_ipc_bool_store_relaxed(logger_ipc_bool_t *cell,
                                                 bool value) {
  __atomic_store_n(&cell->value, value, __ATOMIC_RELAXED);
}

static inline void logger_ipc_bool_store_release(logger_ipc_bool_t *cell,
                                                 bool value) {
  __atomic_store_n(&cell->value, value, __ATOMIC_RELEASE);
}

static inline bool logger_ipc_bool_load_relaxed(const logger_ipc_bool_t *cell) {
  return __atomic_load_n(&cell->value, __ATOMIC_RELAXED);
}

static inline bool logger_ipc_bool_load_acquire(const logger_ipc_bool_t *cell) {
  return __atomic_load_n(&cell->value, __ATOMIC_ACQUIRE);
}

#endif /* LOGGER_FIRMWARE_IPC_ATOMIC_H */