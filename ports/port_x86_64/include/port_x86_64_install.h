/*
 * Micro OS — the door the installer opens on a disk of the x86-64 machine.
 *
 * The rule of ADR 0041 decides whether a *mount* may write a disk this system
 * did not make. An install is a different act: the operator names the exact
 * disk and size, and the volume the rule would name does not exist yet. After
 * that confirmation the installer opens the writes of the target controller,
 * including the table area it is about to append one entry to. The door is
 * closed again when the install returns, successful or not.
 */
#ifndef MICRO_OS_PORT_X86_64_INSTALL_H
#define MICRO_OS_PORT_X86_64_INSTALL_H

#include "kernel/blockdev.h"
#include "kernel/types.h"

/* The controller-level hooks, called by the dispatcher below. */
void nvme_install_writes(bool open);
void ahci_install_writes(bool open);

/*
 * Open (`open == true`) or close the writes of `disk` for an install. A disk
 * without a rule needs no override. Returns 0, or a KERR_* code.
 */
int port_x86_64_install_writes(const struct k_blockdev *disk, bool open);

#endif /* MICRO_OS_PORT_X86_64_INSTALL_H */
