#include "kernel/version.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/port.h"

#define KSTR_(x) #x
#define KSTR(x) KSTR_(x)

const char *kernel_name(void)
{
    return KERNEL_NAME;
}

const char *kernel_version(void)
{
    return KSTR(KERNEL_VERSION_MAJOR) "." KSTR(KERNEL_VERSION_MINOR) "." KSTR(KERNEL_VERSION_PATCH)
        KERNEL_VERSION_SUFFIX;
}

const char *kernel_build_id(void)
{
    return __DATE__ " " __TIME__;
}

const char *kernel_compiler(void)
{
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

const char *kernel_arch(void)
{
    return k_port_arch();
}

struct version_parts {
    unsigned number[3];
    const char *suffix;
};

static const char *version_number(const char *text, unsigned *value)
{
    unsigned number = 0;

    while (*text >= '0' && *text <= '9') {
        number = number * 10u + (unsigned)(*text - '0');
        text++;
    }

    *value = number;

    return text;
}

static void version_parse(const char *text, struct version_parts *parts)
{
    const char *cursor = text != NULL ? text : "";
    unsigned i;

    for (i = 0; i < 3; i++) {
        parts->number[i] = 0;
    }
    parts->suffix = NULL;

    for (i = 0; i < 3; i++) {
        if (*cursor < '0' || *cursor > '9') {
            break;
        }
        cursor = version_number(cursor, &parts->number[i]);
        if (*cursor != '.') {
            break;
        }
        cursor++;
        if (*cursor < '0' || *cursor > '9') {
            break;
        }
    }

    if (*cursor != '\0') {
        parts->suffix = cursor;
    }
}

static int version_suffix_compare(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            unsigned left;
            unsigned right;

            a = version_number(a, &left);
            b = version_number(b, &right);
            if (left != right) {
                return left < right ? -1 : 1;
            }
            continue;
        }
        if (*a != *b) {
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        }
        a++;
        b++;
    }

    if (*a != '\0') {
        return 1;
    }
    if (*b != '\0') {
        return -1;
    }

    return 0;
}

int kernel_version_compare(const char *a, const char *b)
{
    struct version_parts left;
    struct version_parts right;
    unsigned i;

    version_parse(a, &left);
    version_parse(b, &right);

    for (i = 0; i < 3; i++) {
        if (left.number[i] != right.number[i]) {
            return left.number[i] < right.number[i] ? -1 : 1;
        }
    }

    /* A release has no suffix and is newer than any candidate of the same
       three numbers: 0.9.0 > 0.9.0-rc1. */
    if (left.suffix != NULL && right.suffix == NULL) {
        return -1;
    }
    if (left.suffix == NULL && right.suffix != NULL) {
        return 1;
    }
    if (left.suffix == NULL) {
        return 0;
    }

    return version_suffix_compare(left.suffix, right.suffix);
}
