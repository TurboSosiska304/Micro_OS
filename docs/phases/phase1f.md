# Phase 1F - The hardware doctor

The init process walks through the registered modules and checks them one by
one. The result is a report: `ok`, `degraded`, `fail` or `skip`.

```
module health report:
  module     state     detail
  psram      ok        2048 KB
  flash      ok        8 MB
  imu        ok        acc 1644,-228,16784 (raw)
  rtc        degraded  25:00:00 (no clock)
  display    ok        135x240, fb 63 KB
  input      ok        A=up B=up C=up
  buzzer     ok        passive, 2 kHz via LEDC
  led/ir     ok        G19, shared with the IR emitter
  battery    ok        4170 mV (raw 2085 mV)
  wifi       degraded  f4:65:0b:32:00:c0 idle
  mic        skip      SPM1423 MEMS microphone (I2S)
11 module(s), 0 problem(s)
```

`degraded` means that the module works, but with a limitation: the radio
without an access point, the clock of the RTC without a battery.

## The mistakes that cost time

Every one of them was found by the doctor and not by the tests on the host.

* **The flash of the ESP32 can only clear the bits.** A sector that is
  written over old data needs the bits to go from 0 to 1, and the driver
  refuses it. The adapter of the filesystem reads the sector, changes the
  part it needs, erases the sector and writes it back. The sector that is
  being changed is kept in the memory, so that a megabyte is not erased for
  every kilobyte.
* **The changes of the format never reached the flash.** The volume was
  mounted from the memory of the sector cache and was broken after the
  restart. The format has to call the sync of the device at the end.
* **The SPI of the display was called with the host instead of the
  descriptor.** The driver dereferenced a small number and the board died
  with a LoadProhibited exception.
* **40 MHz is too fast for the SPI through the GPIO matrix.** The limit is
  26.7 MHz, the display works at 20 MHz.
* **The DMA of the SPI cannot read the PSRAM.** The framebuffer is sent to
  the panel in pieces of 4 KB through a buffer in the internal memory.
* **The last argument of `i2c_master_transmit()` is a timeout in
  milliseconds, not a flag.** The value `true` meant 1 ms and the
  transactions were cut short, depending on the phase of the boot.
* **`esp_timer_create()` wants the arguments in the other order.** The
  callback was read from the wrong place and the time base never started.
* **The stack of a task was too small.** The buffer of a directory is 4 KB,
  it was on the stack. The stack of a task is 8 KB now and the big buffers
  are static.
* **A panic has to reach the console even when the log is collected.** The
  crash of the board looked like a board that simply did not answer.

## What is worth remembering

* The doctor is not a formality, it is the instrument that finds the bugs of
  the hardware. Every one of the bugs above looked like "the device is
  broken" while the code was wrong.
* The report of the doctor is the same as the one in `/proc/modules`.
