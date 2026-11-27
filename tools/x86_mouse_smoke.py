#!/usr/bin/env python3
"""Micro OS — a small smoke for the PS/2 mouse of the x86-64 port.

Boot the desktop, move the pointer through the QEMU monitor and prove that the
cursor moved in the picture the machine drew. It is the narrow check the task
asks for; the full pointer suite lives in tools/x86_vga_smoke.py.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time

import x86_vga_smoke as vga


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="build-x86_64/disk.img")
    parser.add_argument("--loader", default="build-x86_64/loader32.elf")
    parser.add_argument("--kernel", default="build-x86_64/microos64.bin")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--display", default="none")
    parser.add_argument("--shots", default="")
    args = parser.parse_args(argv)

    shots = args.shots or os.path.join(tempfile.gettempdir(), "microos-mouse-smoke")
    os.makedirs(shots, exist_ok=True)
    vga.SHOTS_DIR = shots

    checks: list[tuple[str, bool, str]] = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-mouse-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(args.image, disk)
        machine = vga.Machine(args.loader, args.kernel, workdir, "std", args.display, disk)
        try:
            booted = machine.wait_serial(vga.PROMPT, args.timeout)
            expect("the shell is up", booted, "no prompt")
            if not booted:
                raise RuntimeError("no shell")

            machine.type_line("exec /bin/wm")
            expect("wm takes the screen",
                   machine.wait_serial("fb: the console is taken", args.timeout),
                   "no FBIO_TAKE")
            expect("desktop is up",
                   machine.wait_serial("wm: the desktop is up", args.timeout),
                   "desktop never started")

            picture = vga.settled_picture(
                machine, workdir,
                lambda seen: seen.pixel(4, 4) == vga.WALL_TOP_RGB
                and seen.text_at(vga.WM_EDIT_RECT[0] + vga.UI_FRAME_PAD,
                                 vga.WM_EDIT_RECT[1] + vga.UI_TITLE_Y,
                                 "edit - /doc.txt", vga.EDIT_TITLE_INK_RGB, vga.EDIT_BAR_RGB),
                seconds=10.0,
            )
            expect("desktop picture is drawn", picture is not None, "no screendump")
            if picture is not None:
                shutil.copyfile(os.path.join(workdir, "screen.ppm"),
                                os.path.join(shots, "mouse-before.ppm"))

            if picture is not None:
                cx, cy = vga.VBE_WIDTH // 2, vga.VBE_HEIGHT // 2
                expect("cursor is drawn at the centre",
                       picture.pixel(cx, cy) == vga.WM_CURSOR_RGB,
                       f"centre pixel is {picture.pixel(cx, cy)}")

                # Move pointer to a known corner then to a visible spot.
                for _ in range(12):
                    machine.monitor_command("mouse_move -200 -200")
                    time.sleep(0.02)
                for _ in range(3):
                    machine.monitor_command("mouse_move 80 60")
                    time.sleep(0.04)
                time.sleep(0.3)

                moved = vga.settled_picture(machine, workdir, lambda seen: True, seconds=3.0)
                if moved is not None:
                    shutil.copyfile(os.path.join(workdir, "screen.ppm"),
                                    os.path.join(shots, "mouse-moved.ppm"))
                    # Cursor should no longer be at centre in the same form;
                    # and some white cursor ink should exist near target.
                    tx, ty = 240, 180
                    found = False
                    for dy in range(-2, 20):
                        for dx in range(-2, 16):
                            px, py = tx + dx, ty + dy
                            if 0 <= px < vga.VBE_WIDTH and 0 <= py < vga.VBE_HEIGHT:
                                if moved.pixel(px, py) == vga.WM_CURSOR_RGB:
                                    found = True
                                    break
                        if found:
                            break
                    expect("cursor moved to the target area", found,
                           "no cursor ink found near expected position")
                    expect("centre no longer has cursor ink",
                           moved.pixel(cx, cy) != vga.WM_CURSOR_RGB,
                           "cursor still at centre")
                else:
                    expect("cursor moved to the target area", False, "no screendump after move")
                    expect("centre no longer has cursor ink", False, "no screendump after move")


        finally:
            machine.close()

    print("Micro OS — x86-64 PS/2 mouse smoke")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")
    print("=" * 60)
    failed = [c for c in checks if not c[1]]
    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s)")
        return 1
    print(f"OK: {len(checks)} check(s) passed")
    print(f"screenshots: {shots}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
