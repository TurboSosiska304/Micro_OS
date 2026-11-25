# The plan of 2.0.2

2.0.0 and 2.0.1 were tested on a disk image this tree writes itself - a
writable root, a framebuffer in RAM - and both were broken on the medium a
person actually boots: the UEFI stick of a laptop. This release is not a
feature: it is the machine of 2.0 on the medium the release hands out, with a
gate that boots that medium.

## The defects the owner's laptop found

1. **The desk on a read-only root.** `boot0p1` is mounted read-only by the rule
   of ADR 0041, and `wm` keeps the pixels of a window in a file of `/tmp`:

   ```
   root: boot0p1 cannot open its writes, the root stays read-only
   wm: no file for the pixels of a window
   wm: /bin/term exited with code 138
   ```

   The desktop says "the desktop is up" and has no window. `settings` cannot
   write `/etc/display` either, so the palette and the font of the machine are
   the defaults of the build.
2. **A framebuffer without write combining.** The pixels of the screen go to
   the memory of the video card with no MTRR and no PAT entry: every 4 bytes is
   one uncached transaction on a real machine. The console prints 5-10
   characters a second after the kernel takes the screen, and `wm` draws at the
   same speed. In QEMU the video memory is RAM, so no gate saw it.
3. **The end of a machine that does not switch off.** After the clean end and
   `power: the machine goes down: off`, the laptop keeps the machine on and the
   kernel faults on a user address:

   ```
   x86-64: exception 14 (page fault), error 0x0000000000000000,
           rip 0xffffffff800655c0, cr2 0x00000000bffed1f4
   ```

   Error 0 is a page that is not there: the shutdown tore an address space
   down and something read it afterwards.
4. **The tests ran a different machine.** `x86-vga-smoke` starts `wm` from
   `init` on a writable disk; the shell starts it after `exec`, on a stick.

## The gates of the release

* `make uefi-stick` and a smoke that boots **the stick it wrote**: the desk from
  the shell (`exec /bin/wm`), `Alt+T` opens `/bin/term`, the terminal answers
  `help`, `Alt+F4` closes it, the prompt returns.
* The same boot of `x86-iso` (the ISO is read-only by design and must have the
  same desk).
* `tmpfs` on `/tmp` when the root is read-only, with a check that a file of 4
  MiB can be written there and that it is gone after a reboot.
* The framebuffer mapped write-combining, with a number: the time of one screen
  of text and of one frame of `wm`, before and after, on a machine of QEMU with
  the write-combining emulated and on the laptop of the owner.
* `poweroff` on the laptop reaches S5 and the machine is off, with no fault
  after the write of the sleep type; `make power-smoke` covers the path with
  `acpi=off` as well.
* The full gate of 2.0.1 again, since the merge of these paths touches the
  kernel, the port and the desk.
