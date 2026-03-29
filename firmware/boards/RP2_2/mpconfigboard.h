// Board and hardware specific configuration for RP2-2 prototype logger.
#define MICROPY_HW_BOARD_NAME                   "RP2-2 (Pimoroni Pico LiPo 2 XL W)"
#define MICROPY_HW_FLASH_STORAGE_BYTES          (PICO_FLASH_SIZE_BYTES - 1536 * 1024)

// Enable networking.
#define MICROPY_PY_NETWORK                      (1)
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "RP2-2"

// CYW43 driver configuration.
#define CYW43_USE_SPI                           (1)
#define CYW43_LWIP                              (1)
#define CYW43_GPIO                              (1)
#define CYW43_SPI_PIO                           (1)

#define MICROPY_HW_PIN_EXT_COUNT                CYW43_WL_GPIO_COUNT

// Pico LiPo 2 XL W has onboard PSRAM on GP47.
#define MICROPY_HW_ENABLE_PSRAM                 (1)
#define MICROPY_HW_PSRAM_CS_PIN                 (47)
#define MICROPY_GC_SPLIT_HEAP                   (1)

int mp_hal_is_pin_reserved(int n);
#define MICROPY_HW_PIN_RESERVED(i) mp_hal_is_pin_reserved(i)

// RP2-2 defaults:
// - PiCowbell PCF8523 RTC on I2C0 (SDA=GP4, SCL=GP5)
// - PiCowbell microSD on SPI0 (SCK=GP18, MOSI=GP19, MISO=GP16, CS=GP17)
// - PiCowbell SD detect optional jumper to GP15
// - BOOT / user switch on GP30 (active low)
// - Battery sense on GP43
#define MICROPY_HW_I2C0_SDA                     (4)
#define MICROPY_HW_I2C0_SCL                     (5)

#define MICROPY_HW_SPI0_SCK                     (18)
#define MICROPY_HW_SPI0_MOSI                    (19)
#define MICROPY_HW_SPI0_MISO                    (16)
