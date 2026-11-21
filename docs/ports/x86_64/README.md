# The x86-64 port: bare metal in QEMU

A port of the kernel for a machine with no operating system under it: no
libc, no POSIX, no firmware. It is the proof that the kernel is portable, and
the machine where a program can really be loaded and run (the window of
`execve` is here).

| | |
|---|---|
| port | `ports/port_x86_64` |
| loader | `loader32.S`: a multiboot image that QEMU loads with `-kernel`; it copies the kernel to 16 MiB, builds the identity map of the first gigabyte and turns the long mode on |
| console | the text screen of the VGA (80x25, `0xB8000`, light grey on black) and COM1, 115200, `\r\n`, out of one `k_port_write()` |
| input | the PS/2 keyboard on IRQ1 (scancode set 1, Shift and Caps Lock) or COM1 |
| clock | the PIT at 1 kHz; critical sections save and restore the flags |
| tasks | `switch.S` (callee-saved registers and the stack) and a round robin over the ready queue |
| store | `ata0`: a raw image on the primary IDE channel, PIO, MicroFS on its first partition, mounted at `/` (ADR 0008); `ram0`, 512 KB of the memory of the machine, when there is no disk |
| network | `virtio0`: virtio-net-pci in its legacy I/O interface, polled; ARP, IPv4, ICMP, UDP, a client of TCP (one segment in flight, a timeout retransmission) and a listener with SYN/ACK and `accept`; the resolver of QEMU user-net with a cache of eight names and their time to live; 10.0.2.15/24 on QEMU user-net, gateway 10.0.2.2, resolver 10.0.2.3, no DHCP |
| programs | `k_port_exec_supported()` is true; the window is 16 MiB at 32 MiB (`0x02000000`-`0x03000000`) over a physical arena at 64 MiB; a program is a file in `/bin` (the MOX1 format of `kernel/exec.h`), runs in ring 3 with page tables of its own and calls the kernel with `int $0x80` (ADR 0007) |
| protection | NX and W^X: `EFER.NXE` from the loader, the text read-only and executable, every data page - rodata, data, bss, stacks, heap, arena, screen - writable and NX; no page is both (ADR 0007, sections 9 and 10) |
| build | `make x86-build` (needs `cmake`, `ninja`, `xorriso` is not needed) and `make x86-disk` for the volume with `/bin` |
| check | `make x86-smoke` (serial), `make x86-vga-smoke` (screen and keyboard), `make x86-disk-smoke` (two boots on a disk), `make x86-net-smoke` (the network) - all need `qemu-system-x86` |

## Run it

```bash
make x86-build
make x86-disk         # the image of the disk: /bin and /etc of MicroFS
make x86-run          # a QEMU window: the VGA console and its keyboard
make x86-run-serial   # the same machine in a terminal (-nographic)
make x86-run-disk     # the window with a disk of its own, kept between runs
make x86-run-disk-serial  # the same disk in a terminal
make x86-net-run      # the same machine with a virtio-net card, in a terminal
make x86-smoke        # boot and check the lines of the banner and the shell
make x86-vga-smoke    # check the screen and the keyboard without a display
make x86-disk-smoke   # two boots on one image: a file written in the first is read in the second
make x86-net-smoke    # boot with QEMU user-net and check ping, UDP, HTTP, the page of the state and DNS
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
port; `tools/x86_vga_smoke.py` boots with `-display none`, reads the video
memory with the monitor of QEMU (`pmemsave`), takes a screenshot
(`screendump`) and types with `sendkey`, so it proves the screen and IRQ1
without a display server. It is a target of its own and not a part of
`make ci`, because the monitor of QEMU is a tool a build farm does not have
to have. `python3 tools/x86_vga_smoke.py --display gtk` runs the same checks
against the real window instead of its memory; the screenshot is then the
picture of the window, which is how a person watches the keys arrive.

The machine boots from three files: `build-x86/loader32.elf` (the loader),
`build-x86/microos64.bin` (the kernel, a flat image) and
`build-x86/programs.img` (the disk). The heap of the kernel stops below the
window of the programs; the kernel is linked at 16 MiB and the programs are
placed at 32 MiB. The sixteen programs of `/bin` are built first:
`build-x86/hello.mox`, `crash.mox`, `fork.mox`, `badptr.mox`, `signals.mox`,
`cat.mox`, `echo.mox`, `ls.mox`, `wc.mox`, `head.mox`, `grep.mox`,
`mkdir.mox`, `rm.mox`, `pwd.mox` and `spawn.mox` are the images of
`ports/port_x86_64/programs/` (`programs/program.ld` links them for the
window, `tools/mkprog.sh` turns them into the format). They are not blobs of
the kernel: `make x86-disk` writes them into a MicroFS volume of a raw disk
image with `tools/mkfs_mfs.c`, and the boot mounts that volume and runs
`/bin/hello` through `execve`. The last ten are linked against
`components/libc/user` and enter at `_start` (ADR 0016); the first five use
the raw `programs/prog.h`. The build passes `--base 0x02000000` to
`mkprog.sh`, which refuses an image linked for another window: nothing
relocates the program, so a wrong address would run and read its data from
somewhere else.

The virtual and the physical address are no longer the same number for a
program: `src/paging.c` builds its page tables with the window at 32 MiB
mapped onto the arena at 64 MiB, and the kernel reaches the bytes through the
identity map while the program reaches them through its own. `src/gdt.c`
builds the GDT the loader never had (user code and data, a 64-bit TSS with
`RSP0` and the double-fault stack), and `src/interrupts.S` takes the stack of
the TSS on a call from ring 3.

## The window: the VGA text and the keyboard

The screen of the mode the firmware leaves behind is a memory: 80 columns by
25 rows of two bytes at `0xB8000`, the character and its colour. The port
does not program a video mode and does not set a palette - the text mode 3 is
the one every PC has had since 1987, and a console is text. `k_port_write()`
writes every byte there and to COM1, so the window and the serial log are the
same picture and the boot has no second path for a second console; the kernel
does not know that there are two.

The screen obeys the control characters of a terminal - `\n`, `\r`, `\t` and
`\b` - scrolls a row up when the last line is full, and puts the position of
the cursor into the registers 14 and 15 of the CRTC (`0x3D4`/`0x3D5`) after
every write, so the cursor of the hardware is where the next character will
land. The colour is one pair for every cell: light grey on black. The one
escape sequence the console of the kernel writes is the pair of the `clear`
command (`ESC[2J` and `ESC[H`), and the screen reads it; a sequence of any
other kind is dropped whole instead of being printed, because a window is not
a terminal emulator and a `[2J` in the middle of a line is not a picture.

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

What the window does not do:

* it is the text mode and only the text mode: no graphics, no font of its
  own, no colour but the one pair;
* the scancode set 1 and the US layout only: a keyboard of another layout
  sends the same numbers and gets the letters of this table;
* the E0 sequences are dropped whole, so the arrows, Home/End, Delete, the
  second form of the keypad and the right Ctrl do nothing. The editing of a
  line is the one the shell has, not the one a library with a history has;
* no repeat of a key in the driver: the device repeats it, and the ring is
  the only buffer between the two.

## The disk

The disk is the machine's, not a test's: `make x86-disk` writes
`build-x86/programs.img`, an 8 MB raw image with an MBR, one Linux partition
at LBA 2048 and a MicroFS volume in it. The volume already carries `/bin`
(the sixteen programs) and `/etc/setting`: the host tool `tools/mkfs_mfs.c`
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

## What works

* the boot: the loader, long mode, the banner, the contract check at 1.8;
* the console: the VGA text screen and COM1 from one call, the keyboard of
  the PS/2 with Shift, Caps Lock, Backspace and Ctrl+C, and the serial port
  with the console it always had;
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
  the VGA, the boot information and the arena of the programs. A program has
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

* the disk is one, and it is the primary ATA channel: no SATA, no virtio, no
  NVMe, no card, no second disk;
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
* the kernel is not in a higher half: it is the identity map of the first
  gigabyte, so the window of the programs is a hole in the kernel's map. The
  ladder of the change (linker, loader, `page_phys()`, boot and stacks, the
  smoke gate) is in ADR 0007 and is not taken yet;
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
  of the state and a shell of telnet need.

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
