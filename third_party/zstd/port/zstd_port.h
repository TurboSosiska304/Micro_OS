/*
 * Micro OS — the configuration of the library of zstd for a program.
 *
 * The header is force-included into every source of the library
 * (`-include port/zstd_port.h`), before zstd reads its own hooks, so the
 * release of `third_party/zstd` stays byte for byte and an update is a copy of
 * its files. What it turns off is what this system does not have:
 *
 *   ZSTD_NO_TRACE       the library's trace hook is a pointer a caller may
 *                       set; the CLI of this port never sets it, and the
 *                       machinery of a trace is code the image does not need.
 *   ZSTD_DISABLE_ASM    the decompressor of 1.5.7 has an assembly loop of the
 *                       Huffman decoder (huf_decompress_amd64.S) that the
 *                       build of this tree does not carry: the C path is the
 *                       one of every other machine, and a program of this
 *                       system is compiled for one processor at a time.
 *   ZSTD_LEGACY_SUPPORT 0
 *                       the frames of a version before 1.0 are refused with
 *                       the sentence of the library instead of the code of a
 *                       decoder for a format nobody writes any more.
 *
 * `ZSTD_MULTITHREAD` is never defined: it needs `pthread_*`, which the library
 * of a program of this system does not have. A single-thread context is the
 * one the CLI of this port builds (`zstd_cli_port.h` says so for the program
 * side of the same decision).
 */
#ifndef MICRO_OS_ZSTD_PORT_H
#define MICRO_OS_ZSTD_PORT_H

#ifndef ZSTD_NO_TRACE
#define ZSTD_NO_TRACE 1
#endif

#ifndef ZSTD_DISABLE_ASM
#define ZSTD_DISABLE_ASM 1
#endif

#ifndef ZSTD_LEGACY_SUPPORT
#define ZSTD_LEGACY_SUPPORT 0
#endif

#endif /* MICRO_OS_ZSTD_PORT_H */
