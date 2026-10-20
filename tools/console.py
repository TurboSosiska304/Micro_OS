#!/usr/bin/env python3
"""Micro OS — read the UART console of the board.

Usage:
    tools/console.py [port] [seconds]

The port is auto detected when it is not given. The script prints whatever
the board sends and exits with the status 0 when the self test passed.
"""

import glob
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(3)


def find_port() -> str:
    candidates: list[str] = []
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates += sorted(glob.glob(pattern))
    if not candidates:
        print("No serial port found", file=sys.stderr)
        raise SystemExit(2)
    return candidates[0]


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

    print(f"--- console on {port} for {seconds:.0f}s ---", flush=True)

    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except Exception as exc:  # noqa: BLE001
        print(f"cannot open {port}: {exc}", file=sys.stderr)
        return 2

    deadline = time.time() + seconds
    buffer = ""

    try:
        while time.time() < deadline:
            try:
                data = ser.read(256)
            except serial.SerialException:
                break
            if not data:
                continue
            text = data.decode("utf-8", errors="replace")
            buffer += text
            print(text, end="", flush=True)
    finally:
        ser.close()

    print("\n--- end of capture ---", flush=True)

    if "selftest: PASS" in buffer:
        print("RESULT: PASS")
        return 0

    print("RESULT: FAIL (no 'selftest: PASS' in the output)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
