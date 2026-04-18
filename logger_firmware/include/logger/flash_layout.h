#ifndef LOGGER_FIRMWARE_FLASH_LAYOUT_H
#define LOGGER_FIRMWARE_FLASH_LAYOUT_H

#include "hardware/flash.h"

#define LOGGER_FLASH_PERSIST_REGION_SIZE (16u * FLASH_SECTOR_SIZE)

#if PICO_RP2350 && PICO_RP2350_A2_SUPPORTED
#define LOGGER_BTSTACK_STORAGE_OFFSET                                          \
  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - (FLASH_SECTOR_SIZE * 2u))
#else
#define LOGGER_BTSTACK_STORAGE_OFFSET                                          \
  (PICO_FLASH_SIZE_BYTES - (FLASH_SECTOR_SIZE * 2u))
#endif

#define LOGGER_FLASH_PERSIST_REGION_OFFSET                                     \
  (LOGGER_BTSTACK_STORAGE_OFFSET - LOGGER_FLASH_PERSIST_REGION_SIZE)

#define LOGGER_FLASH_CONFIG_SLOT_COUNT 8u
#define LOGGER_FLASH_CONFIG_SLOT_SIZE FLASH_SECTOR_SIZE
#define LOGGER_FLASH_CONFIG_REGION_OFFSET LOGGER_FLASH_PERSIST_REGION_OFFSET
#define LOGGER_FLASH_CONFIG_REGION_SIZE                                        \
  (LOGGER_FLASH_CONFIG_SLOT_COUNT * LOGGER_FLASH_CONFIG_SLOT_SIZE)

#define LOGGER_FLASH_METADATA_SLOT_COUNT 8u
#define LOGGER_FLASH_METADATA_SLOT_SIZE FLASH_SECTOR_SIZE
#define LOGGER_FLASH_METADATA_REGION_SIZE                                      \
  (LOGGER_FLASH_METADATA_SLOT_COUNT * LOGGER_FLASH_METADATA_SLOT_SIZE)

#define LOGGER_FLASH_METADATA_REGION_OFFSET                                    \
  (LOGGER_FLASH_CONFIG_REGION_OFFSET + LOGGER_FLASH_CONFIG_REGION_SIZE)

/*
 * Reserve a roomier raw-flash journal area so config and hot metadata can use
 * multi-slot wear levelling and the system log can afford larger records.
 */

_Static_assert(LOGGER_FLASH_CONFIG_REGION_SIZE +
                       LOGGER_FLASH_METADATA_REGION_SIZE ==
                   LOGGER_FLASH_PERSIST_REGION_SIZE,
               "flash persistence regions must exactly fill the reserved area");

#endif
