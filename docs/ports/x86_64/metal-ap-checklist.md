# The checklist of the bring-up on real metal

The same image boots a firmware machine and a QEMU machine. QEMU of two or
four cores is green, but a laptop or a desktop can still come up with the
processor it booted on, or reset before the shell. These are the lines to
read when that happens; no debugger or serial wire is needed.

## What to type

At the shell of the machine:

```
smp
dmesg | grep -i ap
dmesg | grep 'the boot is'
```

The first command prints the cores the kernel runs and the state of the
bring-up of every core the firmware named. The second prints the summary the
boot left in the kernel log; the third says `uefi` or `multiboot`, which is
the boot path the trampoline had to come from.

## What success looks like

On the ThinkPad X1 Carbon Gen 6 (eight threads) the answer has to be:

```
smp: 8 core(s) in use of 8 reported, apic 0xfee00000
smp: ap trampoline from kernel image, signature ok
smp: ap cpu 1 apic 0x2: accepted ok, self 0x2, ...
...
smp: 7 AP(s) reached C of 7 named by the firmware, 0 failure(s)
```

`from kernel image` is the expected word on a firmware boot: the UEFI loader
does not carry the trampoline, so the kernel installs it. `from loader` is
the BIOS path. Both are fine when the signature is `ok` and every core is
`accepted`.

## What to send back when it is not

Copy the whole output of `smp` and of `dmesg | grep -i ap`, plus the line
that names the boot. The useful lines are these:

* `smp: ap trampoline ... signature missing` — the page at `0x7000` was not
  installed; no SIPI was sent, and the machine stayed on one core on purpose.
* `smp: ap cpu N apic 0xX: <phase> failed, ... us of ... us` — the name of
  the phase is where the bring-up stopped: `trampoline signature`,
  `parameters`, `INIT`, `10 ms after INIT`, `SIPI 1`, `200 us gap`, `SIPI 2`,
  `in C` or `accepted`.
* `ICR <command> did not finish, waited N us` — the local APIC did not
  accept a command; the command may be `INIT assert`, `INIT deassert` or
  `SIPI`.
* `answered as 0xX, not accepted` — the processor identified itself with a
  different APIC id than the MADT named; it was kept out of the scheduler.

The lines of the phases also appear on the screen during the boot, before
the shell, so a machine without a serial port still says where it stopped.

## The safe stick

The Makefile has a switch for the policy of the cores:

```
make uefi-stick SMP=OFF
```

Write `build-uefi/stick.img` to a USB stick and boot from it. If that stick
reaches a shell and a stick built the default way (`SMP=ON`) resets or comes
up with one core, the fault is in the bring-up of the application processors
and not in the screen, the disk or the network of the machine. `SMP=OFF` is
a diagnosis and a way to leave the machine usable; it is not the fix.
