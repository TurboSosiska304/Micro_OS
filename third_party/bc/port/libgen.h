/*
 * Micro OS — the `<libgen.h>` of the bc port.
 *
 * `src/main.c` includes the header on every system that is not Windows and
 * does not call a function of it: the name of the calculator is the part of
 * `argv[0]` after the last separator, which the release takes itself. The
 * program of this machine has no `<libgen.h>`, so the port supplies an empty
 * one; the host build includes the real header and the sources see no
 * difference.
 */
#ifndef MICRO_OS_BC_PORT_LIBGEN_H
#define MICRO_OS_BC_PORT_LIBGEN_H

#ifdef BC_PORT_HOST
#include_next <libgen.h>
#endif

#endif /* MICRO_OS_BC_PORT_LIBGEN_H */
