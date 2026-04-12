// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_CORE_TESTS_FAKE_BTSTACK_H
#define POLAR_SDK_CORE_TESTS_FAKE_BTSTACK_H

#include <stdint.h>

uint8_t gap_encryption_key_size(uint16_t conn_handle);
void sm_request_pairing(uint16_t conn_handle);

#endif