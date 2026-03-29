// RP2-2 logger appliance policy/configuration scaffold.
//
// This file is application-facing board policy, not the low-level pico-sdk
// board definition. The hardware profile is documented in:
//   docs/howto/rp2_2_prototype.md

#ifndef LOGGER_FIRMWARE_RP2_2_BOARD_CONFIG_H
#define LOGGER_FIRMWARE_RP2_2_BOARD_CONFIG_H

#define LOGGER_BOARD_PROFILE "RP2-2"
#define LOGGER_BOARD_NAME "Pimoroni Pico LiPo 2 XL W + Adafruit PiCowbell Adalogger"

// User I/O
#define LOGGER_BUTTON_PIN 30
#define LOGGER_BUTTON_ACTIVE_LOW 1

// External RTC on the PiCowbell Adalogger
#define LOGGER_RTC_I2C_BUS 0
#define LOGGER_RTC_SDA_PIN 4
#define LOGGER_RTC_SCL_PIN 5
#define LOGGER_RTC_I2C_ADDR 0x68

// microSD on the PiCowbell Adalogger
#define LOGGER_SD_SPI_BUS 0
#define LOGGER_SD_MISO_PIN 16
#define LOGGER_SD_CS_PIN 17
#define LOGGER_SD_SCK_PIN 18
#define LOGGER_SD_MOSI_PIN 19
#define LOGGER_SD_DETECT_PIN 15
#define LOGGER_SD_DETECT_OPTIONAL 1

// Power-related pins on the Pico LiPo 2 XL W
#define LOGGER_BATTERY_SENSE_PIN 43

// Study/product policy constants from the v1 spec
#define LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL 4
#define LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL 22
#define LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL 6

#define LOGGER_BATTERY_CRITICAL_STOP_MV 3500
#define LOGGER_BATTERY_LOW_START_BLOCK_MV 3650
#define LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV 3850

#define LOGGER_SD_MIN_FREE_RESERVE_BYTES (512u * 1024u * 1024u)

#endif
