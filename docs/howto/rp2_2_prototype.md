# How-to — RP2-2 prototype hardware profile

Status: Active
Last updated: 2026-03-29

`RP2-2` is the second logger prototype target.

## BOM

- Pimoroni Pico LiPo 2 XL W  
  https://shop.pimoroni.com/products/pimoroni-pico-lipo-2-xl-w?variant=55447911006587
- Adafruit PiCowbell Adalogger for Pico  
  https://www.adafruit.com/product/5703

## Assembly notes

- `RP2-2` uses the **Pico-compatible 40-pin subset nearest the USB-C end** of
  the Pico LiPo 2 XL W.
- The PiCowbell Adalogger should therefore be mounted **toward the USB end** of
  the Pico LiPo 2 XL W, matching Pimoroni's compatibility note for Pico-sized
  add-ons on the XL board.
- The Pico LiPo 2 XL W contributes:
  - RP2350B + RM2 wireless/Bluetooth
  - 16 MB flash
  - 8 MB PSRAM
  - BOOT / user switch on `GP30` (active low)
  - battery sense on `GP43`
  - VBUS sense on `WL_GPIO2`

## Wiring / effective pin mapping

The PiCowbell Adalogger is a stacking board, so these are the effective nets
used by firmware rather than loose jumper wires.

### Adafruit PiCowbell Adalogger

| PiCowbell function | Pico LiPo 2 XL W connection | Notes |
|---|---|---|
| `3V` | board `3V3` rail | logic power |
| `G` | board GND | common ground |
| `SDA` | `GP4` | `I2C0 SDA` |
| `SCL` | `GP5` | `I2C0 SCL` |
| RTC | PCF8523 @ `0x68` | backup battery via CR1220 |
| `MI` | `GP16` | `SPI0 MISO` for microSD |
| `CS` | `GP17` | `SPI0 CS` for microSD |
| `SCK` | `GP18` | `SPI0 SCK` for microSD |
| `MO` | `GP19` | `SPI0 MOSI` for microSD |
| `SD Det` | optional `GP15` | only if the PiCowbell SD-detect jumper is soldered |

Notes:

- The PiCowbell RTC interrupt / square-wave output is **not wired to a Pico GPIO
  by default** in this prototype.
- Adafruit documents the PCF8523 default I2C address as `0x68`.

## Bus allocation used by firmware

- **RTC (PCF8523): I2C0** on `GP4`/`GP5`
- **microSD: SPI0** on `GP18`/`GP19`/`GP16`, CS `GP17`
- **SD detect (optional):** `GP15` if the PiCowbell solder jumper is closed

These defaults are encoded in:

- `firmware/boards/RP2_2/mpconfigboard.h`
- `firmware/boards/RP2_2/pins.csv`
- `firmware/boards/RP2_2/pimoroni_pico_lipo2xl_w.h`

## Firmware target mapping

RP2-2 presets select:

- `MICROPY_BOARD=RP2_2`
- `MICROPY_BOARD_DIR=firmware/boards/RP2_2`
- Pico SDK board: custom `PICO_BOARD=pimoroni_pico_lipo2xl_w`

RP2-2 firmware support includes:

- onboard CYW43 / RM2 networking + BLE
- onboard PSRAM enabled on `GP47`
- frozen `pcf8523.py` helper module for the PiCowbell RTC
- frozen `sdcard` driver from MicroPython's package manifest

## Minimal MicroPython bring-up snippet

```python
from machine import Pin, I2C, RTC, SPI
import pcf8523
import sdcard

# RTC (PiCowbell PCF8523 on I2C0 / GP4+GP5)
i2c0 = I2C(0, sda=Pin(4), scl=Pin(5), freq=400_000)
print("I2C0 devices:", [hex(x) for x in i2c0.scan()])

rtc_ext = pcf8523.PCF8523(i2c0)
print("lost_power", rtc_ext.lost_power(), "battery_low", rtc_ext.battery_low())
print("external rtc", rtc_ext.datetime())

# Optional: copy external RTC into the rp2 software RTC.
RTC().datetime(rtc_ext.machine_datetime())

# SD (PiCowbell microSD on SPI0 / GP16+17+18+19)
spi0 = SPI(0, baudrate=1_000_000, sck=Pin(18), mosi=Pin(19), miso=Pin(16))
sd_cs = Pin(17, Pin.OUT, value=1)
sd = sdcard.SDCard(spi0, sd_cs)
print("SD OK", sd)
```
