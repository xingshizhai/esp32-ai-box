#!/usr/bin/env python3
"""Request an LVGL framebuffer over the debug console and save it as PNG."""
import argparse
import base64
import re
import time

import serial
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="lvgl-serial.png")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as device:
        device.reset_input_buffer()
        device.write(b"screenshot serial\n")
        deadline = time.monotonic() + args.timeout
        header = None
        payload = []
        while time.monotonic() < deadline:
            line = device.readline().decode("ascii", errors="ignore").strip()
            match = re.search(r"<<<LVGLSHOT (\d+) (\d+) (\d+)>>>", line)
            if match:
                header = tuple(map(int, match.groups()))
                payload.clear()
            elif header and "<<<LVGLSHOT END>>>" in line:
                break
            elif header and re.fullmatch(r"[A-Za-z0-9+/=]+", line):
                payload.append(line)
        else:
            raise TimeoutError("serial screenshot timed out")

    width, height, expected = header
    raw = base64.b64decode("".join(payload))
    if len(raw) != expected:
        raise ValueError(f"frame size mismatch: got {len(raw)}, expected {expected}")
    image = Image.frombytes("RGB", (width, height), raw, "raw", "BGR;16")
    image.save(args.out)
    print(f"saved {width}x{height} screenshot to {args.out}")


if __name__ == "__main__":
    main()
