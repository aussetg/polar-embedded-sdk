/*
 * PSRAM initialisation for the APS6404L-3SQR on the Pimoroni Pico LiPo 2 XL W.
 *
 * Adapted from MicroPython's rp2_psram.c (MIT licence, (c) 2025 Phil Howard,
 * Mike Bell, Kirk D. Benell).  Stripped to the bare minimum: detect, enter
 * QPI mode, configure QMI window 1 for memory-mapped access.
 *
 * After psram_init() returns a non-zero size, the full PSRAM is accessible
 * at PSRAM_BASE (0x11000000) as ordinary byte-addressable memory.
 */

#include "logger/psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"

#include <stdbool.h>

/* APS6404L-3SQR datasheet references are for Rev. 2.3, Apr 30 2020. */

/* Part density: APS6404L is 64 Mbit / 8M × 8 bits (datasheet p. 1;
 * ordering-code density table p. 7). */
#define APS6404L_SIZE_BYTES (8u * 1024u * 1024u)
_Static_assert(PSRAM_SIZE == APS6404L_SIZE_BYTES,
               "PSRAM_SIZE must match the APS6404L 64 Mbit density");

/* The firmware leaves the APS6404L in its default Linear Burst mode.  The
 * datasheet allows higher clocks only for 32-byte wrapped burst; Linear Burst
 * is limited to 84 MHz (ordering information note, p. 7; command table note,
 * p. 9; AC timing Table 10 note 1, p. 23). */
#define APS6404L_LINEAR_BURST_MAX_HZ 84000000u

/* Read ID 0x9F is SPI-only and limited to 33 MHz (command table p. 9;
 * operation description p. 15). */
#define APS6404L_SPI_READ_ID_MAX_HZ 33000000u

/* CE# timing constraints from AC timing Table 10 (p. 23): tCPH >= 18 ns;
 * tCEM max is 8 µs for the standard-grade APS6404L-3SQR used by this board. */
#define APS6404L_T_CPH_MIN_NS 18u
#define APS6404L_T_CEM_STANDARD_MAX_NS 8000u

/* Page size is 1 KiB / CA[9:0] (datasheet §8.2, p. 9). */
#define APS6404L_PAGE_BYTES 1024u

/* Command values from the APS6404L command table (p. 9): 0xF5 exits QPI,
 * 0x35 enters QPI, 0xEB is QPI Fast Quad Read, and 0x38 is QPI Write. */
#define APS6404L_CMD_EXIT_QPI 0xF5u
#define APS6404L_CMD_ENTER_QPI 0x35u
#define APS6404L_CMD_QPI_FAST_READ 0xEBu
#define APS6404L_CMD_QPI_WRITE 0x38u
#define APS6404L_CMD_SPI_READ_ID 0x9Fu

/* SPI Read ID returns KGD at byte 5 and EID at byte 6; KGD PASS is 0x5D
 * (Read ID operation and KGD table, datasheet p. 15). */
#define APS6404L_READ_ID_BYTES 7u
#define APS6404L_READ_ID_KGD_INDEX 5u
#define APS6404L_READ_ID_EID_INDEX 6u
#define APS6404L_KGD_PASS 0x5Du
#define APS6404L_EID_64MBIT 0x26u
#define PSRAM_EID_DENSITY_SHIFT 5u
#define PSRAM_EID_SIZE_ID_2MB 0u
#define PSRAM_EID_SIZE_ID_4MB 1u
#define PSRAM_EID_SIZE_ID_8MB 2u

#define PSRAM_MIB_BYTES (1024u * 1024u)

/* QPI Fast Quad Read 0xEB is command + 24-bit address + six wait cycles
 * before data (command table p. 9; QPI read operation p. 16).  RP2350 QMI can
 * overrun MAX_SELECT by one complete in-progress transfer.  The RP2350
 * datasheet says cache misses are issued as 64-bit QSPI transfers, and the
 * pico-sdk XIP cache API defines XIP_CACHE_LINE_SIZE as 8 bytes. */
#define APS6404L_QPI_CMD_BITS 8u
#define APS6404L_QPI_ADDR_BITS 24u
#define APS6404L_QPI_FAST_READ_WAIT_CYCLES 6u
#define RP2350_QMI_CACHE_MISS_TRANSFER_BYTES 8u
#define RP2350_QMI_MAX_SELECT_GUARD_SCK 1u

_Static_assert(RP2350_QMI_CACHE_MISS_TRANSFER_BYTES == XIP_CACHE_LINE_SIZE,
               "RP2350 QMI cache-miss transfer size must match XIP line size");

#define QMI_MAX_SELECT_FIELD_MAX 0x3fu
#define QMI_MIN_DESELECT_FIELD_MAX 0x1fu

static uint32_t psram_ceil_div_u32(uint32_t numerator, uint32_t denominator) {
  return (uint32_t)(((uint64_t)numerator + denominator - 1u) / denominator);
}

static uint32_t psram_ceil_div_u64(uint64_t numerator, uint64_t denominator) {
  return (uint32_t)((numerator + denominator - 1u) / denominator);
}

static uint32_t psram_encode_clkdiv(uint32_t divisor) {
  return divisor == 256u ? 0u : divisor;
}

static uint32_t psram_clkdiv_for_max_hz(uint32_t clock_hz, uint32_t max_hz) {
  uint32_t divisor = psram_ceil_div_u32(clock_hz, max_hz);
  if (divisor == 0u) {
    divisor = 1u;
  }
  if (divisor > 256u) {
    divisor = 256u;
  }
  return divisor;
}

static bool psram_compute_qmi_timing(uint32_t clock_hz, uint32_t *timing_out) {
  const uint32_t divisor =
      psram_clkdiv_for_max_hz(clock_hz, APS6404L_LINEAR_BURST_MAX_HZ);
  const uint32_t sck_hz = clock_hz / divisor;
  if (sck_hz == 0u || sck_hz > APS6404L_LINEAR_BURST_MAX_HZ) {
    return false;
  }

  /* RXDELAY follows the RP2350/MicroPython QMI tuning rule.  With the
   * APS6404L linear-burst 84 MHz cap above, the datasheet's >84 MHz sampling
   * tuning warning (Table 10 note 3, p. 23) is avoided. */
  uint32_t rxdelay = divisor;
  if (rxdelay > 7u) {
    rxdelay = 7u;
  }

  const uint32_t qpi_read_overhead_sck = (APS6404L_QPI_CMD_BITS / 4u) +
                                         (APS6404L_QPI_ADDR_BITS / 4u) +
                                         APS6404L_QPI_FAST_READ_WAIT_CYCLES;
  const uint32_t cache_miss_data_sck =
      (RP2350_QMI_CACHE_MISS_TRANSFER_BYTES * 8u) / 4u;
  const uint32_t worst_overrun_sck = qpi_read_overhead_sck +
                                     cache_miss_data_sck +
                                     RP2350_QMI_MAX_SELECT_GUARD_SCK;
  const uint32_t worst_overrun_ns =
      psram_ceil_div_u64((uint64_t)worst_overrun_sck * 1000000000ull, sck_hz);
  if (worst_overrun_ns >= APS6404L_T_CEM_STANDARD_MAX_NS) {
    return false;
  }

  const uint32_t max_select_budget_ns =
      APS6404L_T_CEM_STANDARD_MAX_NS - worst_overrun_ns;
  uint32_t max_select = (uint32_t)(((uint64_t)max_select_budget_ns * clock_hz) /
                                   (64ull * 1000000000ull));
  if (max_select == 0u) {
    return false; /* 0 disables MAX_SELECT; do not silently allow that. */
  }
  if (max_select > QMI_MAX_SELECT_FIELD_MAX) {
    max_select = QMI_MAX_SELECT_FIELD_MAX;
  }

  const uint32_t tcph_cycles = psram_ceil_div_u64(
      (uint64_t)APS6404L_T_CPH_MIN_NS * clock_hz, 1000000000ull);
  const uint32_t qmi_builtin_deselect_cycles = (divisor + 1u) / 2u;
  uint32_t min_deselect = 0u;
  if (tcph_cycles > qmi_builtin_deselect_cycles) {
    min_deselect = tcph_cycles - qmi_builtin_deselect_cycles;
  }
  if (min_deselect > QMI_MIN_DESELECT_FIELD_MAX) {
    return false;
  }

  /* PAGEBREAK=1024 matches the APS6404L 1 KiB page size (datasheet §8.2,
   * p. 9).  COOLDOWN=1 permits sequential bursts but MAX_SELECT bounds CE#
   * assertion so the APS6404L can refresh within tCEM (Table 10, p. 23). */
  *timing_out = 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                QMI_M1_TIMING_PAGEBREAK_VALUE_1024
                    << QMI_M1_TIMING_PAGEBREAK_LSB |
                max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                psram_encode_clkdiv(divisor) << QMI_M1_TIMING_CLKDIV_LSB;
  return true;
}

/*
 * Read the PSRAM JEDEC ID via QMI direct mode.
 * Returns the detected size in bytes, or 0 if no PSRAM found.
 */
static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
  const uint32_t clock_hz = clock_get_hz(clk_sys);
  const uint32_t direct_clkdiv =
      psram_clkdiv_for_max_hz(clock_hz, APS6404L_SPI_READ_ID_MAX_HZ);
  if ((clock_hz / direct_clkdiv) > APS6404L_SPI_READ_ID_MAX_HZ) {
    return 0;
  }

  /* Enable direct mode with SCK <= 33 MHz for Read ID 0x9F (datasheet p. 9,
   * p. 15).  This keeps the 7-byte ID transaction's CE#-low time comfortably
   * inside the standard-grade 8 µs tCEM limit (Table 10, p. 23). */
  qmi_hw->direct_csr = psram_encode_clkdiv(direct_clkdiv)
                           << QMI_DIRECT_CSR_CLKDIV_LSB |
                       QMI_DIRECT_CSR_EN_BITS;

  /* Wait for any in-flight XIP transfer to drain. */
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }

  /* Reset any prior QPI state: toggle CS1 with a quad NOP. */
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                      QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                      APS6404L_CMD_EXIT_QPI;
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

  /* Read ID: command 0x9F, then dummy/return bytes.  Byte 5 = KGD, byte 6 =
   * EID (datasheet p. 15). */
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  uint8_t kgd = 0;
  uint8_t eid = 0;

  for (size_t i = 0; i < APS6404L_READ_ID_BYTES; ++i) {
    qmi_hw->direct_tx = (i == 0) ? APS6404L_CMD_SPI_READ_ID : 0xffu;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
    }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }
    if (i == APS6404L_READ_ID_KGD_INDEX) {
      kgd = (uint8_t)qmi_hw->direct_rx;
    } else if (i == APS6404L_READ_ID_EID_INDEX) {
      eid = (uint8_t)qmi_hw->direct_rx;
    } else {
      (void)qmi_hw->direct_rx;
    }
  }

  /* Release CS1 and disable direct mode. */
  qmi_hw->direct_csr &=
      ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

  if (kgd != APS6404L_KGD_PASS) {
    return 0;
  }

  /* Decode size from the Read ID EID density field (datasheet Read ID figure,
   * p. 15).  APS6404L specifically is the 64 Mbit / 8 MiB density (p. 1,
   * p. 7); smaller values are returned only so the app can report a precise
   * wrong-chip size before rejecting it. */
  size_t psram_size = PSRAM_MIB_BYTES;
  uint8_t size_id = eid >> PSRAM_EID_DENSITY_SHIFT;
  if (eid == APS6404L_EID_64MBIT || size_id == PSRAM_EID_SIZE_ID_8MB) {
    psram_size *= 8u; /* 8 MiB */
  } else if (size_id == PSRAM_EID_SIZE_ID_2MB) {
    psram_size *= 2u;
  } else if (size_id == PSRAM_EID_SIZE_ID_4MB) {
    psram_size *= 4u;
  }

  return psram_size;
}

size_t __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
  gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

  uint32_t intr_stash = save_and_disable_interrupts();

  size_t detected = psram_detect();
  if (detected == 0u) {
    restore_interrupts(intr_stash);
    return 0;
  }

  /* Switch PSRAM into QPI mode via command 0x35 (datasheet command table,
   * p. 9).  Reuse the conservative direct-mode divider used for Read ID. */
  const uint32_t direct_clkdiv = psram_clkdiv_for_max_hz(
      (uint32_t)clock_get_hz(clk_sys), APS6404L_SPI_READ_ID_MAX_HZ);
  qmi_hw->direct_csr = psram_encode_clkdiv(direct_clkdiv)
                           << QMI_DIRECT_CSR_CLKDIV_LSB |
                       QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
  }

  qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | APS6404L_CMD_ENTER_QPI;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
  }

  /*
   * Disable direct mode.  This releases CS1; the PSRAM is now in QPI
   * mode.  The APS6404L requires tCPH >= 18 ns between CS# deassert
   * and the next CS# assert.  The window-register writes below
   * (multiple APB accesses at ~6.7 ns each) provide >50 ns of
   * margin before the first QPI-mode memory access (AC timing Table 10,
   * datasheet p. 23).
   */
  qmi_hw->direct_csr = 0;

  /* Configure QMI window 1 timing for APS6404L Linear Burst mode. */
  uint32_t timing = 0u;
  if (!psram_compute_qmi_timing((uint32_t)clock_get_hz(clk_sys), &timing)) {
    restore_interrupts(intr_stash);
    return 0;
  }
  qmi_hw->m[1].timing = timing;

  /* Read format: 0xEB, quad command/address/data, six wait cycles
   * (APS6404L command table p. 9; QPI Fast Quad Read figure p. 16). */
  qmi_hw->m[1].rfmt =
      QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
      QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
      QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
      QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
      QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
      QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
      QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB;
  qmi_hw->m[1].rcmd = APS6404L_CMD_QPI_FAST_READ;

  /* Write format: 0x38, quad command/address/data, no wait cycles
   * (APS6404L command table p. 9; QPI Write figure p. 17). */
  qmi_hw->m[1].wfmt =
      QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
      QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
      QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
      QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
      QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
      QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
  qmi_hw->m[1].wcmd = APS6404L_CMD_QPI_WRITE;

  /* Enable writes to PSRAM through XIP. */
  hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

  restore_interrupts(intr_stash);
  return detected;
}
