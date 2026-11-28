# Hang audit

The watchdog of the kernel (`kernel/watchdog.h`, `kernel/watchdog.c`) prints
one dump per incident when a task does not get CPU time or progress for
`K_WATCHDOG_THRESHOLD_MS` (5 s). This document lists the wait loops in
`components/kernel/src/*.c` and `ports/port_x86_64/src/*.c` and the reason each
one is bounded or intentionally unbounded.

## Kernel

| File:line | Loop | Bound / reason |
|---|---|---|
| `kfile.c:267` | pipe read waits for data | Unbounded wait on `K_WAIT_CHANNEL_PIPES`; returns on data, EOF (no writers), or `K_WAIT_SIGNAL`. Intentional: pipe read blocks until writer produces or closes. |
| `kfile.c:332` | pipe write waits for room/reader | Unbounded wait on pipes; returns on room, EPIPE (no readers), or signal. Intentional: backpressure. |
| `wait.c:178` | `k_wait_sleep` main loop | Bounded by `timeout_ms` argument or interrupted by signal/wake; `K_WAIT_INFINITE` is intentional for callers that own the condition. |
| `mm.c:449` | gap search in VMA tree | Bounded by address space; progresses `candidate` upward and terminates on gap or limit. |
| `panic.c:54` | halt after panic | Intentional terminal loop; machine is dead. |
| `logfile.c:341` | log rotation/write loop | Bounded by file size/rotation steps; returns on I/O error. |
| `task.c:995` | `task_exit` not reached | Intentional terminal loop after `k_port_task_exit`. |
| `task.c:1147` | `task_wait` poll | Bounded by `timeout_ms`; returns on zombie/dead/timeout. |
| `task.c:1686` | `task_wait_any` poll | Bounded by `timeout_ms`; returns on any child exit/timeout. |
| `tty.c:246` | tty wait byte | Bounded by chunked waits and optional deadline; polls console and sleeps on `K_WAIT_CHANNEL_CONSOLE`. |
| `tty.c:523` | canonical line read | Waits for line/EOF/signal; `tty_wait_byte` returns signal or byte. Intentional interactive read. |
| `blockdev.c:205` | Robin Hood hash probe | Bounded by cache table size; breaks on empty slot. |
| `blockdev.c:374` | dirty run extend forward | Bounded by cache size; breaks on non-dirty or missing slot. |
| `blockdev.c:384` | dirty run extend backward | Bounded by cache size; breaks on non-dirty/missing/sector 0. |
| `kmm.c:108` | heap lock spin | Sleeps 1 ms between attempts; unbounded if owner deadlocks. Short critical sections by design; watchdog dump names mutex/lock holders. |
| `ksys.c:865` | poll/select wait loop | Bounded by timeout argument; wakes on channel events or signal. |
| `sched.c:635` | `k_sched_sleep_unless` | Bounded by deadline; breaks on stop/wake/block/unblock/timeout. |
| `vfs.c:1876` | read loop until EOF | Bounded by file size; breaks on EOF or read error. |

## x86-64 port

| File:line | Loop | Bound / reason |
|---|---|---|
| `virtio_blk.c:364` | virtio-blk request wait | Bounded by TSC/timeout budget; returns error on timeout. |
| `ahci.c:345` | AHCI register wait | Bounded by TSC budget/fallback polls. |
| `ata.c:262` | ATA BSY/DRQ wait | Bounded by TSC budget/fallback polls. |
| `ata.c:487` | ATA DMA completion | Bounded by TSC budget/fallback polls. |
| `net.c:603` | DHCP RX wait | Bounded by caller deadline/timeout in surrounding logic; polls network. |
| `net.c:1868` | TCP recv wait | Bounded by `NET_WAIT_DEFAULT_MS` deadline. |
| `net.c:2006` | TCP accept wait | Bounded by `timeout_ms` deadline. |
| `e1000e.c:354` | e1000e register wait | Bounded by TSC budget/fallback polls. |
| `port_exec.c:1237` | exec wait for child | Bounded by child exit; Ctrl+C forwarded; parent wait has external stop conditions. |
| `port_exec.c:1582` | exec trampoline not reached | Intentional terminal loop after `task_exit`. |
| `usb_bt.c:280` | HCI event wait | Bounded by `BT_HCI_EVT_TIMEOUT_MS`. |
| `fb.c:374` | Bresenham line draw | Bounded by pixel count; breaks when endpoint reached. |
| `port_x86_64_task.c:610` | idle task loop | Intentional; waits for interrupt forever. |
| `port_x86_64_task.c:869` | task trampoline not reached | Intentional terminal loop. |
| `port_x86_64_task.c:1163` | port sleep wait loop | Bounded by sleep deadline; emits SLPDBG on stall and hard timeout recovery. |
| `port_x86_64_task.c:1650` | AP/idle loop | Intentional; schedule + hlt forever. |
| `paging.c:737` | panic halt (image too large) | Intentional terminal halt. |
| `smp.c:649` | AP halt on fatal bring-up failure | Intentional terminal halt. |
| `smp.c:709` | AP halt on fatal runtime failure | Intentional terminal halt. |
| `nvme.c:294` | NVMe ready wait | Bounded by TSC budget/fallback polls. |
| `nvme.c:333` | NVMe completion wait | Bounded by TSC budget/fallback polls. |
| `xhci.c:712` | xHCI command completion | Bounded by uptime deadline and poll spin limit. |

## Notes

- "Intentional" means the loop is the correct steady state (idle, panic halt)
  or a terminal not-reached guard.
- "Unbounded wait" means no fixed wall-clock deadline inside the loop; progress
  depends on another task, device, or signal. These are the highest-value
  watchdog targets because they can become permanent hangs when the wake never
  arrives.
- The watchdog dump is emitted through `k_port_write()` and avoids locks. On
  x86 it can also be triggered by QEMU NMI or the COM1 debug byte `0x14`.
