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
    return KSTR(KERNEL_VERSION_MAJOR) "." KSTR(KERNEL_VERSION_MINOR) "." KSTR(KERNEL_VERSION_PATCH);
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
