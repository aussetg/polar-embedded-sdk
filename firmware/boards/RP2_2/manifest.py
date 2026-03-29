include("$(PORT_DIR)/boards/manifest.py")

require("bundle-networking")

# Bluetooth
require("aioble")

# SPI SD card helper (drivers/storage/sdcard.py)
require("sdcard")

# Local frozen helper for the PiCowbell PCF8523 RTC.
freeze("modules")
