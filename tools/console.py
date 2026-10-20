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


def reset_board(ser) -> None:
    """Reset the board through the DTR/RTS lines of the USB serial chip.

    The CH9102 of the StickC Plus2 is wired to the auto reset circuit of the
    ESP32: RTS drives EN (reset), DTR drives GPIO0 (boot mode). Holding IO0
    high and pulsing EN low boots the application.
    """
    ser.dtr = False  # IO0 high: boot the application, not the loader
    ser.rts = True   # EN low: reset
    time.sleep(0.1)
    ser.rts = False  # EN high: run
    time.sleep(0.2)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    do_reset = "--reset" in sys.argv

    # Every --send is followed by the commands to write to the board.
    sends: list[str] = []
    argv = sys.argv[1:]
    index = 0
    while index < len(argv):
        if argv[index] == "--send":
            index += 1
            while index < len(argv) and not argv[index].startswith("--"):
                sends.append(argv[index])
                index += 1
            continue
        index += 1

    port = args[0] if len(args) > 0 else find_port()
    seconds = float(args[1]) if len(args) > 1 else 20.0

    print(f"--- console on {port} for {seconds:.0f}s ---", flush=True)

    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except Exception as exc:  # noqa: BLE001
        print(f"cannot open {port}: {exc}", file=sys.stderr)
        return 2

    if do_reset:
        print("--- resetting the board ---", flush=True)
        reset_board(ser)
        # The board needs a moment to boot, the commands would be lost.
        time.sleep(3.0)

    for line in sends:
        print(f"--- sending: {line}", flush=True)
        ser.write((line + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.4)

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
