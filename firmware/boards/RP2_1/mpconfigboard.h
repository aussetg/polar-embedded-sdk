// Board and hardware specific configuration for RP2-1 prototype logger.
#define MICROPY_HW_BOARD_NAME                   "RP2-1 (Pimoroni Pico Plus 2 W)"
#define MICROPY_HW_FLASH_STORAGE_BYTES          (PICO_FLASH_SIZE_BYTES - 1536 * 1024)

// Enable networking.
#define MICROPY_PY_NETWORK                      (1)
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "RP2-1"

// CYW43 driver configuration.
#define CYW43_USE_SPI                           (1)
#define CYW43_LWIP                              (1)
#define CYW43_GPIO                              (1)
#define CYW43_SPI_PIO                           (1)

#define MICROPY_HW_PIN_EXT_COUNT                CYW43_WL_GPIO_COUNT

int mp_hal_is_pin_reserved(int n);
#define MICROPY_HW_PIN_RESERVED(i) mp_hal_is_pin_reserved(i)

// RP2-1 defaults:
// - RTC (RV3028) on I2C0 (SDA=GP12, SCL=GP13, INT=GP11)
// - SD breakout on SPI0 (SCK=GP18, MOSI=GP19, MISO=GP20, CS=GP17)
#define MICROPY_HW_I2C0_SDA                     (12)
#define MICROPY_HW_I2C0_SCL                     (13)

#define MICROPY_HW_SPI0_SCK                     (18)
#define MICROPY_HW_SPI0_MOSI                    (19)
#define MICROPY_HW_SPI0_MISO                    (20)
