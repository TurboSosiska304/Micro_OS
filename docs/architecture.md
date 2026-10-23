# Micro OS architecture

> Russian version: [architecture_ru.md](architecture_ru.md)

## 1. Overview

```
+---------------------------------------------------------------+
| Applications: /bin/sh, utilities, daemons (init starts them)  |
+---------------------------------------------------------------+
| API: POSIX-like interface (open/read/write/fork/...)            |
+---------------------------------------------------------------+
| Kernel:                                                       |
|   sched   - task scheduler (PCB, states, PIDs)                  |
|   vfs     - virtual filesystem: inode, dentry, mount, /proc, /dev|
|   kmm     - memory manager (kmalloc, slab, pools)               |
|   ksys    - system call table and its ABI                       |
|   kshell  - command interpreter                                |
+---------------------------------------------------------------+
| Drivers: fb(tft), input(buttons), buzzer, imu, rtc, ir, mic     |
| Wifi: the portable core of the station + the radio of the ESP32 |
+---------------------------------------------------------------+
| Port: ESP32 (ESP-IDF) | Host (Linux, for tests and simulation)  |
+---------------------------------------------------------------+
```

Rule number one: **the kernel knows nothing about hardware**. Everything that
depends on the platform sits behind the `kernel/port.h` contract:

```c
void     k_port_write(const char *data, k_size_t len);
k_u64_t  k_port_uptime_ms(void);
void     k_port_enter_critical(void);
void     k_port_exit_critical(void);
void     k_port_halt(void);
void     k_port_reset(void);
const char *k_port_name(void);
```

That is exactly what allows the kernel tests to run on a laptop.

## 2. What is Linux-like here, and what is not

| Property | Linux | Micro OS |
|---|---|---|
| Modular monolith | yes | yes (kernel + drivers in one address space) |
| Layered subsystems | yes | yes (`kernel/boot.h`, initialisation levels) |
| Processes / threads | yes | yes, simplified, from phase 1 |
| Filesystem | yes | yes, from phase 2 (in-RAM FS plus a VFS layer) |
| `/proc`, `/dev`, `/sys` | yes | `/proc` and `/dev` from phase 2, `/sys` later |
| System calls | yes | yes, from phase 3, but **without hardware isolation** |
| Memory protection | MMU | **none** (the ESP32 has no MMU) |
| Multi-user | yes | no (a single user: the device itself) |
| Dynamic linker | yes | no (everything is statically linked) |

We honestly call this **Linux-like**, not Linux: without an MMU there is no real
separation of user and kernel space. System calls are ordinary function calls
with a uniform ABI; the boundary is kept by discipline, not by hardware.

## 3. Boot model

Booting mirrors the Linux model: subsystems are registered with a **level** and are
initialised in level order.

| Level | Contents | Examples |
|---|---|---|
| `CORE` (0) | critical subsystems | log, board, memory |
| `DRIVER` (10) | hardware drivers | tft, input, buzzer, imu, rtc |
| `SERVICE` (20) | system services | vfs, procfs, devfs |
| `APP` (30) | applications | init, shell |

Every subsystem returns 0 or a negative error code. A failed initialisation
triggers a panic that names the subsystem, so a broken driver is diagnosed
immediately instead of causing mysterious crashes later.

Phase 0 already implements this mechanism (`components/kernel/src/boot.c`),
including level ordering and error checking.

## 4. Scheduler (phase 1)

* `struct task` — the analogue of `task_struct`: state, priority, stack, TID,
  quantum counter, file descriptors, working directory, environment.
* States: `TASK_NEW -> TASK_READY -> TASK_RUNNING -> TASK_SLEEPING -> TASK_ZOMBIE`.
* Priorities 0..`TASK_PRIO_MAX`; scheduling is preemptive round-robin with
  priorities, like Linux `SCHED_RR`.
* Timer: 1 ms tick (FreeRTOS tick or `esp_timer`), `sleep()` and `usleep()` are
  implemented on top of a timer queue.
* PIDs are handed out starting from the init process (as in Linux), and
  `/proc/<pid>` are virtual directories.

### Execution substrate

Preempting a task at an arbitrary point requires a context switch. On ESP32 there
are two options:

1. **FreeRTOS as substrate** — every kernel task is a FreeRTOS task, priorities
   map one to one. Fast and reliable, but preemption is not "ours".
2. **Our own scheduler on top of FreeRTOS** — FreeRTOS provides a single
   low-level task (or an ISR trigger) and we switch contexts ourselves by
   saving and restoring stack pointers and registers.

Plan: start with option 1 (it works immediately), move to option 2 when real
Linux-like semantics are needed (priorities, states, `fork`). The decision is
recorded in [ADR 0002](adr/0002-scheduler-substrate.md).

## 5. Virtual filesystem (phase 2)

The goal is "everything is a file":

```
/            root (in-RAM FS: /etc, /bin, /dev, /tmp, /proc)
/proc        virtual FS: version, uptime, meminfo, tasks, self/
/dev         devices: tty0, imu, rtc, buzzer, ir, mic
/sys         (later) driver parameters
```

Implementation: `inode` -> `dentry` -> `file_operations` (open/read/write/ioctl),
as in Linux, but with a flat dentry list inside a single binary tree.

### Who may work with the filesystem at the same time

The volume is one piece of state - the map of the free blocks, the table of
the inodes and the cache of the sector of the header all describe the same
storage - so two tasks that write files at the same time have to be kept
apart:

* every open file belongs to the one that opened it: the position and the
  flags of the file live in the descriptor (`VFS_FILE_DATA_SIZE` bytes of
  private data), not in a table of the filesystem. Two tasks that read
  their own files never touch each other's position;
* the filesystem itself has one lock, taken by every public call of
  `mfs_*`. The kernel has no blocking primitive yet, so the lock is a
  "sleep lock": a task that has to wait sleeps a millisecond and looks
  again, and the CPU goes to the task that holds the lock. It is
  recursive, because the functions of the filesystem call each other
  (`mfs_close` puts the changes on the flash through `mfs_sync`);
* the adapter between the filesystem and the VFS
  (`components/mfs/src/mfs_vfs.c`) is the only place where the codes of
  the errors of the filesystem are translated into the codes of the
  contract of the system calls: the two sets are close but not the same
  (`-9` is "the directory is not empty" of the filesystem and a "bad file
  descriptor" of the kernel).

## 6. Memory (phase 3)

* `kmm_alloc`/`kmm_free` — our own allocator (bitmap or slab on top of the ESP-IDF
  heap).
* Large objects (frame buffers, file caches) live in PSRAM, small ones in internal
  SRAM (which is scarce — that is the constraint).
* Virtual memory is emulated: each task has its own "address space", but without
  an MMU a switch means switching the stack pointer and the data segment base.

## 7. Drivers (phase 4)

The interface follows Linux:

```c
struct file_operations {
    int  (*open)(struct inode *ino, struct file *f);
    ssize_t (*read)(struct file *f, void *buf, size_t count);
    ssize_t (*write)(struct file *f, const void *buf, size_t count);
    int  (*ioctl)(struct file *f, unsigned long cmd, void *arg);
};
```

Planned drivers: `drv_framebuffer` (ST7789v2), `drv_input` (A/B/C),
`drv_buzzer`, `drv_imu` (MPU6886), `drv_rtc` (BM8563), `drv_ir`, `drv_mic`
(SPM1423), `drv_led`. The pinout and the quirks are documented in
[docs/hardware/stickc-plus2.md](hardware/stickc-plus2.md).

The WiFi driver is built like the kernel itself: a portable core
(`components/wifi`) that owns the state of the station and a radio
(`components/wifi/src/wifi_esp32.c`) that talks to the ESP-IDF. The two talk
through a table of functions, so the state machine of the driver is covered
by the unit tests of the host. The decision is recorded in
[ADR 0003](adr/0003-wifi-driver.md).

### The panel is one piece of hardware

The SPI of the panel is a sequence: the command, the data, the window and
the pixels have to arrive in that order, and a second task that writes its
own pixels in the middle of a transfer changes the meaning of the commands
on the wire. Everything that draws (`drv_display_clear`, `drv_display_text`,
`drv_display_flush`, `drv_display_set_offset`) takes the lock of the panel,
and the lock is recursive because the functions of the panel call each
other. The shared buffer of the transfer and the offset of the image are
behind the same lock.

The console of the board (`components/graphics`) does not draw from the task
that prints. It writes the lines down and says that the screen is out of
date, and one task of its own, of a low priority, brings the screen up to
date at most ten times a second: a listing of fifty lines is one redraw of
the panel instead of fifty, and the task that prints does not stand still
in the middle of the dialogue.

## 8. Testing

* The kernel is tested on the host: `make test` (own framework, no dependencies).
* The simulator `make sim` runs the very same kernel on Linux, so the shell and
  the VFS can be debugged without flashing.
* On the device there is a UART console; the output can be mirrored to the TFT
  (phase 4).

## 9. Resource budget

| Resource | Available | Comment |
|---|---|---|
| Flash | 8 MB | `factory` 2 MB + MicroFS 5.75 MB (one application, no OTA) |
| PSRAM | 2 MB | file data, buffers |
| Internal SRAM | ~100 KB usable by code | task stacks, drivers |
| CPU | 240 MHz, 2 cores | already taken by FreeRTOS; our kernel lives on top |
| Battery | 200 mAh | deep sleep + RTC is the main "wearable" scenario |
