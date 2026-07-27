#!/usr/bin/env python3
"""Fetch a live LVGL screenshot over Wi-Fi and save it as a PNG."""

import argparse
import os
import socket
import struct
import sys

import numpy as np
from PIL import Image


HEADER_FMT = "<HHHHI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def recv_exact(sock: socket.socket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = sock.recv(length - len(chunks))
        if not chunk:
            raise ConnectionError("connection closed before all bytes were received")
        chunks.extend(chunk)
    return bytes(chunks)


def rgb565_to_rgb888(data: bytes, width: int, height: int, stride: int, swap_bytes: bool = True) -> np.ndarray:
    pixels = np.frombuffer(data, dtype="<u2").reshape(height, stride // 2)[:, :width]

    if swap_bytes:
        # LVGL with swap_bytes=true sends big-endian RGB565, swap back
        pixels = ((pixels & 0xFF) << 8) | ((pixels >> 8) & 0xFF)

    red = ((pixels >> 11) & 0x1F).astype(np.uint8)
    green = ((pixels >> 5) & 0x3F).astype(np.uint8)
    blue = (pixels & 0x1F).astype(np.uint8)

    red = (red << 3) | (red >> 2)
    green = (green << 2) | (green >> 4)
    blue = (blue << 3) | (blue >> 2)

    return np.dstack((red, green, blue))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device_ip", help="device IP address")
    parser.add_argument("--port", type=int, default=3333, help="screenshot port")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "screenshots", "current.png"), help="output PNG path")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    args = parser.parse_args()

    with socket.create_connection((args.device_ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(b"S")

        header = recv_exact(sock, HEADER_SIZE)
        width, height, stride, _reserved, data_len = struct.unpack(HEADER_FMT, header)
        pixel_data = recv_exact(sock, data_len)

    rgb = rgb565_to_rgb888(pixel_data, width, height, stride, swap_bytes=True)
    image = Image.fromarray(rgb, "RGB")

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    image.save(args.out)

    print(f"saved {width}x{height} screenshot to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())