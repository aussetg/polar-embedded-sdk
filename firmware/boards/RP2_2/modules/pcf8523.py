"""Minimal MicroPython PCF8523 RTC driver for RP2-2.

This driver is intentionally small and dependency-free. It supports the parts
of the PiCowbell Adalogger RTC that we currently need:

- read/write datetime
- detect oscillator-stop / lost-power state
- detect low backup-battery state
- copy the external RTC time into `machine.RTC()`

Datetime tuples use the order:

    (year, month, day, weekday, hour, minute, second)

with weekday in the usual MicroPython/CPython `time.localtime()` style range
0..6.
"""

import time

try:
    from micropython import const
except ImportError:  # pragma: no cover - CPython lint/import convenience
    def const(value):
        return value


_REG_CONTROL1 = const(0x00)
_REG_CONTROL3 = const(0x02)
_REG_SECONDS = const(0x03)
_REG_MINUTES = const(0x04)
_REG_HOURS = const(0x05)
_REG_DAYS = const(0x06)
_REG_WEEKDAYS = const(0x07)
_REG_MONTHS = const(0x08)
_REG_YEARS = const(0x09)

_CONTROL3_PM_MASK = const(0xE0)
_CONTROL3_BLF = const(0x04)

_SECONDS_OS = const(0x80)

_DEFAULT_ADDRESS = const(0x68)


def _bcd_decode(value):
    return ((value >> 4) * 10) + (value & 0x0F)


def _bcd_encode(value):
    if not 0 <= value <= 99:
        raise ValueError("BCD value out of range")
    return ((value // 10) << 4) | (value % 10)


class PCF8523:
    ADDRESS = _DEFAULT_ADDRESS

    def __init__(self, i2c, address=_DEFAULT_ADDRESS):
        self._i2c = i2c
        self._address = address
        self._buf1 = bytearray(1)
        self._buf7 = bytearray(7)

    def _read_reg(self, reg):
        return self._i2c.readfrom_mem(self._address, reg, 1)[0]

    def _write_reg(self, reg, value):
        self._buf1[0] = value & 0xFF
        self._i2c.writeto_mem(self._address, reg, self._buf1)

    def _update_reg(self, reg, mask, value):
        current = self._read_reg(reg)
        current = (current & ~mask) | (value & mask)
        self._write_reg(reg, current)

    def lost_power(self):
        return bool(self._read_reg(_REG_SECONDS) & _SECONDS_OS)

    def battery_low(self):
        return bool(self._read_reg(_REG_CONTROL3) & _CONTROL3_BLF)

    def datetime(self):
        data = self._i2c.readfrom_mem(self._address, _REG_SECONDS, 7)
        second = _bcd_decode(data[0] & 0x7F)
        minute = _bcd_decode(data[1] & 0x7F)
        hour = _bcd_decode(data[2] & 0x3F)
        day = _bcd_decode(data[3] & 0x3F)
        weekday = data[4] & 0x07
        month = _bcd_decode(data[5] & 0x1F)
        year = 2000 + _bcd_decode(data[6])
        return (year, month, day, weekday, hour, minute, second)

    def set_datetime(self, value):
        value = tuple(value)
        if len(value) != 7:
            raise ValueError("expected (year, month, day, weekday, hour, minute, second)")

        year, month, day, weekday, hour, minute, second = value
        if not 2000 <= year <= 2099:
            raise ValueError("year out of range: expected 2000..2099")
        if not 1 <= month <= 12:
            raise ValueError("month out of range")
        if not 1 <= day <= 31:
            raise ValueError("day out of range")
        if not 0 <= weekday <= 6:
            raise ValueError("weekday out of range: expected 0..6")
        if not 0 <= hour <= 23:
            raise ValueError("hour out of range")
        if not 0 <= minute <= 59:
            raise ValueError("minute out of range")
        if not 0 <= second <= 59:
            raise ValueError("second out of range")

        # Enable standard battery switchover + detection before updating the
        # time registers.
        self._update_reg(_REG_CONTROL3, _CONTROL3_PM_MASK, 0)

        self._buf7[0] = _bcd_encode(second) & 0x7F
        self._buf7[1] = _bcd_encode(minute) & 0x7F
        self._buf7[2] = _bcd_encode(hour) & 0x3F
        self._buf7[3] = _bcd_encode(day) & 0x3F
        self._buf7[4] = weekday & 0x07
        self._buf7[5] = _bcd_encode(month) & 0x1F
        self._buf7[6] = _bcd_encode(year - 2000)
        self._i2c.writeto_mem(self._address, _REG_SECONDS, self._buf7)

        # Ensure STOP is cleared after writing the clock registers.
        self._update_reg(_REG_CONTROL1, 0x20, 0x00)

    def set_from_localtime(self, value=None):
        if value is None:
            value = time.localtime()
        value = tuple(value)
        if len(value) < 7:
            raise ValueError("expected localtime()/struct_time-like tuple")
        self.set_datetime((value[0], value[1], value[2], value[6], value[3], value[4], value[5]))

    def machine_datetime(self):
        year, month, day, weekday, hour, minute, second = self.datetime()
        return (year, month, day, weekday, hour, minute, second, 0)

    def sync_machine_rtc(self, rtc=None):
        if rtc is None:
            from machine import RTC

            rtc = RTC()
        rtc.datetime(self.machine_datetime())
        return rtc
