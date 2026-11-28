/*
 * Micro OS — the copy of the system and the manifest that verifies it.
 *
 * The install of L4 copies what the running machine really has: the kernel of
 * boot module 0, the programs of /bin and the defaults of /etc. Every file
 * gets its size and its SHA-256 in /etc/install.manifest; the hash of the
 * manifest itself lives beside it in /etc/install.manifest.sha256. The two
 * calls below write that tree and check it through the VFS, so one filesystem
 * or the other makes no difference to the code.
 */
#ifndef MICRO_OS_PORT_X86_64_INSTALL_COPY_H
#define MICRO_OS_PORT_X86_64_INSTALL_COPY_H

#include "kernel/types.h"

#define INSTALL_KERNEL_PATH "/boot/kernel"
#define INSTALL_MANIFEST_PATH "/etc/install.manifest"
#define INSTALL_MANIFEST_HASH_PATH "/etc/install.manifest.sha256"

/*
 * Copy the system below `point`, which has to be the mount of the freshly
 * formatted volume. The manifest and its hash are written last. Returns 0, or
 * a KERR_* code; a failed copy leaves a partial tree on the volume.
 */
int port_x86_64_install_copy(const char *point, k_u32_t *out_files);

/*
 * Read the manifest below `point` and hash every file it names. A mismatch
 * names the file on the console and returns KERR_IO. The hash of the manifest
 * is checked first, so a changed line cannot verify itself. Returns 0 on
 * success and sets `out_files` to the number of verified files.
 */
int port_x86_64_install_verify(const char *point, k_u32_t *out_files);

#endif /* MICRO_OS_PORT_X86_64_INSTALL_COPY_H */
