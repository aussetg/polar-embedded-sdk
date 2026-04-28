// SPDX-License-Identifier: MIT
#include "ff.h"

#include "logger/storage.h"

/*
 * Link-time FatFS boundary guard.
 *
 * logger_appliance links this file with GNU ld --wrap=f_* for the FatFS API
 * symbols exposed by ff.h.  That turns accidental direct FatFS calls in
 * journal/queue/session/upload code into the same fail-fast ownership check as
 * storage.c, without relying on each call site remembering to add a local
 * assertion.  The f_size/f_tell/f_eof/f_error helpers are ff.h macros, not
 * linkable API symbols; real media access is still guarded at f_* and disk_*
 * boundaries.
 *
 * The guard is intentionally not a lock.  FatFS remains single-owner:
 *   - pre-worker BOOT: core 0 may access SD/FatFS directly
 *   - post-worker runtime: only core 1 may access SD/FatFS
 */

#define LOGGER_FATFS_GUARD() logger_storage_assert_fatfs_context()

FRESULT __real_f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT __real_f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work,
                      UINT len);
FRESULT __real_f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs);
FRESULT __real_f_mkdir(const TCHAR *path);
FRESULT __real_f_stat(const TCHAR *path, FILINFO *fno);
FRESULT __real_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __real_f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT __real_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT __real_f_sync(FIL *fp);
FRESULT __real_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT __real_f_truncate(FIL *fp);
FRESULT __real_f_close(FIL *fp);
FRESULT __real_f_unlink(const TCHAR *path);
FRESULT __real_f_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT __real_f_opendir(DIR *dp, const TCHAR *path);
FRESULT __real_f_readdir(DIR *dp, FILINFO *fno);
FRESULT __real_f_closedir(DIR *dp);
FRESULT __real_f_findfirst(DIR *dp, FILINFO *fno, const TCHAR *path,
                           const TCHAR *pattern);
FRESULT __real_f_findnext(DIR *dp, FILINFO *fno);
FRESULT __real_f_chmod(const TCHAR *path, BYTE attr, BYTE mask);
FRESULT __real_f_utime(const TCHAR *path, const FILINFO *fno);
FRESULT __real_f_chdir(const TCHAR *path);
FRESULT __real_f_chdrive(const TCHAR *path);
FRESULT __real_f_getcwd(TCHAR *buff, UINT len);
FRESULT __real_f_getlabel(const TCHAR *path, TCHAR *label, DWORD *vsn);
FRESULT __real_f_setlabel(const TCHAR *label);
FRESULT __real_f_forward(FIL *fp, UINT (*func)(const BYTE *, UINT), UINT btf,
                         UINT *bf);
FRESULT __real_f_expand(FIL *fp, FSIZE_t fsz, BYTE opt);
FRESULT __real_f_fdisk(BYTE pdrv, const LBA_t ptbl[], void *work);
FRESULT __real_f_setcp(WORD cp);
int __real_f_putc(TCHAR c, FIL *fp);
int __real_f_puts(const TCHAR *str, FIL *cp);
TCHAR *__real_f_gets(TCHAR *buff, int len, FIL *fp);

FRESULT __wrap_f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT __wrap_f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work,
                      UINT len);
FRESULT __wrap_f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs);
FRESULT __wrap_f_mkdir(const TCHAR *path);
FRESULT __wrap_f_stat(const TCHAR *path, FILINFO *fno);
FRESULT __wrap_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __wrap_f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT __wrap_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT __wrap_f_sync(FIL *fp);
FRESULT __wrap_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT __wrap_f_truncate(FIL *fp);
FRESULT __wrap_f_close(FIL *fp);
FRESULT __wrap_f_unlink(const TCHAR *path);
FRESULT __wrap_f_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT __wrap_f_opendir(DIR *dp, const TCHAR *path);
FRESULT __wrap_f_readdir(DIR *dp, FILINFO *fno);
FRESULT __wrap_f_closedir(DIR *dp);
FRESULT __wrap_f_findfirst(DIR *dp, FILINFO *fno, const TCHAR *path,
                           const TCHAR *pattern);
FRESULT __wrap_f_findnext(DIR *dp, FILINFO *fno);
FRESULT __wrap_f_chmod(const TCHAR *path, BYTE attr, BYTE mask);
FRESULT __wrap_f_utime(const TCHAR *path, const FILINFO *fno);
FRESULT __wrap_f_chdir(const TCHAR *path);
FRESULT __wrap_f_chdrive(const TCHAR *path);
FRESULT __wrap_f_getcwd(TCHAR *buff, UINT len);
FRESULT __wrap_f_getlabel(const TCHAR *path, TCHAR *label, DWORD *vsn);
FRESULT __wrap_f_setlabel(const TCHAR *label);
FRESULT __wrap_f_forward(FIL *fp, UINT (*func)(const BYTE *, UINT), UINT btf,
                         UINT *bf);
FRESULT __wrap_f_expand(FIL *fp, FSIZE_t fsz, BYTE opt);
FRESULT __wrap_f_fdisk(BYTE pdrv, const LBA_t ptbl[], void *work);
FRESULT __wrap_f_setcp(WORD cp);
int __wrap_f_putc(TCHAR c, FIL *fp);
int __wrap_f_puts(const TCHAR *str, FIL *cp);
TCHAR *__wrap_f_gets(TCHAR *buff, int len, FIL *fp);

FRESULT __wrap_f_mount(FATFS *fs, const TCHAR *path, BYTE opt) {
  LOGGER_FATFS_GUARD();
  return __real_f_mount(fs, path, opt);
}

FRESULT __wrap_f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work,
                      UINT len) {
  LOGGER_FATFS_GUARD();
  return __real_f_mkfs(path, opt, work, len);
}

FRESULT __wrap_f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs) {
  LOGGER_FATFS_GUARD();
  return __real_f_getfree(path, nclst, fatfs);
}

FRESULT __wrap_f_mkdir(const TCHAR *path) {
  LOGGER_FATFS_GUARD();
  return __real_f_mkdir(path);
}

FRESULT __wrap_f_stat(const TCHAR *path, FILINFO *fno) {
  LOGGER_FATFS_GUARD();
  return __real_f_stat(path, fno);
}

FRESULT __wrap_f_open(FIL *fp, const TCHAR *path, BYTE mode) {
  LOGGER_FATFS_GUARD();
  return __real_f_open(fp, path, mode);
}

FRESULT __wrap_f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  LOGGER_FATFS_GUARD();
  return __real_f_read(fp, buff, btr, br);
}

FRESULT __wrap_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
  LOGGER_FATFS_GUARD();
  return __real_f_write(fp, buff, btw, bw);
}

FRESULT __wrap_f_sync(FIL *fp) {
  LOGGER_FATFS_GUARD();
  return __real_f_sync(fp);
}

FRESULT __wrap_f_lseek(FIL *fp, FSIZE_t ofs) {
  LOGGER_FATFS_GUARD();
  return __real_f_lseek(fp, ofs);
}

FRESULT __wrap_f_truncate(FIL *fp) {
  LOGGER_FATFS_GUARD();
  return __real_f_truncate(fp);
}

FRESULT __wrap_f_close(FIL *fp) {
  LOGGER_FATFS_GUARD();
  return __real_f_close(fp);
}

FRESULT __wrap_f_unlink(const TCHAR *path) {
  LOGGER_FATFS_GUARD();
  return __real_f_unlink(path);
}

FRESULT __wrap_f_rename(const TCHAR *path_old, const TCHAR *path_new) {
  LOGGER_FATFS_GUARD();
  return __real_f_rename(path_old, path_new);
}

FRESULT __wrap_f_opendir(DIR *dp, const TCHAR *path) {
  LOGGER_FATFS_GUARD();
  return __real_f_opendir(dp, path);
}

FRESULT __wrap_f_readdir(DIR *dp, FILINFO *fno) {
  LOGGER_FATFS_GUARD();
  return __real_f_readdir(dp, fno);
}

FRESULT __wrap_f_closedir(DIR *dp) {
  LOGGER_FATFS_GUARD();
  return __real_f_closedir(dp);
}

FRESULT __wrap_f_findfirst(DIR *dp, FILINFO *fno, const TCHAR *path,
                           const TCHAR *pattern) {
  LOGGER_FATFS_GUARD();
  return __real_f_findfirst(dp, fno, path, pattern);
}

FRESULT __wrap_f_findnext(DIR *dp, FILINFO *fno) {
  LOGGER_FATFS_GUARD();
  return __real_f_findnext(dp, fno);
}

FRESULT __wrap_f_chmod(const TCHAR *path, BYTE attr, BYTE mask) {
  LOGGER_FATFS_GUARD();
  return __real_f_chmod(path, attr, mask);
}

FRESULT __wrap_f_utime(const TCHAR *path, const FILINFO *fno) {
  LOGGER_FATFS_GUARD();
  return __real_f_utime(path, fno);
}

FRESULT __wrap_f_chdir(const TCHAR *path) {
  LOGGER_FATFS_GUARD();
  return __real_f_chdir(path);
}

FRESULT __wrap_f_chdrive(const TCHAR *path) {
  LOGGER_FATFS_GUARD();
  return __real_f_chdrive(path);
}

FRESULT __wrap_f_getcwd(TCHAR *buff, UINT len) {
  LOGGER_FATFS_GUARD();
  return __real_f_getcwd(buff, len);
}

FRESULT __wrap_f_getlabel(const TCHAR *path, TCHAR *label, DWORD *vsn) {
  LOGGER_FATFS_GUARD();
  return __real_f_getlabel(path, label, vsn);
}

FRESULT __wrap_f_setlabel(const TCHAR *label) {
  LOGGER_FATFS_GUARD();
  return __real_f_setlabel(label);
}

FRESULT __wrap_f_forward(FIL *fp, UINT (*func)(const BYTE *, UINT), UINT btf,
                         UINT *bf) {
  LOGGER_FATFS_GUARD();
  return __real_f_forward(fp, func, btf, bf);
}

FRESULT __wrap_f_expand(FIL *fp, FSIZE_t fsz, BYTE opt) {
  LOGGER_FATFS_GUARD();
  return __real_f_expand(fp, fsz, opt);
}

FRESULT __wrap_f_fdisk(BYTE pdrv, const LBA_t ptbl[], void *work) {
  LOGGER_FATFS_GUARD();
  return __real_f_fdisk(pdrv, ptbl, work);
}

FRESULT __wrap_f_setcp(WORD cp) {
  LOGGER_FATFS_GUARD();
  return __real_f_setcp(cp);
}

int __wrap_f_putc(TCHAR c, FIL *fp) {
  LOGGER_FATFS_GUARD();
  return __real_f_putc(c, fp);
}

int __wrap_f_puts(const TCHAR *str, FIL *cp) {
  LOGGER_FATFS_GUARD();
  return __real_f_puts(str, cp);
}

TCHAR *__wrap_f_gets(TCHAR *buff, int len, FIL *fp) {
  LOGGER_FATFS_GUARD();
  return __real_f_gets(buff, len, fp);
}
