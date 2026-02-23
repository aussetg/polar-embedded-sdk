#ifndef POLAR_BLE_PIMORONI_MP_COMPAT_H
#define POLAR_BLE_PIMORONI_MP_COMPAT_H

#ifdef __cplusplus
extern "C" {
#include "py/runtime.h"
}

// Pimoroni picographics currently calls mp_handle_pending(true/false).
// Newer MicroPython declares mp_handle_pending(mp_handle_pending_behaviour_t),
// which is not implicitly convertible from bool in C++.
static inline void mp_handle_pending(bool behavior_bool) {
    mp_handle_pending((mp_handle_pending_behaviour_t)behavior_bool);
}
#endif

#endif // POLAR_BLE_PIMORONI_MP_COMPAT_H
