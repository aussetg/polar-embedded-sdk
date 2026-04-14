#ifndef POLAR_SDK_MPY_BTSTACK_CONFIG_H
#define POLAR_SDK_MPY_BTSTACK_CONFIG_H

// Project-local BTstack configuration overlay for the MicroPython firmware
// build.
//
// Why this exists:
// - The rp2 MicroPython port normally injects its own
// btstack_inc/btstack_config.h.
// - That default config is intentionally minimal and differs from the dedicated
//   pico-sdk probe builds used in this repository.
// - Our H10 PSFTP paths depend on the same security/crypto behavior that the
//   pure C probes use successfully, so we override the MPY-side config with a
//   repo-local header that mirrors the probe configuration closely.
//
// How this is selected:
// - polar_sdk/mpy/micropython.cmake overrides
//     MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE
//   for the whole firmware build.
// - That points BTstack directly at this file without modifying vendors/.

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#include "btstack_config_common.h"

// Match the pico-sdk probe profile more closely than the rp2 default.
// Use explicit undef/redefine so the final values are obvious in one place.

#undef HCI_ACL_PAYLOAD_SIZE
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)

#undef MAX_NR_GATT_CLIENTS
#define MAX_NR_GATT_CLIENTS 1

#undef MAX_NR_HCI_CONNECTIONS
#define MAX_NR_HCI_CONNECTIONS 2

#undef MAX_NR_L2CAP_CHANNELS
#define MAX_NR_L2CAP_CHANNELS 4

#undef MAX_NR_L2CAP_SERVICES
#define MAX_NR_L2CAP_SERVICES 1

#undef MAX_NR_SM_LOOKUP_ENTRIES
#define MAX_NR_SM_LOOKUP_ENTRIES 3

#undef MAX_NR_WHITELIST_ENTRIES
#define MAX_NR_WHITELIST_ENTRIES 1

#undef MAX_NR_LE_DEVICE_DB_ENTRIES
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4

#undef MAX_ATT_DB_SIZE
#define MAX_ATT_DB_SIZE 512

#undef HCI_RESET_RESEND_TIMEOUT_MS
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

// CYW43 transport settings used by the working pico-sdk probe builds.
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Match probe-side software crypto enablement. The MicroPython BTstack build
// does not pull these helpers by default, so micropython.cmake adds the needed
// include directories and C sources alongside this config header.
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#define HAVE_ASSERT

#endif