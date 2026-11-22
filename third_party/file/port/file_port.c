/*
 * Micro OS — the two calls libmagic makes that this machine cannot answer.
 *
 * The release reads a named file through `magic.c` (`file_or_fd`), copies a
 * pipe into a temporary file and decompresses an input with a child process.
 * A program of this system has none of the three: `magic_buffer()` hands the
 * bytes it already read, the VFS has no temporary file and no `dup2` of one,
 * and there is no fork (the call of `compress.c` is behind `#if HAVE_FORK`,
 * which the port leaves undefined). The two symbols remain referenced by the
 * sources of the release, so the port answers them here instead of editing
 * the callers.
 */
#include "file.h"

#include <errno.h>
#include <unistd.h>

/*
 * The read of a descriptor that may be a pipe. A pipe of this system answers
 * `EAGAIN` while its producer is still there (ADR 0014), which the library of
 * a program reports as a negative result of `read()`; the wrapper waits the
 * condition out, as the port of zlib does, so a caller that reads a pipe gets
 * the bytes and not a short end of the stream.
 */
file_protected ssize_t
sread(int fd, void *buf, size_t n, int canbepipe)
{
	ssize_t r;

	(void)canbepipe;
	for (;;) {
		r = read(fd, buf, n);
		if (r != -1 || errno != EAGAIN)
			return r;
	}
}

/*
 * A pipe cannot become a file here: there is no temporary file to copy it to
 * and no `dup2` of a directory entry. The call is reached only by
 * `magic_descriptor()` of a pipe, which no program of the tree uses; the
 * sentence says so rather than a wrong answer.
 */
file_protected int
file_pipe2file(struct magic_set *ms, int fd, const void *startbuf,
    size_t nbytes)
{
	(void)fd;
	(void)startbuf;
	(void)nbytes;
	file_error(ms, 0, "a pipe is not a file of this system");
	return -1;
}
