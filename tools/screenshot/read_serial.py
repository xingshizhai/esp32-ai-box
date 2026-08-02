#!/usr/bin/env python3
"""Read raw lines from a serial port for a fixed duration and print them.

Used as a non-TTY alternative to `idf.py monitor` for grabbing boot logs
(e.g. to find the device's IP address) when stdin is not a TTY.

Usage:
    python tools/screenshot/read_serial.py COM3 [--baud 115200] [--seconds 15] [--reset]
"""
import argparse
import sys
import time

import serial


def reset_board(ser: serial.Serial) -> None:
    """Toggle DTR/RTS to perform a hardware reset, like esptool's default reset."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--reset", action="store_true", help="toggle DTR/RTS to reset the board before reading")
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    try:
        if args.reset:
            reset_board(ser)
        end = time.time() + args.seconds
        while time.time() < end:
            line = ser.readline()
            if line:
                sys.stdout.write(line.decode("utf-8", errors="replace"))
                sys.stdout.flush()
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
