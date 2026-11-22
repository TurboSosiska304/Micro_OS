/*
 * Micro OS — the check of the image in the boot loader (ADR 0020).
 *
 * The loader calls this before it copies a byte of the kernel and before it
 * turns the long mode on. The public key is compiled into the loader by the
 * build (sign_public_key.h), so the key travels with the boot and the image
 * cannot bring a key of its own. A refusal prints the sentence and the
 * loader halts: no part of the kernel runs on an image that did not pass.
 */

#include "sign_image.h"
#include "loader_io.h"
#include "sign_public_key.h"

/*
 * The default is the strict policy; a build that wants the development
 * behaviour names it in CMake (MICRO_OS_REQUIRE_SIGNATURE=OFF) and the define
 * reaches this file. An undefined macro is a mistake of the build, not a
 * reason to boot an unchecked image.
 */
#ifndef MICRO_OS_REQUIRE_SIGNATURE
#define MICRO_OS_REQUIRE_SIGNATURE 1
#endif

int loader_verify_image(const void *image, unsigned int size)
{
    const char *why = NULL;
    int verified;

    loader_serial_init();

    verified = microos_sign_image_verify(image, (size_t)size, loader_public_key, &why) == 0;
    if (!microos_sign_policy_continue(verified, MICRO_OS_REQUIRE_SIGNATURE
                                                    ? MICROOS_SIGN_POLICY_REQUIRE
                                                    : MICROOS_SIGN_POLICY_ALLOW)) {
        loader_puts("Micro OS: the image is refused: ");
        loader_puts(why != NULL ? why : "unknown reason");
        loader_puts("\r\n");
        return -1;
    }

    if (!verified) {
        /* The loader still verified; the policy says a person chose to run
         * the image anyway, and the line says what that costs. */
        loader_puts(microos_sign_policy_warning());
        loader_puts(" (");
        loader_puts(why != NULL ? why : "unknown reason");
        loader_puts(")\r\n");
        return 0;
    }

    loader_puts("Micro OS: image signature ok (sha-256, ed25519)\r\n");

    return 0;
}
