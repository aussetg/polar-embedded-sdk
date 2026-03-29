#ifndef LOGGER_FIRMWARE_IDENTITY_H
#define LOGGER_FIRMWARE_IDENTITY_H

#define LOGGER_HARDWARE_ID_HEX_LEN 32

void logger_identity_read_hardware_id_hex(char out_hex[LOGGER_HARDWARE_ID_HEX_LEN + 1]);

#endif
