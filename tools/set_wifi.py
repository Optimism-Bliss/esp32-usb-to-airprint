#!/usr/bin/env python3
"""Set the printer adapter's Wi-Fi credentials over its USB-to-UART COM port."""

import argparse
import getpass
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="ESP32 COM port (e.g. COM5 or /dev/ttyUSB0)")
    args = parser.parse_args()
    ssid = input("Wi-Fi SSID (2.4 GHz): ").encode()
    password = getpass.getpass("Wi-Fi password: ").encode()
    if not 1 <= len(ssid) <= 32 or (password and not 8 <= len(password) <= 64):
        parser.error("SSID must be 1–32 bytes and password must be empty or 8–64 bytes")
    with serial.Serial(args.port, 115200, timeout=0.25) as port:
        time.sleep(2)
        port.reset_input_buffer()
        port.write(f"WIFI {ssid.hex()} {password.hex()}\n".encode("ascii"))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            reply = port.readline()
            if b"WIFI setup saved" in reply:
                print("Wi-Fi saved. The board is restarting.")
                return
            if b"WIFI setup rejected" in reply or b"WIFI setup failed" in reply:
                parser.exit(1, "The board rejected the Wi-Fi configuration.\n")
    parser.exit(1, "No confirmation. Check that the board is running and using its COM port.\n")


if __name__ == "__main__":
    main()
