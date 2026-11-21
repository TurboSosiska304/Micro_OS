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

int loader_verify_image(const void *image, unsigned int size)
{
    const char *why = NULL;

    loader_serial_init();

    if (microos_sign_image_verify(image, (size_t)size, loader_public_key, &why) != 0) {
        loader_puts("Micro OS: the image is refused: ");
        loader_puts(why != NULL ? why : "unknown reason");
        loader_puts("\r\n");
        return -1;
    }

    loader_puts("Micro OS: image signature ok (sha-256, ed25519)\r\n");

    return 0;
}
