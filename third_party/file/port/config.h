/*
 * Micro OS — the configuration of libmagic (third_party/file).
 *
 * The release is built with `configure`, which writes a config.h of the
 * machine it found. This machine is not one of the systems of autoconf: it
 * has the POSIX families of `components/libc/user` and no process, no pipe of
 * its own, no `mmap` and no floating point in a program. The file is the
 * answer of the port, written by hand and force-included through
 * `HAVE_CONFIG_H`; the upstream `configure.ac` stays the reference of what a
 * macro means.
 *
 * A macro of autoconf is *defined* when the answer is yes and left undefined
 * when it is no: file.h and the sources ask with `#ifdef`, so a macro that is
 * defined to 0 would still read as "there is one".
 */
#ifndef MICRO_OS_FILE_CONFIG_H
#define MICRO_OS_FILE_CONFIG_H

/* The version of the release, as `configure` would have written it. */
#define PACKAGE_VERSION "5.45"
#define VERSION "5.45"

/*
 * The ELF reader is compiled in: it is the description of every program of
 * this system and of the programs of the host. `ELFCORE` is what the release
 * assumes whenever the ELF reader is there (readelf.c names the flags of a
 * core file outside its own guard), and it costs a `pread` the port brings.
 */
#define BUILTIN_ELF 1
#define ELFCORE 1

/*
 * There is no fork and no pipe of a program: `file_buffer()` therefore never
 * tries the decompression of `compress.c` (the call is behind `#if HAVE_FORK`)
 * and the port answers its two remaining calls (`sread`, `file_pipe2file`) in
 * `file_port.c`. `-z` is the program's own, through zlib, for gzip.
 */
#define HAVE_FORK 0

/* A static archive has no exported symbols; file.h asks with `#if`. */
#define HAVE_VISIBILITY 0

/* The headers of this system that the release includes. */
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDARG_H 1
#define HAVE_CTYPE_H 1
#define HAVE_REGEX_H 1
#define HAVE_TIME_H 1
#define HAVE_LOCALE_H 1
#define HAVE_ASSERT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1

/*
 * The functions of the C library the release looks for. `getline`,
 * `strcasestr` and `strndup` are in the library of a program; the others are
 * the fallbacks of `src/` (asprintf, vasprintf, dprintf, fmtcheck, strlcat,
 * strlcpy, the `*time_r` family), which the build compiles with the library.
 */
#define HAVE_GETLINE 1
#define HAVE_STRCASESTR 1
#define HAVE_STRNDUP 1
#define HAVE_STRERROR 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOULL 1
#define HAVE_QSORT 1

/*
 * No floating point in a program of this system (`-mgeneral-regs-only`): the
 * magic types `float` and `double` are removed from the sources with
 * MICRO_OS_NO_FLOAT and from the database by tools/magic_subset.py. The two
 * macros stay here so that a reader sees the reason with the switch.
 */
#define MICRO_OS_NO_FLOAT 1

/*
 * The formatter of a program has no `*` width and no `*` precision, and there
 * is no `sscanf`: the two hunks of the release that use them are named by
 * these macros, and `docs/porting-programs.md` lists the gap.
 */
#define MICRO_OS_NO_STAR_PRECISION 1
#define MICRO_OS_NO_SCANF 1

/*
 * The bytes of a file the encoding test looks at. The release names 64 KiB,
 * which costs a block of 512 KiB here: `file_unichar_t` is an `unsigned long`
 * and the conversion allocates one per byte. The arena of a program is a
 * megabyte of the format and the database of this port is a third of it, so
 * the port looks at 16 KiB - a text file shows its encoding long before the
 * end - and the memory is the answer of the machine, not a taste.
 */
#define MICRO_OS_ENCODING_MAX (16u * 1024u)

#endif /* MICRO_OS_FILE_CONFIG_H */
