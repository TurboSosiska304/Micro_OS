/*
 * Micro OS — the two analysis passes of PCRE2 that a program image leaves out.
 *
 * A program is a MOX1 file of at most KSYS_MAX_IO (64 KiB, kernel/ksys.h),
 * and the full 10.42 compile and match engine is about 65 KiB before the
 * program itself. The two functions here are optimizations of the compiler
 * and not parts of the meaning of a pattern:
 *
 *   auto_possessify() rewrites a greedy repeat followed by a character that
 *   cannot match the repeat into an atomic one;
 *   study() computes the bitmap of the first code units and the minimum
 *   length of a subject, which the matcher uses to skip a subject that is
 *   too short or to start at a place that cannot match.
 *
 * Both only make a match faster; neither changes whether a pattern matches.
 * Their callers in pcre2_compile.c handle a zero answer as success and carry
 * on with the unoptimized opcodes, and the matcher has the same behaviour
 * when the bitmap and the minimum length are absent. `pcre2grep` and the
 * unit test run the same engine with these two stubs, so the library that
 * the image carries is the library the test checks.
 *
 * The name of the file is a local addition and not an upstream one:
 * third_party/pcre2/README.md records it with the rest of the configuration.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcre2_internal.h"

int PRIV(auto_possessify)(PCRE2_UCHAR *code, const compile_block *cb)
{
    (void)code;
    (void)cb;

    return 0;
}

int PRIV(study)(pcre2_real_code *re)
{
    (void)re;

    return 0;
}
