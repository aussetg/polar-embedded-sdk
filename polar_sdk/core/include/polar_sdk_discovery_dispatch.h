// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_DISCOVERY_DISPATCH_H
#define POLAR_SDK_DISCOVERY_DISPATCH_H

#include <stdint.h>

#include "polar_sdk_discovery_orchestrator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *ctx;
  uint8_t (*discover_hr_chars)(void *ctx);
  uint8_t (*discover_pmd_chars)(void *ctx);
  uint8_t (*discover_psftp_chars)(void *ctx);
} polar_sdk_discovery_dispatch_ops_t;

uint8_t polar_sdk_discovery_dispatch_command(
    polar_sdk_discovery_command_t cmd,
    const polar_sdk_discovery_dispatch_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_DISCOVERY_DISPATCH_H
