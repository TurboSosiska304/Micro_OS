# The x86-64 port: bare metal in QEMU

A port of the kernel for a machine with no operating system under it: no
libc, no POSIX, no firmware. It is the proof that the kernel is portable, and
the machine where a program can really be loaded and run (the window of
`execve` is here).

| | |
|---|---|
| port | `ports/port_x86_64` |
| loader | `loader32.S`: a multiboot image that QEMU loads with `-kernel`; it copies the kernel to 16 MiB, maps the same bytes at `0xFFFFFFFF80000000` (the top 2 GiB, where the image is linked), builds the identity map of the first gigabyte and turns the long mode on |
| console | the framebuffer of the VGA of QEMU, 1280x800 at 16 bits per pixel in RGB565 (Bochs VBE, ADR 0023): a grid of 160x50 cells of the 8x16 font of the PC (Spleen, `line_gap` 0/2/4 px of `/etc/font`), the colours of `/etc/console`; the text mode of 80x25 at `0xB8000` when the machine has no Bochs VBE interface; COM1, 115200, `\r\n`, out of one `k_port_write()` |
| input | the PS/2 keyboard on IRQ1 (scancode set 1, Shift, Ctrl and Caps Lock, and the E0 keys - the arrows, Home, End, PageUp, PageDown, Insert and Delete - as events of `/dev/input0`) or COM1 |
| clock | the PIT at 1 kHz on the core 0; the local APIC timer at 1 kHz on every application processor, calibrated against the TSC |
| tasks | `switch.S` (callee-saved registers and the stack), one run queue per core under `KERNEL_SMP`, and a round robin over it |
| processors | the MADT of ACPI, the trampoline at `0x7000`, INIT and the start-up IPI through the local APIC, one GDT/TSS/IST and one 64 KiB stack per core (ADR 0012) |
| store | `ata0`: a raw image on the primary IDE channel, PIO, MicroFS on its first partition, mounted at `/` (ADR 0008); `boot0p1`: the read-only root image of the ISO, as the second module of the boot (ADR 0026); `ram0`, 512 KB of the memory of the machine, when there is neither a disk nor a boot image |
| network | `virtio0`: virtio-net-pci in its legacy I/O interface, polled; ARP, IPv4, ICMP, UDP, a client of TCP (one segment in flight, a timeout retransmission) and a listener with SYN/ACK and `accept`; the resolver of QEMU user-net with a cache of eight names and their time to live; 10.0.2.15/24 on QEMU user-net, gateway 10.0.2.2, resolver 10.0.2.3, no DHCP |
| programs | `k_port_exec_supported()` is true; the window is 16 MiB at 32 MiB (`0x02000000`-`0x03000000`) over a physical arena at 64 MiB; a program is a file in `/bin` (the MOX1 format of `kernel/exec.h`), runs in ring 3 with page tables of its own and calls the kernel with `int $0x80` (ADR 0007) |
| protection | NX and W^X: `EFER.NXE` from the loader, the text read-only and executable, every data page - rodata, data, bss, stacks, heap, arena, screen - writable and NX; no page is both (ADR 0007, sections 9 and 10). The kernel is in the higher half: the image is linked at `0xFFFFFFFF80000000` and mapped there beside the low identity map |
| signature | the kernel carries a 120-byte trailer (magic, size, SHA-256 and Ed25519) and the loader verifies it before it copies a byte (ADR 0020); the public key is compiled into `loader32.elf`, the private key is outside the tree (`MICROOS_SIGN_KEY`), and `tools/microos_sign` signs and verifies |
| build | `make x86-build` (needs `cmake`, `ninja`, `xorriso` is not needed) and `make x86-disk` for the volume with `/bin` |
| check | `make x86-smoke` (serial), `make x86-vga-smoke` (the framebuffer, the text mode and the keyboard), `make x86-disk-smoke` (two boots on a disk), `make x86-net-smoke` (the network) - all need `qemu-system-x86` |
| build | `make x86-build` (needs `cmake`, `ninja`); `make x86-iso` needs `xorriso` and `grub-mkrescue` or limine, because it asks a boot loader to write the El Torito record ([iso.md](iso.md)) |
| check | `make x86-smoke` (serial), `make x86-vga-smoke` (screen and keyboard), `make x86-disk-smoke` (two boots on a disk), `make x86-net-smoke` (the network), `make x86-iso-smoke` (the CD) - all need `qemu-system-x86` |
| medium | `make x86-iso`: a BIOS ISO with the loader, the kernel and the root image, and a shell and `/bin` out of it ([iso.md](iso.md), ADR 0026) |

## Run it

```bash
make x86-build
make x86-disk         # the image of the disk: /bin and /etc of MicroFS
make x86-run          # a QEMU window: the framebuffer of 1280x800 and its keyboard
make x86-run-serial   # the same machine in a terminal (-nographic)
make x86-run-disk     # the window with a disk of its own, kept between runs
make x86-run-disk-serial  # the same disk in a terminal
make x86-net-run      # the same machine with a virtio-net card, in a terminal
make x86-smoke        # boot and check the lines of the banner and the shell
make x86-vga-smoke    # check the picture of both screens and the keyboard, without a display
make x86-disk-smoke   # two boots on one image: a file written in the first is read in the second
make x86-net-smoke    # boot with QEMU user-net and check ping, UDP, HTTP, the page of the state and DNS
make x86-iso          # the BIOS ISO: loader, kernel and root image (needs xorriso and a boot loader)
make x86-iso-smoke    # boot the ISO with -cdrom and check the shell and /bin
```

`make x86-run` opens the window of QEMU with `-display gtk`. The display
backend is a package of its own in some distributions; the target asks QEMU
for its list of backends first and prints the command to run when the one
asked for is not there.

| distribution | the package of the GTK window |
|---|---|
| Arch, Manjaro | `sudo pacman -S qemu-ui-gtk` |
| Debian, Ubuntu | `sudo apt install qemu-system-gui` |
| Fedora | `sudo dnf install qemu-ui-gtk` |

A QEMU built with SDL and without GTK takes `make x86-run X86_DISPLAY=sdl`;
one built without a display backend at all cannot open a window, and
`make x86-run-serial` is the machine for it. `X86_DISPLAY=none` runs the
image without a window and with a serial port that goes nowhere, so with no
console at all: it says that the machine boots and nothing more.

The serial port of the window run is `-serial null` and not `-serial none`:
`null` is a backend that takes the bytes and drops them, so the UART of the
machine is there and behaves; `none` removes the device, and a read of the
status register of a port nobody answers returns `0xFF`, whose bit 0 says "a
byte is ready" for ever. The console polls that register, so it would take
the answer of a missing chip for a key and type a line nobody wrote - the
window fills with "line too long" while a person does nothing. The console a
person reads is the screen, and the bytes of COM1 are the same ones; the port
still takes the keyboard and the UART as two halves of one console.

The two smoke tests check different consoles of the same machine.
`tools/x86_smoke.py` types into COM1 and reads the transcript of the serial
port; `tools/x86_vga_smoke.py` boots the machine twice with `-display none`:
on the VGA of QEMU (`-vga std`, the Bochs VBE interface of ADR 0023) it reads
the picture with the monitor (`screendump`, a PPM of 1280x800), compares the
cells with the bitmaps of the font, sets the theme and types with `sendkey`.
The frame the boot draws first is a check of its own: the dump is taken as soon
as the banner is in the transcript, and the picture must hold only the colours
of the console, so a line of the text mode or of the firmware's screen under
the new picture fails it. Every cell of the 8x16 grid is compared with the
table of the font the kernel was built with
(`ports/port_x86_64/src/font_spleen.c`): the paper, a glyph or the cursor, and
nothing else, so a second copy of the text a few pixels below the first is
caught whatever its offset. The lower case of the font is proved by the
glyphs of `g`, `p` and `y`, whose tails the 5x7 page of the board could not
draw at all, and `font.line_gap 2` is checked in the picture (a line every 18
pixels and 44 rows) and in `/etc/font`. On a card without that interface
(`-vga cirrus`) the test reads the text memory with `pmemsave` and checks the
720x400 picture and the attribute of every cell. So the screen and IRQ1 are
checked without a display server. The target is a target of its own and not a
part of `make ci`, because the monitor of QEMU is a tool a build farm does not
have to have. `python3 tools/x86_vga_smoke.py --display gtk` runs the same
checks against the real window instead of its memory.

The machine boots from three files: `build-x86/loader32.elf` (the loader),
`build-x86/microos64.bin` (the kernel, a flat image) and
`build-x86/programs.img` (the disk). The kernel is linked in the higher half
at `0xFFFFFFFF80000000`, the loader copies the bytes to 16 MiB and maps them
there, and the C of the kernel is built with `-mcmodel=kernel` (ADR 0007).

## The signature of the kernel

`microos64.bin` is the flat kernel followed by the 120-byte trailer of
ADR 0020: `MOSIGN01`, the version, the algorithm, the size of the payload,
its SHA-256 and an Ed25519 signature over those 56 bytes. The loader carries
the public key (`sign_public_key.h`, generated from the key of the build) and
calls `loader_verify_image()` before it builds the page tables, copies the
kernel or enters the long mode. A refusal prints `Micro OS: the image is
refused: <reason>` on COM1 and on the bottom row of the screen, and halts:
the damaged kernel never runs. A good image prints one line first:

    Micro OS: image signature ok (sha-256, ed25519)

The key of a build is the private file named by `MICROOS_SIGN_KEY`, or a fresh
one in `build-x86/signing.key` when the variable is empty. The second case is
the development loop and CI: the check is real, the key is not a secret, and
two builds do not produce the same bytes. A release passes the key:

    make x86-build MICROOS_SIGN_KEY=/path/outside/the/tree/release.key
    make x86-smoke

`tools/microos_sign` does the work by hand:

    microos_sign keygen --key dev.key --pub dev.pub --header build-x86/sign_public_key.h
    microos_sign sign   --key dev.key --in microos64.raw --out microos64.bin --sig microos64.sig
    microos_sign verify --pub dev.pub microos64.bin
    microos_sign info   microos64.bin

`make x86-smoke` checks both answers: the normal boot accepts the image, and a
kernel with one changed byte, a kernel without a trailer and a kernel signed
by another key are each refused by the loader before `board: qemu_x86_64`
appears. The tests call the same verifier on the host
(`tests/cases/test_sign.c`), with the vectors of RFC 8032 and FIPS 180-4.

The refusal is a setting. `MICRO_OS_REQUIRE_SIGNATURE` is a CMake option, on
by default; with it off the loader still verifies the trailer, still prints
the reason, and then prints

    Micro OS: unsigned or damaged image: not guaranteed

and loads the kernel. The build of the other policy is a directory of its own:

    make x86-open-build                  # build-x86-open, the option OFF
    make x86-smoke                       # the strict and the open build

`make x86-smoke` boots the signed kernel on the open loader (it is the same
check), then an unsigned kernel and a kernel with a changed byte: each prints
the warning and reaches `board: qemu_x86_64` and `the kernel is up`. The
strict loader refuses the same bytes, which is what the signature checks of
the smoke test already prove. The setting exists for a development machine
whose image is not signed yet; it does not make a damaged image correct, and
the sentence on the console says that.

There is no anti-rollback on this machine: it has no store that survives a
reset (the RAM volume is gone with the machine), so an older correctly signed
kernel boots and there is no flag. The version in the trailer leaves room for
a floor when the port has a store; today the rule is "a rollback is allowed"
(ADR 0020, section 7).

The heap is the low identity map and stops below the window of the programs,
which are placed at 32 MiB; the image of 1.0 ends 1.9 MiB after the base and
the port allows it up to 16 MiB. The thirty-one programs of `/bin` are built
first:
`build-x86/hello.mox`, `crash.mox`, `fork.mox`, `badptr.mox`, `signals.mox`,
`nx.mox`, `echo.mox`, `streams.mox`, `mkdir.mox`, `rm.mox`, `pwd.mox`,
`spawn.mox`, `zcat.mox`, `pcre2grep.mox`, `lua.mox`, `zstd.mox`, `bc.mox`,
`dc.mox`, `file.mox`, `sqlite3.mox`, `awk.mox` and the ten commands of sbase
(`cat.mox`, `grep.mox`, `head.mox`, `ls.mox`, `tail.mox`, `wc.mox`,
`sort.mox`, `find.mox`, `xargs.mox`, `sed.mox`) are the images of the window
(`programs/program.ld` links them for the window, `tools/mkprog.sh` turns
them into the format). They are not blobs of the kernel: `make x86-disk`
writes them into a MicroFS volume of a raw disk
image with `tools/mkfs_mfs.c`, and the boot mounts that volume and runs
`/bin/hello` through `execve`. All but the first six are linked against
`components/libc/user` and enter at `_start` (ADR 0016); `zcat` also links the
static archive of zlib 1.3.1, `lua` the core of Lua 5.4.7, `zstd` the library
of its release, `file` libmagic and the decompressors of the two formats, and
`/bin/sqlite3` the archive of SQLite with the VFS of the port
([`docs/porting-programs.md`](../../porting-programs.md)). SQLite, Lua, zstd
and `file -l` are the programs built without `-mgeneral-regs-only`: a `REAL`,
a `lua_Number` and a human-readable size are doubles, and the loader turns the
SSE and x87 units on for them. `zstdcat` is the same image as `zstd` under a
second name.
The first six use the raw `programs/prog.h`. The build passes
`--base 0x02000000` to `tools/link_program.sh`, which links the program, checks
that the text really landed at `0x02000020` - the ld of binutils 2.38 moves it
32 bytes up, the 2.47 of this host does not - and hands the linked file to
`mkprog.sh`; the image of another window is refused, and nothing relocates the
program, so a wrong address would run and read its data from somewhere else.

The virtual and the physical address are no longer the same number for a
program: `src/paging.c` builds its page tables with the window at 32 MiB
mapped onto the arena at 64 MiB, and the kernel reaches the bytes through the
identity map while the program reaches them through its own. `src/gdt.c`
builds the GDT the loader never had (user code and data, a 64-bit TSS with
`RSP0` and the double-fault stack), and `src/interrupts.S` takes the stack of
the TSS on a call from ring 3.

The kernel has two names too. The image is linked at the top and the loader
also leaves its bytes in the low identity map, which is why `page_phys()`
subtracts the base for a page-table entry and `table_entry()` can walk a table
by the physical address an entry holds. `kernel_image_map()` gives the image
chunks one table of 4 KiB pages and names it from both PD entries, so the text
is read-only and the data NX whichever name the processor came through; the
space of a program inherits the high half as it is, supervisor-only, because
the kernel runs on that CR3 during a call.

## The screen: a framebuffer of 1280x800

The machine had one screen until 1.2: the text mode of the VGA. ADR 0023 gives
it the screen of a machine with a picture, and the drawing code of the board
talks to it unchanged - the console, the font and the boxes draw through
`kernel/render.h`, and a framebuffer is one more renderer.

The card is the VGA of QEMU, `1234:1111`, found over the configuration space
of the PCI bus that the network driver already read (`src/pci.c` is the one
copy of the mechanism now). `src/vbe.c` programs the Bochs VBE registers on
the I/O ports `0x1CE`/`0x1CF`: the identifier `0xB0C0`-`0xB0C5`, 1280x800, 16
bits per pixel, then the enable with the linear framebuffer. The mode is read
back before it is accepted, and the window is a memory BAR of the card whose
size the device reports to be at least the 2,048,000 bytes of the picture
(`port_x86_64_pci_bar_size()`, the all-ones probe) - the first BAR under QEMU,
16 MB at `0xFD000000`.

That address is above the identity map of the first gigabyte, so
`port_x86_64_paging_map_device()` maps the window at
`PORT_X86_64_SCREEN_VIRT` (`0xFFFFFFFFA0000000`, half a gigabyte into the top
2 GiB) in 2 MiB pages, writable and NX. The tables of the higher half are
shared by every address space, so the `write` of a program in ring 3 draws on
the same picture as the boot log, and the audit of W^X counts a device page as
data.

The mode is turned on without `VBE_NOCLEARMEM`: the card clears the memory it
shows as part of the switch, and it has to - the cells of the text mode live
in the same memory (the offset `0x18000` of the card is the `0xB8000` window),
so a picture that kept it would have the screen of the firmware inside it. The
console empties that window before the switch as well, and after the first
lines of the boot it draws the whole picture again from the cells it holds:
the log is redrawn, not lost, and every pixel of the first frame goes through
the display once more. A display that keeps the frame it painted before the
mode switch therefore has nothing of the 80x25 screen left under the new
picture.

`src/fb.c` registers the renderer (`bochs-vbe`) and holds the grid of the
console: 160 columns by 50 rows of cells of 8x16 pixels, the font of the PC.
The font is Spleen 8x16 2.3.0 (BSD-2-Clause): the table is
`src/font_spleen.c`, generated by `tools/spleen_font.py` from the BDF in
`third_party/spleen/`, and the licence travels with the bundle
(`tools/bundle.sh`, `docs/licensing.md`). The 5x7 page of `components/font` is
the panel of the board and stays there - a cell of 10x14 pixels and letters
with no lower case are the wrong picture for a screen of a PC. A character is
a glyph blitted into the memory, a scroll is a move of the rows of the picture
by the step of a line, the cursor is the reverse of a cell, and a flush does
not exist: the memory of the card is the glass. `src/console.c` is the console
itself - the position, the tab stops, the erase of the backspace, `\n`, `\r`,
`\t`, `\b`, `ESC[2J`, `ESC[H` and the em dash of the banner - and it writes
cells to whichever screen it was given: `src/vga.c` for the text mode,
`src/fb.c` for the framebuffer.

The step of a line is the cell plus `line_gap`, a key of `/etc/font` (ADR
0021): 0, 2 or 4 pixels give 16, 18 or 20 pixels a line - 50, 44 or 40 rows of
a picture of 800 - and the grid is laid out again from the cells the console
holds, so a command that changes the key never loses a line of the log:

```text
microos> config set font.line_gap 2
font: line_gap 2, the cell is 8x16 at 18 px, 44 rows of 160 columns
font.line_gap=2
```

`font.scale`, `font.columns` and `font.rows` are the cap of the panel of the
board: this machine keeps them and says once that they are not its business.
A value of `line_gap` that is not one of the three is 0 and a warning.

The pair of `console` of /etc (ADR 0021) is the theme and its inversion, and
the screen repaints the cells it holds, so a line that a person typed is read
again in the new colours:

```text
microos> config set console.theme light
console: theme light, invert 0, the screen the framebuffer of 1280x800, 16 bpp, 160x50 cells
```

Dark is grey `0xAD55` on black, light is black on `0xAD55`. `/etc/display` is
the panel of the board: this machine has none, so the boot keeps the file and
says so.

### The text mode stays the fallback

A machine whose card has no Bochs VBE interface - `-vga cirrus`, a card with a
VGA of its own - keeps the text mode the firmware left behind: 80 columns by
25 rows of two bytes at `0xB8000`, the character and its attribute. The port
programs no mode and no palette there. The screen obeys the same control
characters and the same two sequences of `clear`, and puts the position of the
cursor into the registers 14 and 15 of the CRTC (`0x3D4`/`0x3D5`), so the
cursor of the hardware is where the next character lands. The attribute of
every cell is `/etc/console` (`0x07` grey on black, `0x70` black on light
grey), and the applier logs the number it put there (`console: the attribute
of the screen 112`).

The boot names the screen it chose, so a machine whose picture did not come up
says why:

```text
x86-64: the screen is the framebuffer of 1280x800, 16 bpp, 160x50 cells
x86-64: the screen is the text mode of the VGA, 80x25
```

The 80x50 mode of the fallback with the 8x8 font of the VGA of ADR 0023 is not
built: it is a font load through the sequencer and the graphics registers on
the only console of a machine without a picture, and it is a debt of its own
([debts.md](../../debts.md), K11).

### The keyboard

The keyboard hangs on the first line of the 8259 (IRQ1, the vector `0x21`).
The stub in `interrupts.S` saves the registers around the call of C, the
handler reads the byte of the data port `0x60`, translates it with the
scancode set 1 (the 8042 of the machine translates the set 2 of the device
into the set 1 of the PC), puts the character in a ring of 64 and sends the
EOI. A task takes the characters out of the ring: `k_port_console_poll()` is
non-blocking, `k_port_console_getchar()` waits in `hlt` when there is
nothing, and both ask the keyboard before they ask COM1, so the two consoles
are alive at once and neither has to be chosen at the boot.

The keys are the US layout: the letters, the digits and the punctuation a
command line needs. Shift changes the characters and Caps Lock capitalises
the letters, which is the whole of what the lock does on a PC. Enter is
`\r`, Backspace is `\b` and Ctrl+C is the character 3 - the three editing
commands the shell of the kernel reads (`src/shell.c`), and the reason a
typo can be erased in the window at all. Tab is not a key of this console:
the shell splits a command line on spaces, and a tab would arrive as part of
a word.

What the keyboard does not do:

* the scancode set 1 and the US layout only: a keyboard of another layout
  sends the same numbers and gets the letters of this table;
* the E0 sequences carry the keys of the second form and not the characters
  of a line: the arrows, Home, End, PageUp, PageDown, Insert and Delete are
  events of `/dev/input0` (`key.up`, `key.delete`, ...) and the right Ctrl is
  the modifier of the same form, while the number pad of the same block types
  nothing of the console - the map of `kernel/input_set1.h` is the table.
  The `E1` sequence (Pause) is still dropped whole;
* no repeat of a key in the driver: the device repeats it, and the ring is
  the only buffer between the two.

What the screen does not do: the console is a terminal and not a desktop.
There is a framebuffer a program can draw in (`/dev/fb0`, ADR 0028) and the
programs of the interface are files of `/bin` (`ui`, `edit`, `fm`; ADR 0030),
but the machine has no window server, no mouse and no compositor, and a 3D
driver of a real card is not promised (ADR 0023, sections 4 and 5). The
picture has one
pair of colours for every cell - `console.theme` swaps the pair of the whole
screen - and no attribute per cell: a command that wants a red word or an
inverted line has nothing to ask for yet (`docs/debts.md`, K11).

## The disk

The disk is the machine's, not a test's: `make x86-disk` writes
`build-x86/programs.img`, an 8 MB raw image with an MBR, one Linux partition
at LBA 2048 and a MicroFS volume in it. The volume already carries `/bin`
(the thirty-one programs), `/etc/setting`, the fixtures `/etc/greeting.gz`,
`/etc/setting.zst` and `/etc/broken.zst`, the scripts of the interpreters and
of the database (`/etc/hello.lua`, `/etc/hex.lua`, `/etc/bc.bc`,
`/etc/dc.dc`, `/etc/sql-*.sql`, `/etc/columns` and `/etc/hello.awk`), the database of libmagic (`/etc/magic.mgc`
and its text) and the licenses of zlib, PCRE2, Lua, zstd, sbase, bc, file, SQLite
and awk in `/etc/licenses`: the host tool `tools/mkfs_mfs.c`
(formats and writes) links `components/mfs`, so the layout of the volume has
one implementation and the host test `tests/cases/test_mkfs_image.c` mounts
what the machine boots.

`make x86-run` and `make x86-run-serial` boot on that image: the machine finds
the drive (`ata0`), reads the MBR, registers the partition (`ata0p1`), mounts
it at `/` and runs `/bin/hello` before the shell. A boot writes nothing to the
volume; the programs are files the tool left there. The boot also mounts
`/proc` (procfs) and `/dev` (devfs), so `tree /` draws the same `bin/`,
`etc/`, `proc/`, `dev/` tree the board and the simulator have. A volume
without `/etc` (the memory volume of a machine with no disk) gets `/etc`,
`/etc/init.d` and `/etc/setting` from the boot; a volume that has them is
never changed.

`make x86-run-disk` copies the image once to `build-x86/disk.img` and keeps
that copy: a person boots the machine, writes a file, boots it again and finds
the file there. `make x86-disk-smoke` is the proof and the regression test:
one image, two boots, `/bin` and a program from the disk in both,
`write /persist.txt` in the first and `cat /persist.txt` in the second.

The driver is `src/ata.c`: the ATA registers at 0x1F0, LBA28, PIO, polled -
no DMA (the port has no contract for a physical address yet) and no interrupt
of the channel. A command runs with the interrupts off, so the timer and the
keyboard of this machine cannot interleave another task's request with the
transfer; every wait has a budget of five seconds, a failed command is
repeated once and reported with the sector that failed. A disk that does not
answer is reported and the machine goes on with `ram0`; that volume has no
`/bin`, so the boot says which file it could not find and starts the shell.
`programs.img` and `disk.img` are generated and not committed; `make clean`
does not remove `build-x86`, `rm -rf build-x86` does.

## The tar of a release

`make x86-user` builds the complete machine and the release bundle, unpacks
the x86-64 tar and starts its `run.sh`; the tar carries the loader, the
kernel, `x86-all.img` (the disk of MicroFS with `/bin` and `/etc`), the
licenses and a `run.sh` with the disk and the virtio-net card. The tar is
the artifact of a release, not a build directory: what the target starts is
what a person downloads. `make x86-user-serial` is the same with `-nographic`.
## The CD

`make x86-iso` writes `build-x86-iso/microos.iso`: the loader, the signed
kernel and the root image in one BIOS ISO, which a machine without `-kernel`
boots from its CD drive. The root image is the second module of the boot and
the port mounts its first partition at `/` (`boot0p1`, read-only); the shell,
`/bin` and the programs come from the CD. `make x86-iso-smoke` boots the ISO
with `-cdrom` and checks the transcript. The page of the medium, with the
build steps, the requirements and the gaps (UEFI, a hybrid USB image, a
writable root, a reproducible ISO), is [iso.md](iso.md); the decision is
ADR 0026.

## The network

The machine gets a card from QEMU and a stack from the port:

```bash
make x86-net-run     # the card, and 8080 of the loopback forwarded to 8080 of the guest
make x86-net-smoke   # the same machine, driven by tools/x86_net_smoke.py
```

`make x86-net-run` forwards the port 8080 of the loopback to the same port of
the guest, so a person can try the page of the state the way the smoke test
does: `httpd 8080` on the console, then `curl http://127.0.0.1:8080/` on the
host. The smoke test makes its own forward, on a port it found free, so a run
of the test and a run of the machine do not fight over 8080.

The driver is `src/virtio_net.c` and the protocols are `src/net.c`. The card
is virtio-net and not e1000, and the reason is the address map of this port:
the kernel is the identity map of the first gigabyte, and the MMIO BAR of a
PCI device in QEMU sits near 4 GiB, outside that map. The legacy virtio
interface is programmed through port I/O, so the rings are the only memory the
device sees, and the page tables of the machine do not have to grow a mapping
of MMIO before the first frame. The e1000 keeps its rings behind the BAR and
would need that mapping first (ADR 0007 territory), and the virtqueue is a
documented format while half of the e1000 is a register file learned from
another driver.

The device is polled: the timer and the keyboard are the only lines the PIC
unmasks, and a PCI line needs the I/O APIC (K8). The receive ring is asked for
a frame, and the transmit path waits for the used entry of its one descriptor.
The driver reads the MAC from the config space (`52:54:00:12:34:56` under
QEMU), resets the device, negotiates `VIRTIO_NET_F_MAC` only, and sets the two
queues up; the receive queue holds the size the device reports (256 under
QEMU) of 2 KiB buffers, and the header of a legacy virtio-net packet (10
bytes) is skipped.

The stack is the smallest one that makes `kernel/port/net.h` true: ARP with a
cache of eight entries, IPv4 with checksums, ICMP echo (an answer to a ping of
the guest, and the request of `ping`), UDP with one exchange at a time, a
client of TCP with a 4 KiB receive window, one segment in flight and a fixed
400 ms retransmission, and the server half of the contract: a table of two
doors, a SYN that is answered with SYN/ACK from a number of its own, an ACK
that completes the handshake, and a session that `accept` hands to the caller.
A segment for a port nobody listens on is answered with a reset, so a client
of another machine fails at once instead of waiting for its own timeout.

A name that is not a dotted quad goes to the resolver of QEMU user-net
(10.0.2.3) as one query with a timeout of three seconds, and the answer is
read by the portable resolver of `components/net` (`src/dns.c`, public as
`net/dns.h`) and kept for its time to live - at most a day - in a cache of
eight names. The names are compared without case, the second command for a
host costs no packet, and `netinfo` counts the queries and the hits. The
parser of the port is the one the host tests exercise byte by byte, so a
record of type A, a CNAME on the way and a truncated packet are the same
answers on the machine and in `make test`.

The tables of the sessions and of the doors are behind one mutex of the
kernel, because the shell and the task of the page of the state are two tasks
on one card; the mutex is held for a look and a change and never across a
wait. A frame is still dispatched by its four addresses, and a task that
polls while the other waits is a task that serves both.

The configuration is QEMU user-net and there is no DHCP: the guest is
10.0.2.15/24, the host answers as 10.0.2.2 for everything outside the subnet
and as the resolver, so a server of the host on the loopback is
`10.0.2.2:<port>` for the guest. The numbers are constants of `src/net.c`.
A name outside the guest network resolves through 10.0.2.3 and the client
reaches it: on a host with a network, `http get http://example.com/` answers
`HTTP 200` and the first bytes of the page (the buffer of the command is 256
bytes, so the rest is cut).

| command | what it does |
|---|---|
| `netinfo` | the device, the MAC, the link, the address, the counters of the driver and of the stack, and the resolver's queries and hits |
| `ping <address>` | one ICMP echo and the round trip |
| `netudp <address> <port> <text>` | one UDP request and one answer, printed |
| `dns [name]` | resolve a name (and say whether the cache answered) or list the names in the cache |
| `http get <url>` | the HTTP client of `components/net` (with `-DMICRO_OS_X86_NET=ON`) |
| `ntp [server]` | the clock from an NTP server (with `-DMICRO_OS_X86_NET=ON`) |
| `httpd [port\|off]` | the page of the state on the sockets of the port (with `-DMICRO_OS_X86_NET=ON`) |

`make x86-net-build` is a second image (`build-x86-net`) with the components of
the network linked in; the default image of `make x86-smoke` keeps them out, so
its command list and its transcript are the ones they always were. The driver
and the stack are in every image and cost a few kilobytes; a machine QEMU was
given no card for finds nothing on the PCI bus and keeps the "no" answers of
the contract.

`make x86-net-smoke` starts an echo server of UDP and an HTTP server on the
loopback of the host, boots the machine with the card, and checks the protocols
in one transcript: the device and its MAC, `ping 10.0.2.2` (ARP and ICMP), a
UDP exchange with the echo server, and `http get` - the handshake, the sequence
numbers, the request, the answer and the body. It then opens the page of the
state in the guest (`httpd 8080`), reaches it from the host through a port
QEMU forwards to the guest (`hostfwd=tcp:127.0.0.1:<port>-:8080`) and reads
`/` and `/health` - the listener, the SYN/ACK, the session of `accept`, the
answer and the close, with a machine that is not the one that asked. The last
part is the resolver: `dns example.com` twice, the first answer from the
network and the second from the cache, and `netinfo` says one query and one
hit. A host with no resolver of its own skips the DNS checks and the rest of
the run is unchanged. The test is not part of `make ci`: the user-mode network
and a port on the loopback are tools of a person.

What the network does not do: no DHCP, no IPv6, no reassembly of fragments (a
fragmented packet is counted and dropped, which is what `netinfo` reports), one
segment in flight and no congestion window. A duplicate segment is answered
with an acknowledgement and retransmitted data is not reassembled; the fixed
retransmission is the whole of the reliability. The driver polls, because the
PCI line needs the I/O APIC that K8 brings.

## The processors and the second core

The port reads the processor list of the machine the way a PC has answered
since ACPI: the root pointer in the extended BIOS area (or the last kilobyte
of the BIOS), the RSDT or the XSDT under it, and the MADT with one entry per
processor; the local APIC address comes from the same table. The boot prints
both numbers, and they are the truth:

```
x86-64: 2 cpu(s) reported by the firmware, 2 in use (apic 0xfee00000)
cpu: 2 core(s) of the machine are used by the kernel
```

The left number is what the firmware calls enabled; the right one is what the
kernel started and can take a task. A machine of `-smp 2` with a kernel built
without `KERNEL_SMP` says `1 in use` and names the flag; one with the flag
says `2 in use`, and a core the firmware lists but the port could not start
is named and not counted.

### How a core is started

The list of the MADT is the plan. For every processor but the boot's, the
port allocates a 64 KiB stack, writes four words at the physical page
`0x8000` (the kernel CR3, the top of that stack, the virtual address of
`port_x86_64_ap_entry()` and the kernel number of the core), and sends the
sequence of the PC through the local APIC: INIT, ten milliseconds, the
start-up IPI with vector 7, two hundred microseconds, the start-up IPI again.
Vector 7 names the page `0x7000`, where the loader copied the trampoline of
`ap_trampoline.S`: sixteen-bit code that opens A20, loads a temporary GDT,
turns on PAE and long mode with the kernel's page tables and calls the C
entry on the stack from the parameter page.

The C of the core (`src/smp.c`) sets its identity first, then builds its GDT,
TSS and IST (`src/gdt.c`), loads the table of the interrupts the boot built,
sets `CR0.WP` for itself, and reports that it reached C. It then waits: the
scheduler of the kernel does not exist while the port is still being brought
up, and its array has room for the cores the boot accepted only. `kmain()`
releases the cores after `kernel_boot()`, and each one enters its idle loop.
A core that does not answer in 100 ms is not accepted and never enters the
scheduler.

The identity of a core is one load. Every core points `IA32_GS_BASE` at a
record whose first word is its kernel number, and `k_port_cpu_id()` is
`movl %gs:0`; the boot's number is 0 and the others follow the MADT. That is
what makes the current context, the TSS `RSP0`, the thread pointer and the
interrupt nesting of the port per core, and it is why the application
processor writes its identity twice on the way up: a load of a segment
register in long mode clears the base of `GS`, and the second write follows
the GDT the core loaded.

### The clock of a core

The millisecond of the kernel is one number and the core 0 advances it, from
the PIT and the 8259, as in 1.1. An application processor has no 8259: its
clock is its own local APIC timer, calibrated against the TSC at 1 kHz (QEMU
counts at about 62.6 MHz with divider 16, so an initial count of about
62 600). The external mode of `LINT0` belongs to the core 0 alone; with every
core in that mode, QEMU delivered the timer of the 8259 to either of them
(1889 ticks on the core 0 and 4933 on the core 1 of 6822) and the clock of
the kernel ran at a third of real time.

Since 1.6 the two lines of the port are entries of the I/O APIC of the
firmware (ADR 0034): `src/ioapic.c` reads the address of the chip and the ISA
overrides of the MADT, programs one redirection entry per line (the vector,
the destination core, the polarity, the trigger and the mask), masks the 8259
as a whole and sends the EOI of a line to the local APIC of the core that took
it. A machine whose firmware describes no chip keeps the 8259 exactly as
1.5.0 had it, and a chip whose window answers nothing is not trusted with
either line.

### What the shell shows

In a `KERNEL_SMP` build the shell has `smp`, `smp workers <n>`, `smp ping`,
`smp irq [keyboard <cpu>]` and `smp wake` (they are not registered in a
one-core build, which is the kernel of 1.1):

```
microos> smp
smp: 2 core(s) in use of 2 reported, apic 0xfee00000
smp: cpu 0 apic 0x0 ticks 134 current shell
smp: cpu 1 apic 0x1 ticks 3414 current idle
smp: queues consistent
smp: the command ran on cpu 0
smp: wake before the tick: cpu 0 0 cpu 1 2 (started by the ipi of the other core)
smp: started at the tick: cpu 0 1 cpu 1 0 (started by the timer of the core)
smp: the kernel handed 3 task(s) to another core
smp: the last hand-over went to cpu 1 at tick 812, the core started it at tick 812
smp: cpu 0 was woken 1 time(s), 1 ipi(s) taken, clock running
smp: cpu 1 was woken 2 time(s), 2 ipi(s) taken, clock running
microos> smp workers 8
smp: worker 5 on cpu 0
smp: worker 8 on cpu 1
...
smp: 8 worker(s) done
microos> smp ping
smp: ping cpu 1: ipi count 0 -> 1
smp: ping ok
microos> smp irq
smp: the I/O APIC at 0xfec00000, 24 line(s), the 8259 is masked
smp: line timer: isa irq 0 -> gsi 2, vector 0x20, cpu 0
smp: line keyboard: isa irq 1 -> gsi 1, vector 0x21, cpu 0
smp: irq 0x20 (timer): cpu 0 2299 cpu 1 0
smp: irq 0x21 (keyboard): cpu 0 0 cpu 1 0
microos> smp wake
smp: wake: 4 task(s) of 4 started, 2 of them on cpu 1
smp: wake: the kernel handed 2 -> 4 task(s) to another core
smp: wake: cpu 1 started 1 -> 3 task(s) before its tick
smp: wake: the ticks of cpu 1 went 135 -> 135 with its clock stopped
smp: wake ok: the tasks of cpu 1 started by the ipi, before its tick
```

`smp workers` is the proof that a task of this machine runs on each core: the
scheduler puts a new task on the next core of its round robin. `smp ping` is
the proof of the IPI vector: the count is read on the core that received the
interrupt, not on the one that sent it. `smp irq` prints the table of the I/O
APIC and the count of the two lines per core; `smp irq keyboard 1` moves the
keyboard to the second core. `smp wake` stops the clock of the other core, so
that the only way a task can begin there is the interrupt of this one, and
starts it again before it returns (ADR 0034).

### The gate

```bash
make x86-smp-build    # the kernel with KERNEL_SMP, an image of its own
make x86-smp-run      # the two-core machine in a terminal
make x86-smp-smoke    # boot -smp 2 and check 52 things about the two cores
make soak-smp         # the 600-second session on the two-core image
make x86-smoke        # the one-core kernel: 323 checks, unchanged
```

`tools/x86_smp_smoke.py` boots the image of `build-x86-smp` on the disk of
`make x86-disk` with `-smp 2`, reads the two numbers, runs the program of the
boot in ring 3 and drives the `smp` command: the two cores are in use, the
queues are consistent, a worker ran on each core, the IPI arrived on the core
it named, the ticks of both cores moved between the two reports, the table of
the I/O APIC names a core for the timer and one for the keyboard, the tasks of
`smp wake` start on the core whose clock is stopped, and the keys the monitor
of QEMU presses arrive on the core the keyboard line was sent to. A line
that names a missing core, an exception or a stopped machine fails the run;
so does a transcript that stops answering.

The 600-second session (`tools/soak.py --smp`) runs the load of the campaign
on the same image and samples `smp` every cycle: the ticks of the core 1 have
to move as well as those of the core 0, the queues have to stay consistent
and no answer may hold an exception. The short form of the tool is
`make soak-smp SOAK_MINUTES=1`.

The one-core build is the kernel of 1.1: `KERNEL_SMP` is off by default, and
`make x86-smoke` and every other check of the port run it.

### What remains

The guard page under the stack of a task is still open: an overflow writes
into the heap and the port does not see it. A machine with several I/O APICs
keeps the first, and a level-triggered line whose device needs its entry
masked while it is served is not programmed - the two lines of the port are
edge triggered, and the disk and the network are polled. These are rows of K8
in [debts.md](../../debts.md).

## What works## What works

* the boot: the loader, long mode, the banner, the contract check at 1.9;
* the signature of the kernel (ADR 0020): the loader verifies the trailer of
  SHA-256 and Ed25519 with the key compiled into it before it copies a byte,
  and a damaged, unsigned or foreign-key image stops with the reason and
  never reaches the kernel;
* the console: a framebuffer of 1280x800 in RGB565 through the Bochs VBE
  interface of the VGA of QEMU (ADR 0023), 160 columns by 50 rows of the 8x16
  font of the PC (Spleen 8x16 2.3.0, BSD-2-Clause, with the licence in the
  bundle), drawn through `kernel/render.h`; the step of a line is the cell
  plus `font.line_gap` (0, 2 or 4 pixels: 50, 44 or 40 rows) and the grid is
  laid out again when the key changes; the text mode of 80x25 when the
  machine has no such interface; COM1 from the same call, the keyboard of the
  PS/2 with Shift, Caps Lock, Backspace and Ctrl+C, and the serial port with
  the console it always had; the colours of the cells are `/etc/console`
  (`theme`, `invert`) at the boot and at `config set` (ADR 0021);
* the clock, the interrupts (IDT with 32 exceptions, PIC, PIT), the context
  switch, the scheduler, `ps`, `uptime`, `free`, `dmesg`;
* paging and ring 3 (ADR 0007): the kernel is a supervisor-only identity map
  of the first gigabyte, the window of the programs is 4 KiB user pages with
  the null page unmapped and a guard page under the 64 KiB stack, and the
  program prints the `cpl` it reads from CS (`3`); a program is a task, `exit`
  ends it, the caller of `execve` waits for it, and `execve` answers 0;
* NX and W^X (ADR 0007, sections 9 and 10): the loader turns `EFER.NXE` on
  with long mode, and every entry of every table says what the page may do.
  The text of the kernel is readable and executable and not writable; its
  rodata is read-only and NX; its data, bss, the stacks, the page tables and
  the heap are writable and NX, and so is the rest of the first gigabyte -
  the VGA, the boot information and the arena of the programs - and the
  window of the framebuffer, which the port maps in the top 2 GiB (ADR 0023).
  A program has
  the same two regions of its own: the header and the code are read-only
  code, and the data, the bss, `argv`/`envp`, the 64 KiB stack and the
  restorer page carry the protection they need (the restorer is code, so it
  is executable and not writable). `programs/program.ld` pads the text to a
  page and `k_port_exec_enter()` refuses an image whose text does not end on
  one. The port reads the tables back: `kmain` prints
  `x86-64: the map is W^X` and a program whose window is not W^X is refused
  before it runs. `/bin/nx` jumps into its data (fault 14 with error `0x15`,
  the instruction-fetch bit), writes its text (fault 14 with error `0x7`) and
  hands its text to `read` (the kernel's store into a read-only page makes a
  private copy of it as data, so the parent's code is untouched and the child
  dies on its next fetch), and `make x86-smoke` reads the lines and the
  parent that survives them;
* the filesystem: MicroFS mounted at `/`, on the first partition of the disk
  when one is attached and on a volume of 512 KB in the memory of the port
  when there is not; `ls`, `cat`, `write`, `mkdir`, `rm`, `df`, `fsck` and
  `mount` work on it and `/bin` exists, the modes and the owners are stored
  (`0644` for a new file, `0755` for a directory), and the boot gives
  `/bin/hello` its `0755` through the VFS;
* a disk: a raw image on the primary IDE channel, read and written by PIO, its
  MBR read by the portable parser, MicroFS on the first partition (`ata0p1`)
  and the volume formatted at the first boot; what is written to it survives
  a reset, which `make x86-disk-smoke` checks (ADR 0008);
* a medium: `make x86-iso` writes a BIOS ISO with the loader, the kernel and
  an 8 MB root image as the second module; the machine boots it with
  `-cdrom` and no other flag, mounts the image's first partition at `/`
  (`boot0p1`, read-only) and runs `/bin` from it, which `make x86-iso-smoke`
  checks (ADR 0026, [iso.md](iso.md));
* a network: a virtio-net card on the PCI bus, its MAC and link read from the
  config space, ARP, IPv4, ICMP, UDP, a client of TCP and a server of TCP under
  `kernel/port/net.h`; `netinfo`, `ping`, `netudp` and `dns` from the port,
  and `http get`, `httpd` and `ntp` from `components/net` with
  `-DMICRO_OS_X86_NET=ON`. `make x86-net-smoke` checks the echo of the gateway,
  a UDP exchange with a server of the host, an HTTP GET over the user-mode
  network of QEMU, the page of the state served by the guest to a client of the
  host through `hostfwd`, and a name that is asked once and answered twice;
* a second filesystem: `-DMICRO_OS_FS_FAT32=ON` links the FAT reader and writer
  reader and its `vfs_mount_type()` (ADR 0009). The ext2 reader is the same
  choice under `-DMICRO_OS_FS_EXT2=ON`: a read-only volume mounted by its
  type, with modes, owners and `statx` (ADR 0019). The options are off by
  default, so the smoke tests of the machine keep one MicroFS volume; a
  machine that has a FAT or an ext2 partition mounts it with a call of the
  boot, because `/etc/fstab` and the `mount` command do not speak the
  registry yet;
* a program out of a file: `make x86-disk` writes the images of `hello`,
  `crash`, `fork`, `badptr`, `signals`, `nx`, `cat`, `echo`, `ls`, `spawn`, `wc`,
  `head`, `grep`, `mkdir`, `rm` and `pwd` into `/bin` of the MicroFS volume
  (every image is built with `--arch x86-64`, and `execve` answers `ENOEXEC`
  for one of another machine before it enters a byte), gives them the mode
  0755, and the boot asks for `/bin/hello` with `execve` -
  the stat, the check of the mode, the read through the VFS, the parse of the
  image and the entry are the ones of the call and not a copy of them. The
  program is entered with `argc` in `rdi`, `argv` in `rsi` and `envp` in
  `rdx` (ADR 0016), prints its lines on the console and calls `exit`; the
  call reports 0 and the port prints `the program exited with 0`. The shell
  runs the same files with `exec`: `cat`, `echo`, `ls`, `spawn` and `wc` are
  ordinary C programs of `components/libc/user`, `signals` exercises the
  dispositions of ADR 0015, and `make x86-smoke` reads their lines back;
  `spawn` starts another program with an environment of its own, and the
  programs inherit the descriptors of the stage that started them, so
  `echo one | /bin/cat` and `ls /bin | wc -l` work (ADR 0017);
* a fault of a program is not a fault of the machine: `/bin/crash` writes to
  address zero in ring 3, the port prints `the program faulted: vector 14 at
  0x0000000000000000`, takes the program down and the shell answers the next
  command;
* the edge of the system call: `k_port_exec_contains()` (a call of the contract since 1.7) walks
  the page tables of the program, `components/kernel/src/ksys.c` copies the
  paths, the vector of `execve` and the buffers of `stat`, `getcwd`, `read`,
  `write` and `readdir` through the check, and a range that is not mapped is
  `EFAULT`. `/bin/badptr` hands `write`, `stat` and `read` an address outside
  the window, the last bytes of the window with a request that crosses its
  end and the guard page; every call answers 14 and the machine writes the
  next line with a valid pointer;
* the signals of K7: `/bin/signals` installs a handler of the program (the
  kernel builds a frame on the stack and the restorer page calls
  `sigreturn`), a child ends with `SIGTERM` and the status 143, a `waitpid`
  is cut by `SIGINT` with `EINTR`, `SIG_IGN` leaves the task alive, the tick
  of the timer delivers a handler to a program that never calls, and `kill`
  of a pid that is not there is `ESRCH`. The shell has `kill` as well, and
  Ctrl+C of the console reaches the program the shell waits for (ADR 0015);
* the shell on the console, which is the window and COM1 at once, with
  `help`, `uname`, `uptime`, `ps`, `free`, `memtest` and the rest of the
  portable commands, plus the commands of the files:
  `chmod`, `chown` and `exec` reach the same calls as the boot, and the
  thread pointer of the library follows the task, so their `errno` works;
* an `errno` per task: each task gets a copy of the TLS block and
  `IA32_FS_BASE` is written on every context switch and at the door of the
  kernel, because a program of ring 3 runs with a zero thread pointer.

## What does not work yet

* the guard page under the stack of a task: an overflow writes into the heap
  and the port does not see it; a machine with several I/O APICs keeps the
  first, and a level-triggered line whose device needs its entry masked while
  it is served is not programmed (ADR 0034, K8 of debts.md);
* the disk is one, and it is the primary ATA channel: no SATA, no virtio, no
  NVMe, no card, no second disk;
* the CD of K13 is BIOS only and its root is the image of the medium: every
  write is `EIO`, there is no place for a file of a person, an EFI machine
  does not start it and a USB stick written with it does not boot
  ([iso.md](iso.md), "What does not work yet", ADR 0026);
* the driver polls and copies through the processor: no interrupt, no DMA and
  no queue, so a large copy is slow; there is no cache of the kernel, and the
  writes reach the cache of the drive until the filesystem flushes it (at
  every close, every new directory and the unmount);
* the tables understood are the MBR of four primaries and the GPT; extended
  and logical partitions wait, and a medium of 4 KB sectors does not;
* without a disk the volume is still the memory of the port, and it does not
  survive a reset - the boot then makes the filesystem again;
* `fork()` and copy on write (K4, contract 1.5): a program is a task of the
  scheduler with its own CR3 and kernel stack, `execve` makes it and waits for
  it, and `fork()` copies the page tables, points both copies at the same
  frames and clears the write bit. The first write of either side faults,
  copies the 4 KiB page and takes the write bit back; the child returns from
  the call with 0, the parent with the pid, and `waitpid` sees the status.
  `/bin/fork` is the program of the smoke test and prints the 42 of the
  parent next to the 99 of the child;
* the kernel is in the higher half: the image is linked at
  `0xFFFFFFFF80000000`, the loader copies it to 16 MiB and maps it there, and
  the C of the kernel is built with `-mcmodel=kernel`. The low identity map of
  the first gigabyte stays for the heap, the arena and the devices, so the
  window of the programs is part of a space whose kernel is out of the reach
  of ring 3 at the top. ADR 0007, "The higher half: taken", has the addresses
  and what the change still does not do;
* there is no SMAP and no SMEP: the edge checks the page tables of the program
  and then reads the same pages in ring 0. Both features are a decision of
  ADR 0007 section 10 and not a part of 1.0: QEMU's default `qemu64` does not
  advertise them, and SMAP needs `STAC`/`CLAC` in the portable copy edge
  (`components/kernel/src/ksys.c`), which is a change of the ABI and not of
  this port. NX is on;
* the network is small on purpose: no DHCP, no IPv6, no reassembly of
  fragments, no congestion window, one segment in flight, and the driver polls
  because the PCI line needs the I/O APIC that K8 brings. The server is one
  session at a time per door (two doors, four sessions), which is what a page
  of the state and a shell of telnet need;
* the screen has one mode: 1280x800 and 16 bits per pixel, asked for at the
  boot and not a setting, and the fallback of a card without the interface is
  the text mode of 80x25 - the 80x50 of ADR 0023 is not built. There is no
  surface for a program, no mouse and no compositor: the picture is a console
  (ADR 0023, and [debts.md](../../debts.md), K11).

## Why it exists

The kernel is portable only if something other than the board runs it. This
port caught three things the board never would: a build that needed
`-U_FORTIFY_SOURCE` on a distribution that fortifies by default; the execution
of the kernel on a machine with an MMU, which now runs a program in ring 3
with an address space of its own; and a program
that read its data one byte away from where it was written, because
`tools/mkprog.sh` padded its segments at the end of the file instead of before
the data. A machine that really jumps into the image is the only way to notice
that the header was right and the bytes were not.
