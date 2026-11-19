/*
 * Micro OS — the flash of the ESP32 as a block device.
 *
 * The partition `mfs` of partitions.csv becomes a device named "flash0":
 * the filesystem asks for a sector, and this file turns it into a read,
 * a write or an erase of the partition.
 *
 * role-backends-block: the body is filled by the portability work; until
 * then no device is registered and the filesystem has nothing to mount.
 */
#include "port_esp32.h"

void port_esp32_blockdev_init(void) {}
