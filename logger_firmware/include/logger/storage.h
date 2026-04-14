#ifndef LOGGER_FIRMWARE_STORAGE_H
#define LOGGER_FIRMWARE_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGGER_STORAGE_PATH_MAX 320

typedef struct {
  bool initialized;
  bool detect_pin_configured;
  bool detect_pin_asserted;
  bool card_present;
  bool card_initialized;
  bool mounted;
  bool writable;
  bool logger_root_ready;
  bool reserve_ok;
  uint64_t capacity_bytes;
  uint64_t free_bytes;
  uint32_t sector_count;
  uint16_t sector_size_bytes;
  char filesystem[8];
  char manufacturer_id[3];
  char oem_id[5];
  char product_name[8];
  char revision[8];
  char serial_number[9];
} logger_storage_status_t;

void logger_storage_init(void);
bool logger_storage_refresh(logger_storage_status_t *status);
bool logger_storage_format(logger_storage_status_t *status);
bool logger_storage_ready_for_logging(const logger_storage_status_t *status);

bool logger_storage_ensure_dir(const char *path);
bool logger_storage_write_file_atomic(const char *path, const void *data,
                                      size_t len);
bool logger_storage_append_file(const char *path, const void *data, size_t len,
                                uint64_t *new_size_bytes);
bool logger_storage_remove_file(const char *path);
bool logger_storage_file_size(const char *path, uint64_t *size_bytes);
bool logger_storage_file_exists(const char *path);
bool logger_storage_read_file(const char *path, void *data, size_t cap,
                              size_t *len_out);
bool logger_storage_truncate_file(const char *path, uint64_t size_bytes);

#endif
