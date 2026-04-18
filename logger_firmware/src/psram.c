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

/*
 * Read the PSRAM JEDEC ID via QMI direct mode.
 * Returns the detected size in bytes, or 0 if no PSRAM found.
 */
static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
  /* Enable direct mode with a slow clock for the ID read. */
  qmi_hw->direct_csr =
      30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

  /* Wait for any in-flight XIP transfer to drain. */
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }

  /* Reset any prior QPI state: toggle CS1 with a quad NOP. */
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                      QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                      0xf5u;
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

  /* Read ID: command 0x9f, then 7 dummy/return bytes.  Byte 5 = kgd, byte 6
   * = eid. */
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  uint8_t kgd = 0;
  uint8_t eid = 0;

  for (size_t i = 0; i < 7; ++i) {
    qmi_hw->direct_tx = (i == 0) ? 0x9fu : 0xffu;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
    }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }
    if (i == 5) {
      kgd = (uint8_t)qmi_hw->direct_rx;
    } else if (i == 6) {
      eid = (uint8_t)qmi_hw->direct_rx;
    } else {
      (void)qmi_hw->direct_rx;
    }
  }

  /* Release CS1 and disable direct mode. */
  qmi_hw->direct_csr &=
      ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

  if (kgd != 0x5Du) {
    return 0;
  }

  /* Decode size from EID. */
  size_t psram_size = 1024u * 1024u; /* base: 1 MiB */
  uint8_t size_id = eid >> 5;
  if (eid == 0x26u || size_id == 2u) {
    psram_size *= 8u; /* 8 MiB */
  } else if (size_id == 0u) {
    psram_size *= 2u;
  } else if (size_id == 1u) {
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

  /* Switch PSRAM into QPI mode via direct command 0x35. */
  qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB |
                       QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
  }

  qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
  while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
  }

  /*
   * Disable direct mode.  This releases CS1; the PSRAM is now in QPI
   * mode.  The APS6404L requires tCPH >= 18 ns between CS# deassert
   * and the next CS# assert.  The window-register writes below
   * (multiple APB accesses at ~6.7 ns each) provide >50 ns of
   * margin before the first QPI-mode memory access.
   */
  qmi_hw->direct_csr = 0;

  /* Configure QMI window 1 timing for the APS6404L-3SQR. */
  const int max_psram_freq = 133000000;
  const int clock_hz = (int)clock_get_hz(clk_sys);
  int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
  if (divisor == 1 && clock_hz > 100000000) {
    divisor = 2;
  }
  int rxdelay = divisor;
  if (clock_hz / divisor > 100000000) {
    rxdelay += 1;
  }

  const int64_t clock_period_fs = 1000000000000000ll / (int64_t)clock_hz;
  const int max_select = (int)((125 * 1000000) / clock_period_fs);
  const int min_deselect_raw =
      (int)((18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs) -
      (divisor + 1) / 2;
  const uint32_t min_deselect =
      (uint32_t)(min_deselect_raw > 0 ? min_deselect_raw : 0);

  qmi_hw->m[1].timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                        QMI_M1_TIMING_PAGEBREAK_VALUE_1024
                            << QMI_M1_TIMING_PAGEBREAK_LSB |
                        (uint32_t)max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                        (uint32_t)rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                        (uint32_t)divisor << QMI_M1_TIMING_CLKDIV_LSB;

  /* Read format: quad prefix, quad address, quad suffix, 6 dummy cycles. */
  qmi_hw->m[1].rfmt =
      QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
      QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
      QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
      QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
      QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
      QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
      6u << QMI_M0_RFMT_DUMMY_LEN_LSB;
  qmi_hw->m[1].rcmd = 0xEBu;

  /* Write format: quad prefix, quad address, quad suffix, no dummy. */
  qmi_hw->m[1].wfmt =
      QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
      QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
      QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
      QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
      QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
      QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
  qmi_hw->m[1].wcmd = 0x38u;

  /* Enable writes to PSRAM through XIP. */
  hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

  restore_interrupts(intr_stash);
  return detected;
}
