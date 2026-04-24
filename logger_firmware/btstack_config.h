#ifndef LOGGER_FIRMWARE_BTSTACK_CONFIG_H
#define LOGGER_FIRMWARE_BTSTACK_CONFIG_H

#define ENABLE_LE_CENTRAL

#define ENABLE_LOG_ERROR
/* ENABLE_LOG_INFO is disabled to save ~11 KiB of .rodata format strings.
 * ENABLE_PRINTF_HEXDUMP is kept because hci_dump_embedded_stdout.c (part of
 * pico_btstack_base) requires it at compile time, even though the code is
 * garbage-collected at link time. Re-enable both for debug builds. */
#define ENABLE_PRINTF_HEXDUMP

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1

#define MAX_NR_LE_DEVICE_DB_ENTRIES 4

#define MAX_NR_CONTROLLER_ACL_BUFFERS 3

#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 0
#define HCI_HOST_SCO_PACKET_NUM 0

#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define NVM_NUM_LINK_KEYS 2

/* MAX_ATT_DB_SIZE is required for att_db_util.c to compile (part of pico_btstack_ble),
 * even though we are a GATT client only and the code is GC'd at link time. */
#define MAX_ATT_DB_SIZE 32

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT

#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#define HAVE_AES128
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS
/* Use BTstack's micro-ecc backend for LE Secure Connections P-256.
 * Do not define HAVE_MBEDTLS_ECC_P256 here: BTstack's current mbedTLS
 * ECDH adapter calls mbedtls_ecp_mul() without an RNG and ignores its
 * return value, which produces bad DHKeys with our vendored mbedTLS. */

#endif
