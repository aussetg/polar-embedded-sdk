# Quick BLE scan helper to find Polar advertisements and addresses.

import bluetooth
import time
from micropython import const

_IRQ_SCAN_RESULT = const(5)
_IRQ_SCAN_DONE = const(6)

ble = bluetooth.BLE()
ble.active(True)

seen = {}


def irq(event, data):
    if event == _IRQ_SCAN_RESULT:
        addr_type, addr, adv_type, rssi, adv_data = data
        name = None

        i = 0
        while i + 1 < len(adv_data):
            ln = adv_data[i]
            if ln == 0:
                break
            t = adv_data[i + 1]
            v = adv_data[i + 2 : i + 1 + ln]
            if t in (0x08, 0x09):
                try:
                    name = bytes(v).decode()
                except Exception:
                    name = repr(bytes(v))
            i += 1 + ln

        addr_s = ":".join("%02X" % b for b in bytes(addr))
        if name and "Polar" in name:
            seen[addr_s] = (name, rssi)
            print("Polar", addr_s, "name=", name, "rssi=", rssi)

    elif event == _IRQ_SCAN_DONE:
        print("scan done")


ble.irq(irq)

print("scanning 8000ms...")
ble.gap_scan(8000, 30000, 30000, True)
time.sleep_ms(8500)

print("seen:", seen)
