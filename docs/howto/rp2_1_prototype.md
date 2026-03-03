# How-to — RP2-1 prototype hardware profile

Status: Active
Last updated: 2026-03-02

`RP2-1` is the first logger prototype target.

## BOM

- Pimoroni Pico Plus 2 W  
  https://shop.pimoroni.com/products/pimoroni-pico-plus-2-w?variant=42182811942995
- RV3028 RTC Breakout  
  https://shop.pimoroni.com/products/rv3028-real-time-clock-rtc-breakout?variant=27926940549203
- LiPo SHIM for Pico  
  https://shop.pimoroni.com/products/pico-lipo-shim?variant=32369543086163
- Adafruit SPI Flash SD Card breakout  
  https://shop.pimoroni.com/products/adafruit-spi-flash-sd-card?variant=53511868154235

## Wiring (validated)

> Important power note: for RP2-1 peripherals, use the board's **3V3** rail for logic power.

### RV3028 RTC breakout

| RV3028 pin | Pico Plus 2 W connection | Notes |
|---|---|---|
| VCC | `3V3(OUT)` (physical pin 36) | 3.3V logic/power rail |
| GND | any GND (e.g. physical pin 38) | common ground |
| SDA | `GP12` (physical pin 16) | `I2C0 SDA` |
| SCL | `GP13` (physical pin 17) | `I2C0 SCL` |
| INT/CLKOUT | `GP11` (physical pin 15) | optional interrupt/clock output |

I2C address: `0x52`.

### Adafruit SPI Flash SD Card breakout

| Adafruit SD pin | Pico Plus 2 W connection | Notes |
|---|---|---|
| VIN | `3V3(OUT)` (physical pin 36) | breakout supports 3V-5V VIN |
| GND | any GND (e.g. physical pin 23/38) | common ground |
| SCK | `GP18` (physical pin 24) | `SPI0 SCK` |
| MOSI | `GP19` (physical pin 25) | `SPI0 TX` |
| MISO | `GP20` (physical pin 26) | `SPI0 RX` |
| CS | `GP17` (physical pin 22) | RP2-1 default chip-select |

### LiPo SHIM for Pico

LiPo SHIM is power-path hardware and does not require firmware pin configuration.

From the shim schematic in notes (`lipo_shim_for_pico_schematic.pdf`), it interfaces with Pico power rails:

- `VBUS` (physical pin 40)
- `VSYS` (physical pin 39)
- `3V3_EN` (physical pin 37)
- `3V3(OUT)` (physical pin 36)
- GND

## Bus allocation used by firmware

- **RTC (RV3028): I2C0** on `GP12`/`GP13`
- **SD breakout: SPI0** on `GP18`/`GP19`/`GP20`, CS `GP17`

Note: RP2-1 intentionally uses `I2C0` on `GP12/GP13` (not the board's default `GP4/GP5` pair).

These defaults are encoded in:

- `firmware/boards/RP2_1/mpconfigboard.h`
- `firmware/boards/RP2_1/pins.csv`

## Firmware target mapping

RP2-1 presets select:

- `MICROPY_BOARD=RP2_1`
- `MICROPY_BOARD_DIR=firmware/boards/RP2_1`
- Pico SDK board: `PICO_BOARD=pimoroni_pico_plus2_w_rp2350`

RP2-1 presets include RTC user modules:

- `breakout_rtc` (RV3028)
- `pimoroni_i2c`
- `pimoroni_bus`

## Minimal MicroPython bring-up snippet

```python
from machine import Pin, I2C, SPI
from breakout_rtc import BreakoutRTC
import sdcard

# RTC (I2C0 on GP12/GP13)
i2c0 = I2C(0, sda=Pin(12), scl=Pin(13), freq=400_000)
print("I2C0 devices:", [hex(x) for x in i2c0.scan()])
rtc = BreakoutRTC(i2c0)
print(rtc.update_time(), rtc.string_time(), rtc.string_date())

# SD (SPI0 on GP18/GP19/GP20, CS GP17)
spi0 = SPI(0, baudrate=1_000_000, sck=Pin(18), mosi=Pin(19), miso=Pin(20))
sd_cs = Pin(17, Pin.OUT, value=1)
sd = sdcard.SDCard(spi0, sd_cs)
print("SD OK", sd)
```
