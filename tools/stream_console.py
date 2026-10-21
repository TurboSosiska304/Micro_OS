#!/usr/bin/env python3
"""Micro OS — an interactive console for the board.

The script connects to the serial port of the board and lets you talk to the
kernel as if it were a terminal: every key is sent to the board, everything the
board answers is printed.

    tools/stream_console.py                 # the port is detected
    tools/stream_console.py /dev/ttyACM0    # an explicit port

Keys:
    Ctrl+Q  leave the console
    Ctrl+C  is sent to the board (the shell stops the current command)
"""

import argparse
import os
import select
import sys

try:
    import serial
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(3)

EXIT_KEY = 0x11  # Ctrl+Q


def find_port() -> str:
    candidates: list[str] = []
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates += sorted(glob_paths(pattern))
    if not candidates:
        print("No serial port found", file=sys.stderr)
        raise SystemExit(2)
    return candidates[0]


def glob_paths(pattern: str) -> list[str]:
    import glob

    return glob.glob(pattern)


def main() -> int:
    parser = argparse.ArgumentParser(description="interactive console of Micro OS")
    parser.add_argument("port", nargs="?", help="the serial port of the board")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--raw", action="store_true", help="do not translate the keys")
    args = parser.parse_args()

    port = args.port or find_port()

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except Exception as exc:  # noqa: BLE001
        print(f"cannot open {port}: {exc}", file=sys.stderr)
        return 2

    # The terminal of the user: the keys are sent to the board as they are.
    old_settings = None
    if os.isatty(sys.stdin.fileno()):
        import termios
        import tty

        old_settings = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())

    print(f"--- Micro OS on {port}, Ctrl+Q to leave ---", flush=True)

    try:
        while True:
            ready, _, _ = select.select([sys.stdin, ser], [], [], 0.2)

            if ser in ready:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    text = data.decode("utf-8", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()

            if sys.stdin in ready:
                key = os.read(sys.stdin.fileno(), 1)
                if not key:
                    break
                if key[0] == EXIT_KEY:
                    break
                ser.write(key)
                ser.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if old_settings is not None:
            import termios

            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_settings)
        ser.close()
        print("\n--- console closed ---", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
