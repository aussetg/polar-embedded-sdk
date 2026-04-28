/*
 * Host-side FatFS shim for journal_writer tests.
 * Provides just enough types to compile journal_writer.c without real FatFS.
 */
#ifndef FF_H /* match the real ff.h include guard */
#define FF_H

#include <stdint.h>
#include <string.h>

typedef unsigned int UINT;
typedef enum {
  FR_OK = 0,
  FR_DISK_ERR,
  FR_INT_ERR,
  FR_NOT_READY,
  FR_NO_FILE,
  FR_NO_PATH,
  FR_INVALID_NAME,
  FR_DENIED,
  FR_EXIST,
  FR_INVALID_OBJECT,
  FR_WRITE_PROTECTED,
  FR_INVALID_DRIVE,
  FR_NOT_ENABLED,
  FR_NO_FILESYSTEM,
  FR_MKFS_ABORTED,
  FR_TIMEOUT,
  FR_LOCKED,
  FR_NOT_ENOUGH_CORE,
  FR_TOO_MANY_OPEN_FILES
} FRESULT;

typedef unsigned int FSIZE_t;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_APPEND 0x30
#define FA_CREATE_ALWAYS 0x08

typedef struct {
  FSIZE_t fsize;
  int is_open;
  int sync_count;
  int write_count;
  int close_count;
  uint8_t *buf;
  size_t buf_cap;
  size_t buf_len;
  size_t pos;
} FIL;

/* Shim functions — implemented by the test */
FRESULT f_open(FIL *fp, const char *path, unsigned int mode);
FRESULT f_write(FIL *fp, const void *data, UINT len, UINT *written);
FRESULT f_read(FIL *fp, void *data, UINT len, UINT *read);
FRESULT f_sync(FIL *fp);
FRESULT f_close(FIL *fp);

#endif
