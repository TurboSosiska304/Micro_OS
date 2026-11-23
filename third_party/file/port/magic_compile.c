/*
 * Micro OS — the compiler of a magic database, as a tool of the build.
 *
 * The release compiles its Magdir files with `file -C`; this system does not
 * build that command (its option parser, its terminal and its `fork` are not
 * of the machine), so the build links the library and this small caller
 * instead. It is our file and not a copy of `src/file.c`.
 *
 *   magic_compile -C magic      writes magic.mgc beside the text
 *
 * The exit status is 0 when the database was written and 1 when the parser
 * refused a line; the sentence of the refusal is on stderr.
 */
#include <stdio.h>
#include <string.h>

#include "magic.h"

int main(int argc, char **argv)
{
	magic_t cookie;
	const char *error;
	int rc;

	if (argc != 3 || strcmp(argv[1], "-C") != 0) {
		(void)fprintf(stderr, "usage: magic_compile -C <magic>\n");
		return 2;
	}

	cookie = magic_open(MAGIC_CHECK);
	if (cookie == NULL) {
		(void)fprintf(stderr, "magic_compile: cannot open the library\n");
		return 2;
	}

	rc = magic_compile(cookie, argv[2]) == -1 ? 1 : 0;
	error = magic_error(cookie);
	if (rc != 0 && error != NULL)
		(void)fprintf(stderr, "magic_compile: %s\n", error);
	magic_close(cookie);

	return rc;
}
