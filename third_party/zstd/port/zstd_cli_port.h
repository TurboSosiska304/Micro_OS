/*
 * Micro OS — the configuration of the zstd CLI for a program of this system.
 *
 * The header is force-included into every source of `third_party/zstd/programs`
 * (`-include port/zstd_cli_port.h`), and it is the port layer of the command:
 * the settings the release reads, the calls of a Unix-like system that the
 * library of a program does not have, and the memory policy of a machine whose
 * program owns a megabyte of bss and no more.
 *
 * The release of upstream is not edited: a setting it reads before its own
 * defaults is here, and the few calls whose shape differs are a wrapper of
 * this directory (`port/zstd_cli_os.c`). The numbers and the prototypes of the
 * wrappers are in `port/zstd_cli_os.h`, which the file of the wrappers shares
 * without the renames below - a wrapper may not call itself.
 */
#ifndef MICRO_OS_ZSTD_CLI_PORT_H
#define MICRO_OS_ZSTD_CLI_PORT_H

/* The library of the program, then the settings of the library of zstd. */
#include <ulibc.h>

/*
 * The `assert` of the program comes before the one of the release: the
 * `debug.h` of zstd defines a macro of the same name for a build without
 * DEBUGLEVEL and leaves a previous definition alone (`#ifndef assert`), so the
 * include here is what keeps the two from meeting in the wrong order.
 */
#include <assert.h>

#include "zstd_port.h"
#include "zstd_cli_os.h"

/* The types of the wrappers below, and the declarations of zstd.h under their
 * own names before the four renames at the end of this header. The section of
 * the API that is not stable is the one the CLI of the release reads
 * (`ZSTD_c_srcSizeHint`, `ZSTD_c_targetCBlockSize` and the rest of the
 * parameters `fileio.c` names), so it is asked for here as `fileio.h` does. */
#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include "../lib/zstd.h"

/*
 * What the CLI of this machine does not build:
 *
 *   ZSTD_NOBENCH  the benchmark (`-b`) times a compressor with the clock of a
 *                 host and reports the ratios of a machine with a disk: the
 *                 code is a tenth of the image and the answer means nothing
 *                 here.
 *   ZSTD_NODICT   the dictionary *builder* (`--train`) needs the cover and
 *                 fastCover algorithms of lib/dictBuilder, which this port does
 *                 not carry. The release answers "training mode not
 *                 available" where a dictionary would be trained, and a
 *                 dictionary that somebody else trained still works: `-D` is a
 *                 call of the library (ZSTD_CCtx_loadDictionary) and not of
 *                 the builder.
 *   ZSTD_NOTRACE  the trace of the CLI (`--trace`) writes a file of the
 *                 events of a stream for the developers of zstd; the library
 *                 is built without one (port/zstd_port.h).
 */
#ifndef ZSTD_NOBENCH
#define ZSTD_NOBENCH 1
#endif
#ifndef ZSTD_NODICT
#define ZSTD_NODICT 1
#endif
#ifndef ZSTD_NOTRACE
#define ZSTD_NOTRACE 1
#endif

/*
 * The CLI asks `platform.h` for a POSIX version and takes the POSIX branch of
 * a call when it has one. This system has the shape of a Unix-like one and not
 * its calls: no `mmap` of a dictionary, no `nanosleep`, no `setpriority`, and
 * no `_POSIX_VERSION` in `<unistd.h>`. Zero is the value the release documents
 * for a machine that is not POSIX ("Value of PLATFORM_POSIX_VERSION can be
 * forced on command line"), and it selects the C paths of the release: the
 * dictionary is read with `fread`, and the four `IS_CONSOLE` macros answer 0.
 */
#ifndef PLATFORM_POSIX_VERSION
#define PLATFORM_POSIX_VERSION 0
#endif

/*
 * The CLI of a host prints a stack trace of a fatal signal with the
 * `backtrace()` of glibc when it sees `__GLIBC__`; the compiler of this
 * machine is a glibc-targeted one and its `<limits.h>` defines that macro even
 * though the program is linked against nothing of glibc. The detector is
 * answered here: this machine has no `execinfo.h`, no `SIGSEGV` of a
 * disposition and no symbol table to print.
 */
#ifndef BACKTRACE_ENABLE
#define BACKTRACE_ENABLE 0
#endif

/*
 * A sparse write seeks over a run of zeros and writes the last one. MicroFS
 * has no hole: the blocks of the range would be allocated one by one by the
 * seek, and the file of a decompressed stream is written whole anyway. The
 * release turns the feature off for a destination that is not a regular file;
 * this is the same answer for every destination of this system.
 */
#ifndef ZSTD_SPARSE_DEFAULT
#define ZSTD_SPARSE_DEFAULT 0
#endif

/*
 * The four sizes of the buffers of a stream. The release asks the library for
 * the recommended size of a host; the wrapper answers with the size of this
 * machine, and the CLI allocates its pools from the answer (four buffers of
 * the read pool and two of the write one). The library reads and writes in
 * pieces of any size, so the answer changes the number of calls and not the
 * bytes.
 */
#define ZSTD_CStreamInSize zstd_port_cstream_in_size
#define ZSTD_CStreamOutSize zstd_port_cstream_out_size
#define ZSTD_DStreamInSize zstd_port_dstream_in_size
#define ZSTD_DStreamOutSize zstd_port_dstream_out_size

/*
 * The window of a decoded frame and the parameters of a compressed one, under
 * the policy of `port/zstd_cli_os.h`. The compressor is the one place the
 * release sets a parameter to zero for "the default of the level", and the
 * wrapper reads that zero as "the window of this machine".
 */
ZSTD_DCtx *zstd_port_create_dctx(void);
size_t zstd_port_cctx_set_parameter(ZSTD_CCtx *cctx, ZSTD_cParameter parameter, int value);

#define ZSTD_createDCtx zstd_port_create_dctx
#define ZSTD_CCtx_setParameter zstd_port_cctx_set_parameter

#endif /* MICRO_OS_ZSTD_CLI_PORT_H */
