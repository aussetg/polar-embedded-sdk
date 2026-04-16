#include "logger/storage.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
// ff.h needs to be included before diskio.h
#include "diskio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "logger/capture_stats.h"
#include "logger/util.h"
#include "pico/stdlib.h"

#include "board_config.h"

#define LOGGER_SD_DRIVE_PATH "0:"
#define LOGGER_SD_ROOT_DIR "0:/logger"
#define LOGGER_SD_STATE_DIR "0:/logger/state"
#define LOGGER_SD_SESSIONS_DIR "0:/logger/sessions"
#define LOGGER_SD_EXPORTS_DIR "0:/logger/exports"

#define LOGGER_SD_SPI_INIT_BAUD_HZ 100000u
#define LOGGER_SD_SPI_RUN_BAUD_HZ 12000000u
#define LOGGER_SD_DATA_TIMEOUT_MS 500u
#define LOGGER_SD_MKFS_WORK_BYTES 4096u

#define LOGGER_SD_TOKEN_START_BLOCK 0xfeu

typedef struct {
  bool io_initialized;
  bool card_initialized;
  bool mounted;
  bool logger_root_ready;
  bool writable;
  bool high_capacity;
  DSTATUS dstatus;
  FATFS fatfs;
  uint8_t cid[16];
  uint8_t csd[16];
  uint8_t ocr[4];
  uint32_t sector_count;
} logger_sd_driver_t;

static logger_sd_driver_t g_sd;
static logger_capture_stats_t *g_storage_stats = NULL;

/* DMA channels for SPI bulk data phase.
 *   TX channel: memory → SPI TX FIFO, paced by DREQ_SPIx_TX
 *   RX channel: SPI RX FIFO → memory, paced by DREQ_SPIx_RX
 * Claimed once at init, reused for every block transfer. */
static int g_sd_dma_ch_tx = -1;
static int g_sd_dma_ch_rx = -1;
static uint8_t g_sd_dma_ff_byte = 0xFFu;
static uint8_t g_sd_dma_sink_byte;

static spi_inst_t *logger_sd_spi_bus(void) {
#if LOGGER_SD_SPI_BUS == 0
  return spi0;
#else
  return spi1;
#endif
}

static void logger_format_hex(char *dst, size_t dst_len, const uint8_t *data,
                              size_t data_len) {
  static const char hex[] = "0123456789ABCDEF";
  if (dst_len == 0u) {
    return;
  }
  size_t out = 0u;
  for (size_t i = 0u; i < data_len && (out + 2u) < dst_len; ++i) {
    dst[out++] = hex[(data[i] >> 4) & 0x0f];
    dst[out++] = hex[data[i] & 0x0f];
  }
  dst[out] = '\0';
}

static bool logger_storage_path_tmp(char out[LOGGER_STORAGE_PATH_MAX],
                                    const char *path) {
  const char *suffix = ".tmp";
  const size_t path_len = strlen(path);
  const size_t suffix_len = strlen(suffix);
  if ((path_len + suffix_len + 1u) > LOGGER_STORAGE_PATH_MAX) {
    return false;
  }
  memcpy(out, path, path_len);
  memcpy(out + path_len, suffix, suffix_len + 1u);
  return true;
}

static bool logger_sd_detect_pin_active(void) {
  return gpio_get(LOGGER_SD_DETECT_PIN) == 0;
}

static bool logger_sd_detect_known_present(void) {
  return logger_sd_detect_pin_active();
}

static uint8_t logger_sd_spi_xfer(uint8_t tx) {
  uint8_t rx = 0xffu;
  (void)spi_write_read_blocking(logger_sd_spi_bus(), &tx, &rx, 1u);
  return rx;
}

static void logger_sd_spi_write_ff(size_t count) {
  while (count-- > 0u) {
    (void)logger_sd_spi_xfer(0xffu);
  }
}

static void logger_sd_deselect(void) {
  gpio_put(LOGGER_SD_CS_PIN, 1);
  (void)logger_sd_spi_xfer(0xffu);
}

static void logger_sd_select(void) { gpio_put(LOGGER_SD_CS_PIN, 0); }

static bool logger_sd_wait_ready(uint32_t timeout_ms) {
  const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  while ((to_ms_since_boot(get_absolute_time()) - start_ms) < timeout_ms) {
    if (logger_sd_spi_xfer(0xffu) == 0xffu) {
      return true;
    }
  }
  return false;
}

static uint8_t logger_sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc,
                                      uint8_t *extra, size_t extra_len,
                                      bool release) {
  logger_sd_select();

  uint8_t frame[6];
  frame[0] = (uint8_t)(0x40u | cmd);
  frame[1] = (uint8_t)(arg >> 24);
  frame[2] = (uint8_t)(arg >> 16);
  frame[3] = (uint8_t)(arg >> 8);
  frame[4] = (uint8_t)arg;
  frame[5] = crc;

  for (size_t i = 0u; i < sizeof(frame); ++i) {
    (void)logger_sd_spi_xfer(frame[i]);
  }

  uint8_t r1 = 0xffu;
  for (int i = 0; i < 100; ++i) {
    r1 = logger_sd_spi_xfer(0xffu);
    if ((r1 & 0x80u) == 0u) {
      break;
    }
  }

  if (((r1 & 0x80u) == 0u) && extra != NULL) {
    for (size_t i = 0u; i < extra_len; ++i) {
      extra[i] = logger_sd_spi_xfer(0xffu);
    }
  }

  if (release) {
    logger_sd_deselect();
  }

  return r1;
}

/* Claim DMA channels for SPI bulk transfers. Idempotent. */
static void logger_sd_dma_init(void) {
  if (g_sd_dma_ch_tx >= 0) {
    return;
  }
  g_sd_dma_ch_tx = dma_claim_unused_channel(true);
  g_sd_dma_ch_rx = dma_claim_unused_channel(true);
}

/* Full-duplex SPI bulk transfer using DMA.
 *   tx_src: data to send, or NULL to transmit constant 0xFF
 *   rx_dst: buffer for received data, or NULL to discard
 *   len:    number of bytes to exchange
 *
 * Both channels are paced by their respective SPI DREQs so the
 * transfer proceeds at the SPI clock rate without CPU intervention. */
static void __not_in_flash_func(logger_sd_dma_xfer)(const uint8_t *tx_src,
                                                    uint8_t *rx_dst,
                                                    size_t len) {
  spi_inst_t *spi = logger_sd_spi_bus();
  const uint dreq_tx = spi_get_dreq(spi, true);
  const uint dreq_rx = spi_get_dreq(spi, false);

  /* Drain stale RX FIFO entries so the RX DMA channel starts clean. */
  while (spi_is_readable(spi)) {
    (void)spi_get_hw(spi)->dr;
  }

  /* TX DMA: memory → SPI TX FIFO */
  dma_channel_config_t tx_cfg = dma_channel_get_default_config(g_sd_dma_ch_tx);
  channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
  channel_config_set_dreq(&tx_cfg, dreq_tx);
  channel_config_set_read_increment(&tx_cfg, tx_src != NULL);
  channel_config_set_write_increment(&tx_cfg, false);

  dma_channel_configure(g_sd_dma_ch_tx, &tx_cfg,
                        /* write_addr  */ &spi_get_hw(spi)->dr,
                        /* read_addr   */ tx_src != NULL ? tx_src
                                                         : &g_sd_dma_ff_byte,
                        /* trans_count */ dma_encode_transfer_count(len),
                        /* trigger     */ false);

  /* RX DMA: SPI RX FIFO → memory */
  dma_channel_config_t rx_cfg = dma_channel_get_default_config(g_sd_dma_ch_rx);
  channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
  channel_config_set_dreq(&rx_cfg, dreq_rx);
  channel_config_set_read_increment(&rx_cfg, false);
  channel_config_set_write_increment(&rx_cfg, rx_dst != NULL);

  dma_channel_configure(g_sd_dma_ch_rx, &rx_cfg,
                        /* write_addr  */ rx_dst != NULL ? rx_dst
                                                         : &g_sd_dma_sink_byte,
                        /* read_addr   */ &spi_get_hw(spi)->dr,
                        /* trans_count */ dma_encode_transfer_count(len),
                        /* trigger     */ false);

  /* Start RX before TX: RX must be ready to drain before TX clocks data in. */
  dma_channel_start(g_sd_dma_ch_rx);
  dma_channel_start(g_sd_dma_ch_tx);

  dma_channel_wait_for_finish_blocking(g_sd_dma_ch_tx);
  dma_channel_wait_for_finish_blocking(g_sd_dma_ch_rx);

  /* SPI may still be shifting the last byte after DMA FIFO transfers complete.
   */
  while (spi_is_busy(spi)) {
    tight_loop_contents();
  }

  /* Safety drain — if this fires, the RX DMA missed bytes. */
  unsigned drain_count = 0u;
  while (spi_is_readable(spi)) {
    (void)spi_get_hw(spi)->dr;
    ++drain_count;
  }
  assert(drain_count == 0u);
}

static bool logger_sd_receive_data(uint8_t *buf, size_t len) {
  const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  uint8_t token = 0xffu;
  while ((to_ms_since_boot(get_absolute_time()) - start_ms) <
         LOGGER_SD_DATA_TIMEOUT_MS) {
    token = logger_sd_spi_xfer(0xffu);
    if (token != 0xffu) {
      break;
    }
  }
  if (token != LOGGER_SD_TOKEN_START_BLOCK) {
    return false;
  }

  /* Bulk data phase: DMA clocks out 0xFF while reading len bytes. */
  logger_sd_dma_xfer(NULL, buf, len);

  (void)logger_sd_spi_xfer(0xffu); /* CRC byte 1 */
  (void)logger_sd_spi_xfer(0xffu); /* CRC byte 2 */
  return true;
}

static bool logger_sd_send_data_block(const uint8_t *buf, size_t len) {
  if (!logger_sd_wait_ready(LOGGER_SD_DATA_TIMEOUT_MS)) {
    return false;
  }

  (void)logger_sd_spi_xfer(LOGGER_SD_TOKEN_START_BLOCK);

  /* Bulk data phase: DMA clocks out buf while draining RX. */
  logger_sd_dma_xfer(buf, NULL, len);

  (void)logger_sd_spi_xfer(0xffu); /* CRC byte 1 */
  (void)logger_sd_spi_xfer(0xffu); /* CRC byte 2 */

  const uint8_t resp = logger_sd_spi_xfer(0xffu);
  if ((resp & 0x1fu) != 0x05u) {
    return false;
  }
  return logger_sd_wait_ready(LOGGER_SD_DATA_TIMEOUT_MS);
}

static uint32_t logger_sd_parse_sector_count(const uint8_t csd[16]) {
  const uint8_t csd_version = (uint8_t)((csd[0] >> 6) & 0x03u);
  if (csd_version == 1u) {
    const uint32_t c_size = ((uint32_t)(csd[7] & 0x3fu) << 16) |
                            ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
    return (c_size + 1u) * 1024u;
  }

  const uint32_t read_bl_len = (uint32_t)(csd[5] & 0x0fu);
  const uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
                          ((uint32_t)csd[7] << 2) |
                          ((uint32_t)(csd[8] >> 6) & 0x03u);
  const uint32_t c_size_mult =
      ((uint32_t)(csd[9] & 0x03u) << 1) | ((uint32_t)(csd[10] >> 7) & 0x01u);
  const uint32_t block_len = 1u << read_bl_len;
  const uint32_t mult = 1u << (c_size_mult + 2u);
  const uint64_t block_count = (uint64_t)(c_size + 1u) * mult;
  const uint64_t capacity_bytes = block_count * block_len;
  return (uint32_t)(capacity_bytes / 512u);
}

static void logger_sd_fill_identity(logger_storage_status_t *status) {
  logger_format_hex(status->manufacturer_id, sizeof(status->manufacturer_id),
                    &g_sd.cid[0], 1u);
  logger_format_hex(status->oem_id, sizeof(status->oem_id), &g_sd.cid[1], 2u);

  char pnm[6];
  for (size_t i = 0u; i < 5u; ++i) {
    const uint8_t ch = g_sd.cid[3u + i];
    pnm[i] = (ch >= 32u && ch <= 126u) ? (char)ch : '_';
  }
  pnm[5] = '\0';
  logger_copy_string(status->product_name, sizeof(status->product_name), pnm);

  snprintf(status->revision, sizeof(status->revision), "%u.%u",
           (unsigned)(g_sd.cid[8] >> 4), (unsigned)(g_sd.cid[8] & 0x0fu));
  snprintf(status->serial_number, sizeof(status->serial_number),
           "%02X%02X%02X%02X", g_sd.cid[9], g_sd.cid[10], g_sd.cid[11],
           g_sd.cid[12]);
}

static bool logger_sd_initialize_card(void) {
  logger_sd_driver_t *sd = &g_sd;
  if (sd->card_initialized) {
    return true;
  }
  if ((sd->dstatus & STA_NODISK) != 0u) {
    return false;
  }

  spi_set_baudrate(logger_sd_spi_bus(), LOGGER_SD_SPI_INIT_BAUD_HZ);
  logger_sd_deselect();
  logger_sd_spi_write_ff(16u);

  uint8_t r1 = 0xffu;
  for (int i = 0; i < 10; ++i) {
    r1 = logger_sd_send_command(0u, 0u, 0x95u, NULL, 0u, true);
    if (r1 == 0x01u) {
      break;
    }
    sleep_ms(10);
  }
  if (r1 != 0x01u) {
    return false;
  }

  uint8_t cmd8[4] = {0};
  r1 = logger_sd_send_command(8u, 0x1aau, 0x87u, cmd8, sizeof(cmd8), true);
  bool high_capacity = false;
  if (r1 == 0x01u) {
    if (cmd8[2] != 0x01u || cmd8[3] != 0xaau) {
      return false;
    }
    const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    do {
      (void)logger_sd_send_command(55u, 0u, 0x00u, NULL, 0u, true);
      r1 = logger_sd_send_command(41u, 0x40000000u, 0x00u, NULL, 0u, true);
      if (r1 == 0x00u) {
        break;
      }
      sleep_ms(50);
    } while ((to_ms_since_boot(get_absolute_time()) - start_ms) < 5000u);
    if (r1 != 0x00u) {
      return false;
    }

    r1 = logger_sd_send_command(58u, 0u, 0x00u, sd->ocr, sizeof(sd->ocr), true);
    if (r1 != 0x00u) {
      return false;
    }
    high_capacity = (sd->ocr[0] & 0x40u) != 0u;
  } else if ((r1 & 0x04u) != 0u) {
    const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    do {
      (void)logger_sd_send_command(55u, 0u, 0x00u, NULL, 0u, true);
      r1 = logger_sd_send_command(41u, 0u, 0x00u, NULL, 0u, true);
      if (r1 == 0x00u) {
        break;
      }
      sleep_ms(50);
    } while ((to_ms_since_boot(get_absolute_time()) - start_ms) < 5000u);
    if (r1 != 0x00u) {
      return false;
    }
  } else {
    return false;
  }

  r1 = logger_sd_send_command(16u, 512u, 0x00u, NULL, 0u, true);
  if (r1 != 0x00u) {
    return false;
  }

  r1 = logger_sd_send_command(9u, 0u, 0x00u, NULL, 0u, false);
  if (r1 != 0x00u || !logger_sd_receive_data(sd->csd, sizeof(sd->csd))) {
    logger_sd_deselect();
    return false;
  }
  logger_sd_deselect();

  r1 = logger_sd_send_command(10u, 0u, 0x00u, NULL, 0u, false);
  if (r1 != 0x00u || !logger_sd_receive_data(sd->cid, sizeof(sd->cid))) {
    logger_sd_deselect();
    return false;
  }
  logger_sd_deselect();

  sd->sector_count = logger_sd_parse_sector_count(sd->csd);
  sd->high_capacity = high_capacity;
  sd->card_initialized = sd->sector_count != 0u;
  sd->dstatus = (DSTATUS)(sd->card_initialized ? 0u : STA_NOINIT);
  if (sd->card_initialized) {
    spi_set_baudrate(logger_sd_spi_bus(), LOGGER_SD_SPI_RUN_BAUD_HZ);
  }
  return sd->card_initialized;
}

void logger_storage_init(void) {
  if (g_sd.io_initialized) {
    return;
  }

  memset(&g_sd, 0, sizeof(g_sd));
  g_sd.dstatus = (DSTATUS)STA_NOINIT;

  spi_init(logger_sd_spi_bus(), LOGGER_SD_SPI_INIT_BAUD_HZ);
  spi_set_format(logger_sd_spi_bus(), 8u, SPI_CPOL_0, SPI_CPHA_0,
                 SPI_MSB_FIRST);
  gpio_set_function(LOGGER_SD_MISO_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LOGGER_SD_SCK_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LOGGER_SD_MOSI_PIN, GPIO_FUNC_SPI);
  gpio_pull_up(LOGGER_SD_MISO_PIN);

  gpio_init(LOGGER_SD_CS_PIN);
  gpio_set_dir(LOGGER_SD_CS_PIN, GPIO_OUT);
  gpio_put(LOGGER_SD_CS_PIN, 1);

  gpio_init(LOGGER_SD_DETECT_PIN);
  gpio_set_dir(LOGGER_SD_DETECT_PIN, GPIO_IN);
  gpio_pull_up(LOGGER_SD_DETECT_PIN);

  logger_sd_dma_init();
  g_sd.io_initialized = true;
}

void logger_storage_set_capture_stats(logger_capture_stats_t *stats) {
  g_storage_stats = stats;
}

static bool logger_storage_mount_if_needed(void) {
  if (g_sd.mounted) {
    return true;
  }
  if (!logger_sd_initialize_card()) {
    return false;
  }
  const FRESULT mount_fr = f_mount(&g_sd.fatfs, LOGGER_SD_DRIVE_PATH, 1u);
  if (mount_fr != FR_OK) {
    return false;
  }
  g_sd.mounted = true;
  return true;
}

static void logger_storage_reset_mount_state(void) {
  g_sd.mounted = false;
  g_sd.logger_root_ready = false;
  g_sd.writable = false;
  memset(&g_sd.fatfs, 0, sizeof(g_sd.fatfs));
}

static void logger_storage_unmount(void) {
  if (g_sd.mounted) {
    (void)f_mount(NULL, LOGGER_SD_DRIVE_PATH, 1u);
  }
  logger_storage_reset_mount_state();
}

static bool logger_storage_prepare_root(void) {
  if (g_sd.logger_root_ready) {
    return true;
  }

  const char *dirs[] = {
      LOGGER_SD_ROOT_DIR,
      LOGGER_SD_STATE_DIR,
      LOGGER_SD_SESSIONS_DIR,
      LOGGER_SD_EXPORTS_DIR,
  };
  for (size_t i = 0u; i < (sizeof(dirs) / sizeof(dirs[0])); ++i) {
    const FRESULT fr = f_mkdir(dirs[i]);
    if (fr != FR_OK && fr != FR_EXIST) {
      return false;
    }
  }

  static const char probe_bytes[] = "ok\n";
  FIL file;
  UINT written = 0u;
  const FRESULT open_fr =
      f_open(&file, LOGGER_SD_STATE_DIR "/.probe", FA_WRITE | FA_CREATE_ALWAYS);
  if (open_fr != FR_OK) {
    return false;
  }
  const FRESULT write_fr =
      f_write(&file, probe_bytes, sizeof(probe_bytes) - 1u, &written);
  const FRESULT sync_fr = f_sync(&file);
  const FRESULT close_fr = f_close(&file);
  (void)f_unlink(LOGGER_SD_STATE_DIR "/.probe");
  if (write_fr != FR_OK || sync_fr != FR_OK || close_fr != FR_OK ||
      written != (UINT)(sizeof(probe_bytes) - 1u)) {
    return false;
  }

  g_sd.logger_root_ready = true;
  g_sd.writable = true;
  return true;
}

static void logger_storage_zero_status(logger_storage_status_t *status) {
  memset(status, 0, sizeof(*status));
  status->sector_size_bytes = 512u;
}

bool logger_storage_refresh(logger_storage_status_t *status) {
  logger_storage_zero_status(status);
  logger_storage_init();

  status->initialized = true;
  status->detect_pin_configured = true;
  status->detect_pin_asserted = logger_sd_detect_pin_active();

#if !LOGGER_SD_DETECT_OPTIONAL
  if (!status->detect_pin_asserted) {
    g_sd.dstatus = (DSTATUS)(STA_NOINIT | STA_NODISK);
    return false;
  }
#endif

  if (!logger_storage_mount_if_needed()) {
    status->card_present =
        logger_sd_detect_known_present() || g_sd.card_initialized;
    return false;
  }

  status->card_present = true;
  status->card_initialized = g_sd.card_initialized;
  status->mounted = g_sd.mounted;
  status->capacity_bytes = (uint64_t)g_sd.sector_count * 512u;
  status->sector_count = g_sd.sector_count;
  logger_sd_fill_identity(status);

  switch (g_sd.fatfs.fs_type) {
  case FS_FAT12:
    logger_copy_string(status->filesystem, sizeof(status->filesystem), "fat12");
    break;
  case FS_FAT16:
    logger_copy_string(status->filesystem, sizeof(status->filesystem), "fat16");
    break;
  case FS_FAT32:
    logger_copy_string(status->filesystem, sizeof(status->filesystem), "fat32");
    break;
  default:
    status->filesystem[0] = '\0';
    break;
  }

  if (g_sd.fatfs.fs_type == FS_FAT32 && logger_storage_prepare_root()) {
    status->logger_root_ready = true;
    status->writable = g_sd.writable;
  }

  DWORD free_clusters = 0u;
  FATFS *fs = NULL;
  if (status->mounted &&
      f_getfree(LOGGER_SD_DRIVE_PATH, &free_clusters, &fs) == FR_OK &&
      fs != NULL) {
    status->free_bytes = (uint64_t)free_clusters * fs->csize * 512u;
  }

  status->reserve_ok = status->free_bytes >= LOGGER_SD_MIN_FREE_RESERVE_BYTES;
  return logger_storage_ready_for_logging(status);
}

bool logger_storage_format(logger_storage_status_t *status) {
  logger_storage_init();

#if !LOGGER_SD_DETECT_OPTIONAL
  if (!logger_sd_detect_pin_active()) {
    if (status != NULL) {
      logger_storage_zero_status(status);
      status->initialized = true;
      status->detect_pin_configured = true;
      status->detect_pin_asserted = false;
    }
    g_sd.dstatus = (DSTATUS)(STA_NOINIT | STA_NODISK);
    logger_storage_reset_mount_state();
    return false;
  }
#endif

  logger_storage_unmount();

  static uint8_t mkfs_work[LOGGER_SD_MKFS_WORK_BYTES];
  const MKFS_PARM mkfs = {
      .fmt = FM_FAT32,
      .n_fat = 2u,
      .align = 0u,
      .n_root = 0u,
      .au_size = 0u,
  };
  if (f_mkfs(LOGGER_SD_DRIVE_PATH, &mkfs, mkfs_work, sizeof(mkfs_work)) !=
      FR_OK) {
    if (status != NULL) {
      (void)logger_storage_refresh(status);
    }
    return false;
  }
  if (!logger_storage_mount_if_needed()) {
    if (status != NULL) {
      (void)logger_storage_refresh(status);
    }
    return false;
  }
  if (g_sd.fatfs.fs_type != FS_FAT32 || !logger_storage_prepare_root()) {
    if (status != NULL) {
      (void)logger_storage_refresh(status);
    }
    return false;
  }

  logger_storage_status_t refreshed;
  (void)logger_storage_refresh(&refreshed);
  if (status != NULL) {
    *status = refreshed;
  }
  return refreshed.mounted && refreshed.writable &&
         refreshed.logger_root_ready &&
         strcmp(refreshed.filesystem, "fat32") == 0;
}

bool logger_storage_ready_for_logging(const logger_storage_status_t *status) {
  return status->card_initialized && status->mounted && status->writable &&
         status->logger_root_ready &&
         strcmp(status->filesystem, "fat32") == 0 && status->reserve_ok;
}

bool logger_storage_ensure_dir(const char *path) {
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted) {
    return false;
  }
  const FRESULT fr = f_mkdir(path);
  return fr == FR_OK || fr == FR_EXIST;
}

bool logger_storage_write_file_atomic(const char *path, const void *data,
                                      size_t len) {
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted || !status.writable) {
    return false;
  }

  char tmp_path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_storage_path_tmp(tmp_path, path)) {
    return false;
  }

  FIL file;
  UINT written = 0u;
  if (f_open(&file, tmp_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    return false;
  }

  const FRESULT write_fr =
      (len == 0u) ? FR_OK : f_write(&file, data, (UINT)len, &written);
  const FRESULT sync_fr = f_sync(&file);
  const FRESULT close_fr = f_close(&file);
  if ((len != 0u && (write_fr != FR_OK || written != (UINT)len)) ||
      sync_fr != FR_OK || close_fr != FR_OK) {
    (void)f_unlink(tmp_path);
    return false;
  }

  (void)f_unlink(path);
  if (f_rename(tmp_path, path) != FR_OK) {
    (void)f_unlink(tmp_path);
    return false;
  }
  return true;
}

bool logger_storage_append_file(const char *path, const void *data, size_t len,
                                uint64_t *new_size_bytes) {
  const uint32_t t0 = (uint32_t)to_us_since_boot(get_absolute_time());

  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted || !status.writable) {
    const uint32_t elapsed =
        (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
    logger_capture_stats_record_storage_append(g_storage_stats, elapsed, false);
    return false;
  }

  FIL file;
  if (f_open(&file, path, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
    const uint32_t elapsed =
        (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
    logger_capture_stats_record_storage_append(g_storage_stats, elapsed, false);
    return false;
  }

  UINT written = 0u;
  const FRESULT write_fr =
      (len == 0u) ? FR_OK : f_write(&file, data, (UINT)len, &written);

  const uint32_t sync_t0 = (uint32_t)to_us_since_boot(get_absolute_time());
  const FRESULT sync_fr = f_sync(&file);
  const uint32_t sync_elapsed =
      (uint32_t)to_us_since_boot(get_absolute_time()) - sync_t0;
  logger_capture_stats_record_sync(g_storage_stats, sync_elapsed);

  if (new_size_bytes != NULL) {
    *new_size_bytes = (uint64_t)f_size(&file);
  }
  const FRESULT close_fr = f_close(&file);
  const bool ok = (len == 0u || (write_fr == FR_OK && written == (UINT)len)) &&
                  sync_fr == FR_OK && close_fr == FR_OK;

  const uint32_t elapsed = (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
  logger_capture_stats_record_storage_append(g_storage_stats, elapsed, ok);
  return ok;
}

bool logger_storage_remove_file(const char *path) {
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted) {
    return false;
  }
  const FRESULT fr = f_unlink(path);
  return fr == FR_OK || fr == FR_NO_FILE;
}

bool logger_storage_file_size(const char *path, uint64_t *size_bytes) {
  FILINFO info;
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted) {
    return false;
  }
  if (f_stat(path, &info) != FR_OK) {
    return false;
  }
  if (size_bytes != NULL) {
    *size_bytes = (uint64_t)info.fsize;
  }
  return true;
}

bool logger_storage_file_exists(const char *path) {
  FILINFO info;
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted) {
    return false;
  }
  return f_stat(path, &info) == FR_OK;
}

bool logger_storage_read_file(const char *path, void *data, size_t cap,
                              size_t *len_out) {
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted || data == NULL) {
    return false;
  }

  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    return false;
  }

  const uint64_t file_size = (uint64_t)f_size(&file);
  if (file_size > cap) {
    (void)f_close(&file);
    return false;
  }

  UINT read_bytes = 0u;
  const FRESULT read_fr =
      (file_size == 0u) ? FR_OK
                        : f_read(&file, data, (UINT)file_size, &read_bytes);
  const FRESULT close_fr = f_close(&file);
  if (read_fr != FR_OK || close_fr != FR_OK || read_bytes != (UINT)file_size) {
    return false;
  }
  if (len_out != NULL) {
    *len_out = (size_t)file_size;
  }
  return true;
}

bool logger_storage_truncate_file(const char *path, uint64_t size_bytes) {
  logger_storage_status_t status;
  (void)logger_storage_refresh(&status);
  if (!status.mounted || !status.writable) {
    return false;
  }
  if (size_bytes > 0xffffffffu) {
    return false;
  }

  FIL file;
  if (f_open(&file, path, FA_WRITE) != FR_OK) {
    return false;
  }
  const FRESULT seek_fr = f_lseek(&file, (FSIZE_t)size_bytes);
  const FRESULT truncate_fr =
      (seek_fr == FR_OK) ? f_truncate(&file) : FR_INT_ERR;
  const FRESULT sync_fr = (truncate_fr == FR_OK) ? f_sync(&file) : FR_INT_ERR;
  const FRESULT close_fr = f_close(&file);
  return seek_fr == FR_OK && truncate_fr == FR_OK && sync_fr == FR_OK &&
         close_fr == FR_OK;
}

DSTATUS disk_initialize(BYTE pdrv) {
  logger_storage_init();
  if (pdrv != 0u) {
    return STA_NOINIT;
  }
#if !LOGGER_SD_DETECT_OPTIONAL
  if (!logger_sd_detect_pin_active()) {
    g_sd.dstatus = (DSTATUS)(STA_NOINIT | STA_NODISK);
    return g_sd.dstatus;
  }
#endif
  if (logger_sd_initialize_card()) {
    g_sd.dstatus = 0u;
  } else {
    g_sd.dstatus = (DSTATUS)STA_NOINIT;
  }
  return g_sd.dstatus;
}

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0u) {
    return STA_NOINIT;
  }
  logger_storage_init();
#if !LOGGER_SD_DETECT_OPTIONAL
  if (!logger_sd_detect_pin_active()) {
    g_sd.dstatus = (DSTATUS)(STA_NOINIT | STA_NODISK);
  }
#endif
  return g_sd.dstatus;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0u || buff == NULL || count == 0u) {
    return RES_PARERR;
  }
  if (disk_initialize(pdrv) & STA_NOINIT) {
    return RES_NOTRDY;
  }

  uint32_t block = (uint32_t)sector;
  if (!g_sd.high_capacity) {
    block *= 512u;
  }

  logger_sd_deselect();
  logger_sd_spi_write_ff(1u);

  for (UINT i = 0u; i < count; ++i) {
    const uint8_t r1 =
        logger_sd_send_command(17u, block, 0x00u, NULL, 0u, false);
    if (r1 != 0x00u || !logger_sd_receive_data(buff + (i * 512u), 512u)) {
      logger_sd_deselect();
      return RES_ERROR;
    }
    logger_sd_deselect();
    block += g_sd.high_capacity ? 1u : 512u;
  }

  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0u || buff == NULL || count == 0u) {
    return RES_PARERR;
  }
  if (disk_initialize(pdrv) & STA_NOINIT) {
    return RES_NOTRDY;
  }

  uint32_t block = (uint32_t)sector;
  if (!g_sd.high_capacity) {
    block *= 512u;
  }

  logger_sd_deselect();
  logger_sd_spi_write_ff(1u);

  for (UINT i = 0u; i < count; ++i) {
    const uint8_t r1 =
        logger_sd_send_command(24u, block, 0x00u, NULL, 0u, false);
    if (r1 != 0x00u || !logger_sd_send_data_block(buff + (i * 512u), 512u)) {
      logger_sd_deselect();
      return RES_ERROR;
    }
    logger_sd_deselect();
    block += g_sd.high_capacity ? 1u : 512u;
  }

  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (pdrv != 0u) {
    return RES_PARERR;
  }
  if (disk_initialize(pdrv) & STA_NOINIT) {
    return RES_NOTRDY;
  }

  switch (cmd) {
  case CTRL_SYNC:
    return logger_sd_wait_ready(LOGGER_SD_DATA_TIMEOUT_MS) ? RES_OK : RES_ERROR;
  case GET_SECTOR_COUNT:
    if (buff == NULL) {
      return RES_PARERR;
    }
    *(DWORD *)buff = g_sd.sector_count;
    return RES_OK;
  case GET_SECTOR_SIZE:
    if (buff == NULL) {
      return RES_PARERR;
    }
    *(WORD *)buff = 512u;
    return RES_OK;
  case GET_BLOCK_SIZE:
    if (buff == NULL) {
      return RES_PARERR;
    }
    *(DWORD *)buff = 128u;
    return RES_OK;
  case MMC_GET_CSD:
    if (buff == NULL) {
      return RES_PARERR;
    }
    memcpy(buff, g_sd.csd, sizeof(g_sd.csd));
    return RES_OK;
  case MMC_GET_CID:
    if (buff == NULL) {
      return RES_PARERR;
    }
    memcpy(buff, g_sd.cid, sizeof(g_sd.cid));
    return RES_OK;
  case MMC_GET_OCR:
    if (buff == NULL) {
      return RES_PARERR;
    }
    memcpy(buff, g_sd.ocr, sizeof(g_sd.ocr));
    return RES_OK;
  default:
    return RES_PARERR;
  }
}
