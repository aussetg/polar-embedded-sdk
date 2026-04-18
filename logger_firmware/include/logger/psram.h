#ifndef LOGGER_FIRMWARE_PSRAM_H
#define LOGGER_FIRMWARE_PSRAM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Base address of the APS6404L PSRAM when mapped via QMI window 1.
 * After psram_init() succeeds, this 8 MB region is accessible as
 * ordinary memory (byte-addressable, no erase needed).
 */
#define PSRAM_BASE ((uintptr_t)0x11000000u)
#define PSRAM_SIZE (8u * 1024u * 1024u)

/*
 * Initialise the on-board APS6404L PSRAM.
 *
 * Configures GPIO cs_pin as XIP_CS1, switches the PSRAM into QPI mode,
 * and configures QMI window 1 timing for the APS6404L-3SQR.
 *
 * Returns the detected PSRAM size in bytes (8 MB on success), or 0 on
 * failure (bad chip, bad solder, wrong pin).
 *
 * Must be called after cyw43_arch_init() (CYW43 claims SPI pins first)
 * and before any PSRAM access.
 */
size_t psram_init(unsigned int cs_pin);

#endif
