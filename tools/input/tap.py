#!/usr/bin/env python3
"""Send synthetic touch events to the Smart Buddy debug input server.

Usage:
    python tools/input/tap.py <device-ip> tap X Y [--port 3334]
    python tools/input/tap.py <device-ip> down X Y
    python tools/input/tap.py <device-ip> move X Y
    python tools/input/tap.py <device-ip> up X Y

Connects to the firmware's debug_input TCP server and injects an LVGL
pointer event at the given screen coordinates (320x240), without going
through the I2C touch controller. 'tap' presses and releases in one
command; 'down'/'move'/'up' allow drags or multi-step gestures over a
single connection.
"""
import argparse
import socket
import struct
import sys

CMD_FMT = "<BHH"  # cmd, x, y
CMD_BYTES = {
    "tap": b"T",
    "down": b"D",
    "move": b"M",
    "up": b"U",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device_ip", help="IP address of the Smart Buddy device")
    parser.add_argument("action", choices=sorted(CMD_BYTES), help="gesture action")
    parser.add_argument("x", type=int, help="x coordinate (0-319)")
    parser.add_argument("y", type=int, help="y coordinate (0-239)")
    parser.add_argument("--port", type=int, default=3334, help="debug input server TCP port (default 3334)")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    args = parser.parse_args()

    cmd = CMD_BYTES[args.action][0]
    packet = struct.pack(CMD_FMT, cmd, args.x, args.y)

    with socket.create_connection((args.device_ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(packet)
        ack = sock.recv(1)

    if ack != b"K":
        print(f"warning: unexpected ack {ack!r}", file=sys.stderr)

    print(f"{args.action} ({args.x}, {args.y}) -> ack={ack!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
