# ADR 0050 - The machine that switches itself off

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0041 (a disk this system did not format is read-only), ADR 0042
  (NVMe), ADR 0012 (many cores), ADR 0022 (the layout and the rollback),
  ADR 0008 (block devices), `docs/plan-1.8.md` M2, `docs/debts.md`,
  `docs/ports/esp32/hardware.md`

## Context

Until this change the machine had two ways to stop: a reset, and the power
being taken away. Neither is a shutdown.

* `reboot` was a write to the keyboard controller; the ABI call `reboot()` did
  the same from a program and never returned. Nothing was synced first.
* There was no `shutdown`, no `poweroff` and no `halt` at all. The end of a
  session was a `logout`, the end of a machine was a `Ctrl+]` of QEMU or a
  finger on a switch.
* A volume is written through the block layer and, on the two disks of the
  laptop (AHCI and NVMe, ADR 0041 and ADR 0042), through a cache that is
  emptied by a `FLUSH CACHE EXT` or an NVMe `Flush`. A machine that stops
  without those commands loses the last writes - the ones a person made and
  the shell said were written.
* The board has a `power off` that drops HOLD (G4) and a `power sleep` that
  arms the timer and BUTTON C, and neither of them syncs the volume first.

For a laptop that boots from a stick and reads the internal disk, and for a
wearable whose battery ends the day, "the write reached the medium before the
power did" is not a detail. It is the difference between a volume that mounts
at the next boot and one that has to be repaired - or, worse, one that the
rule of ADR 0041 refuses to mount because it was left dirty.

## Decision

### 1. Four states, and one of them is not a reset

| State | What it means | Who asks |
|---|---|---|
| running | the machine works | - |
| stopping | a clean end is in progress: no new session, tasks are being signalled, volumes are synced | `shutdown`, `poweroff`, `halt`, `reboot` |
| off | the power is gone (or the board sleeps with the button as its way back) | `poweroff`, `shutdown off` |
| reset | the processor starts from the reset vector with the volume clean | `reboot`, `shutdown reboot` |

`halt` is the fifth sentence of the same family and not a state of the power:
the machine is clean and the processor stops, but the power stays on. It is
what a panic does, and what a person does before switching a machine off by
hand. The three calls of the port are the three of the table: `K_POWER_HALT`,
`K_POWER_OFF`, `K_POWER_REBOOT` (`kernel/port/power.h`).

The ABI does not grow. `shutdown` and its siblings are commands of the shell;
a program that wants them runs the shell (or the machine has a `reboot()` of
its own, below). The number of the calls stays at 51.

### 2. The clean end is one order, written in one function

`kernel/power.c` runs the sequence and prints every step with the prefix
`power:`; a person who reads the console of a shutdown knows which step it
reached:

1. **No new session.** `k_power_ending()` answers true, `session_start()`
   refuses with a line of the log, and the getty of every port stops opening
   doors. A telnet client that arrives now waits for a machine that is going
   away, which is the honest answer.
2. **The first sync and flush.** The volumes are synced and every device of
   the block layer is asked for its flush (`k_blockdev_sync_all()`): the cache
   of the layer is written out, then the `FLUSH CACHE EXT` of an ATA/AHCI
   disk, the `Flush` of an NVMe namespace, the sync of a flash chip. This
   happens **before a task is signalled**: a disk that refuses the flush is
   the one failure the machine has to survive without casualties, and the
   pre-flight flush is what makes "the machine stays on" cheap.
3. **SIGTERM to every task but the shell that asked**, and a bounded wait
   (`K_POWER_TASK_WAIT_MS`, 2 s in steps of 20 ms). A task that catches the
   signal may write; a task that ignores it is named in the log and the
   shutdown goes on - the machine cannot wait forever for a program.
4. **The sync and the flush again**, because of the writes of step 3.
5. **Every mount is taken down, the last one first.** A filesystem writes the
   last of its metadata in its `umount` (ext2's bitmaps and superblock), and a
   mount below another is never busy when the order is reversed. A filesystem
   that refuses to leave stops the shutdown: its state is the last thing that
   has not reached the medium.
6. **The last flush, then `k_port_power(action)`.** The flush after the
   unmounts catches the cache of the medium under the metadata the `umount`
   wrote.

A refusal at any step clears the stopping state and returns the error to the
shell, which prints the sentence: the machine keeps running, the getty opens
its door again. The rule of ADR 0041 is the reason: **the data is worth more
than the speed of a shutdown**. The one exception is `-f`, which skips steps
2 to 6 and takes the machine down after a warning that no volume was synced;
no command makes it the default.

The shell prints its own sentence (`shutdown: the clean end of the machine
(off)`) and the kernel log carries the steps, so both the person at the
console and `dmesg` can see what happened.

### 3. The port owns the last step, and only the last step

The contract grows one call and one enum (`kernel/port/power.h`, version
1.13):

```c
enum k_power_action { K_POWER_HALT, K_POWER_OFF, K_POWER_REBOOT };
void k_port_power(enum k_power_action action);   /* does not return on success */
```

`k_port_halt()` and `k_port_reset()` stay: the panic uses the first, and the
`reboot()` of the ABI still uses the second as a hard reset (see "not done").
`kernel/power.c` panics if a port returns from `k_port_power()`.

**x86-64 (`ports/port_x86_64/src/acpi.c`).** The port already reads the ACPI
root pointer for the processors (`src/cpu.c`); the FADT (`FACP`) names the
PM1a control block (and PM1b), and the write of `SLP_TYPa << 10 | SLP_EN` is
the S5 request. The table is found through the RSDP - the one the UEFI loader
hands over, or the scan of the extended BIOS area - then the XSDT (or the
RSDT of an older firmware). A machine without a FADT, or a firmware whose S5
did not take, gets the two ports of the emulators: `0x604` with `0x2000` is
the PIIX4 of QEMU, `0xB004` with `0x2000` is Bochs. A machine with neither
ignores both, and the port says so and **halts**: the power stays on and a
person presses the button, which is better than a reset that pretends the
machine went off.

The reset is the reset register of the FADT when the firmware named one (an
I/O port or a byte of memory and a value, ACPI 6.x 4.8.3.6), then the reset
control port `0xCF9` of the chipset, then the keyboard controller of 1984.
The first two are tried on every machine; the last is the old fallback.

**host (`ports/port_host/src/port_host.c`).** The process is the machine, and
the exit code is the state it reached: **0 = off** (a success), **76 =
reset** (a supervisor may start it again), **89 = halt/panic** (a failure).
The numbers are written down in the port and here, because a script of the
tree reads them by name.

**ESP32 (`components/port_esp32/src/port_esp32.c`).** `reboot` is
`esp_restart()`. `off` is a deep sleep that keeps HOLD (G4) high and arms
EXT1 on BUTTON C (G35) as the only wake source: a board on a battery is off
until a person presses the button, and the chip restarts from the reset
vector when it does. The driver (`src/drv_power.c`) clears the alarm of the
clock first - its INT line is part of the power latch - and drops the latch
instead if the wake source cannot be armed, because a sleep with no way back
is a dead board. `halt` is the idle loop of the kernel with the power kept.

**null.** Nothing. A test that reached the call has a bug of its own, and the
kernel panics on the return.

### 4. The next cold start sees a clean volume

The shutdown is only worth its name if the boot after it finds what it left.
The volume is clean because the `umount` of every filesystem ran and the
disks were flushed after it; the file a person wrote is in the medium, not in
a cache; and the boot mounts the volume as it finds it, without "formatting
because it did not mount". `make power-smoke` is the proof: a boot writes
`/p.txt`, `shutdown` ends the QEMU process by itself, the same image starts
again, `fsck` says the volume is consistent and `cat /p.txt` reads the line.

On the board, a wake from the deep sleep is a start of the chip and not a
continuation of the RAM: the machine **starts again**, the boot log names the
wake (`power: woke from ext1`), and the volume is the one the shutdown left.
That is the honest answer for a machine with no hibernation: this release
continues from the clean volume, not from the memory.

### 5. Gates

* `make power-smoke`: (a) a boot writes a file and `shutdown` ends the QEMU
  process (the S5 of ACPI); (b) the same image boots on a clean volume with
  the file still there; (c) `reboot` resets the guest **in the same QEMU
  process** (the tool does not pass `-no-reboot`) and the volume and the file
  are the same; (d) the negative - a disk that does not answer the flush keeps
  the machine alive - is `tests/cases/test_power.c`, with a block device that
  fails on purpose.
* `make x86-disk-smoke` ends its second boot with `shutdown`, so the disk of
  the IDE, the SATA and the NVMe buses is also taken down cleanly.
* The host tests of the kernel cover the sync of every device and the refusal
  of the flush; the port contract test checks the new call.

## What this ADR does not do

* **the SLP_TYPa of S5** is 5, the value every machine the project has met
  uses. The real value is an object of the DSDT (`\_S5`), an AML program this
  kernel does not execute; a machine that names another one is a line of
  `docs/debts.md`, not a reason to leave the port without a soft off.
* **the power button of a laptop** is not handled. ACPI's fixed event block
  names it and the SCI would have to be routed and read; today a press is the
  firmware's business (it may switch the machine off by itself), and a clean
  shutdown from the button is a lane of its own.
* **the RTC alarm as a wake source of `off`** on the board is not used: the
  alarm wakes a *sleep* (`power sleep alarm`), not the off of the clean end.
  A wearable that wakes itself in the morning is a promise of phase 9.
* **S5 on the board** does not exist: the ESP32 has no PMIC and the only
  switch-off is the latch or the deep sleep of this ADR.
* **the `reboot()` of the ABI** is still the hard reset of `k_port_reset()`;
  a program that calls it loses the writes of the cache, and the clean
  reboot is the shell command. Changing the result of a call that is
  documented as "never returns" is a change of the ABI record, and it waits
  for the release that also gives programs `shutdown`.
* **a `sync` syscall or command** is not added: the clean end is the shell's,
  and a program that wants its bytes on the medium closes its files, which is
  the contract of the filesystem.
