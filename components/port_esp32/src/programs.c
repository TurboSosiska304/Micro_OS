/*
 * Micro OS — the programs of the board (ADR 0018).
 *
 * The programs are built by this component and embedded in the firmware as
 * blobs (components/port_esp32/CMakeLists.txt): there is no medium they could
 * be read from before MicroFS is mounted, and the point of the milestone is
 * that the bytes go through a file and the VFS and not through a variable of
 * the kernel. At the boot, after the volume is mounted, they are written to
 * /bin with the mode of a program and run through execve.
 *
 * The file is written only when its bytes are not already there. A board
 * boots many times, the flash has a finite number of writes, and a program
 * that did not change between two boots costs nothing.
 *
 * The self test calls port_esp32_programs_boot(): it installs hello, runs it
 * twice (the second run proves that the windows of the port can be used
 * again), and asks for a hand-made image of x86-64 to prove that the check of
 * the machine happens before a byte runs. The lines the program prints begin
 * with "PROGRAM:", which is what tools/device-test.sh looks for.
 */
#include <string.h>

#include "esp_heap_caps.h"

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

#include "port_esp32.h"

#define PROGRAM_DIR "/bin"
#define PROGRAM_PATH "/bin/hello"
#define CAT_PATH "/bin/cat"
#define FOREIGN_PATH "/bin/foreign"

/* A program is a file somebody may run: without the three `x` bits execve
 * answers EPERM and the image proves nothing about the machine. */
#define PROGRAM_MODE 0755u

/* The images the build made of programs/hello.c and programs/cat.c; the
 * symbols of the blobs. */
extern const k_u8_t _binary_hello_mox_start[];
extern const k_u8_t _binary_hello_mox_end[];
extern const k_u8_t _binary_cat_mox_start[];
extern const k_u8_t _binary_cat_mox_end[];

/*
 * A minimal, well-formed image of the *other* machine: MOX1 version 2, an
 * entry at 32, four bytes of text and the arch 0 of x86-64. The kernel
 * compares the machine before it asks the port to enter anything, so this
 * file is ENOEXEC and nothing of it runs. It is a constant of the test and
 * not a program: the four bytes would be an invalid instruction here anyway.
 */
static const k_u8_t g_foreign_image[] = {
    'M',  'O',  'X',  '1',  /* magic */
    2,    0,    0,    0,    /* version 2 */
    0,    0,    0,    0,    /* flags */
    32,   0,    0,    0,    /* entry */
    4,    0,    0,    0,    /* text_size */
    0,    0,    0,    0,    /* data_size */
    0,    0,    0,    0,    /* bss_size */
    0,    0,    0,    0,    /* arch: x86-64 */
    0x90, 0x90, 0x90, 0x90, /* text: not Xtensa code */
};

/* Whether the file already holds exactly these bytes. A different size is
 * the fast answer; the same size is compared in pieces of the stack. */
static bool file_is(const char *path, const k_u8_t *image, k_size_t size)
{
    struct vfs_file file;
    struct vfs_stat info;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t buffer[128];
    k_size_t done = 0;

    if (vfs_stat(path, &info) != 0 || info.type != 1 || (k_size_t)info.size != size) {
        return false;
    }

    if (vfs_open(path, VFS_O_RDONLY, &file, priv) != 0) {
        return false;
    }

    while (done < size) {
        k_size_t chunk = (size - done < sizeof(buffer)) ? size - done : sizeof(buffer);
        int got = vfs_read(&file, buffer, chunk);

        if (got != (int)chunk || memcmp(buffer, image + done, chunk) != 0) {
            (void)vfs_close(&file);
            return false;
        }

        done += chunk;
    }

    (void)vfs_close(&file);

    return true;
}

/*
 * Write an image to a file of /bin, and make it executable. The file is
 * created through the VFS and not through the filesystem: the adapter of
 * MicroFS is the only place that knows which number of the filesystem means
 * what, and the boot is not that place.
 */
static int program_install(const char *path, const k_u8_t *image, k_size_t size)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    int rc;

    if (file_is(path, image, size)) {
        /* The bytes are there; the mode can still have been lost. */
        if (vfs_chmod(path, PROGRAM_MODE) != 0) {
            kerror("esp32: cannot make %s executable", path);
            return -1;
        }

        kinfo("esp32: %s is already in the volume (%u bytes)", path, (unsigned)size);
        return 0;
    }

    rc = vfs_mkdir_p(PROGRAM_DIR);
    if (rc != 0) {
        kerror("esp32: cannot make %s (%s)", PROGRAM_DIR, vfs_strerror(rc));
        return -1;
    }

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        kerror("esp32: cannot create %s (%s)", path, vfs_strerror(rc));
        return -1;
    }

    rc = vfs_write(&file, image, size);
    (void)vfs_close(&file);

    if (rc != (int)size) {
        kerror("esp32: %s took %d byte(s) of %u", path, rc, (unsigned)size);
        return -1;
    }

    if (vfs_chmod(path, PROGRAM_MODE) != 0) {
        kerror("esp32: cannot make %s executable", path);
        return -1;
    }

    kinfo("esp32: %s is written (%u bytes)", path, (unsigned)size);

    return 0;
}

/*
 * Run the program that proves the machine: Xtensa code from the instruction
 * RAM, the vectors in DRAM, the door of the port and exit() waking the caller
 * of execve. The vector has three words so that the program can print them
 * and a person reading the console can see that nothing was faked.
 */
static int program_run(void)
{
    char *first[] = {(char *)"hello", (char *)"one", (char *)"two", NULL};
    char *second[] = {(char *)"/bin/hello", NULL};
    int rc;

    kinfo("esp32: running %s through execve", PROGRAM_PATH);

    rc = ksys_execve(PROGRAM_PATH, first);
    if (rc != 0) {
        kerror("esp32: %s did not run (%s, %d)", PROGRAM_PATH, vfs_strerror(rc), rc);
        return -1;
    }

    /* The task of the first program is gone and the windows are free: the
     * same file starts again, with a vector of one word. */
    rc = ksys_execve(PROGRAM_PATH, second);
    if (rc != 0) {
        kerror("esp32: the second run of %s did not run (%s, %d)", PROGRAM_PATH, vfs_strerror(rc),
               rc);
        return -1;
    }

    return 0;
}

/* The machine is checked before the port is asked to place the image. */
static int program_refuse_foreign(void)
{
    char *argv[] = {(char *)"foreign", NULL};
    int rc;

    if (program_install(FOREIGN_PATH, g_foreign_image, sizeof(g_foreign_image)) != 0) {
        return -1;
    }

    rc = ksys_execve(FOREIGN_PATH, argv);
    if (rc != KERR_NOEXEC) {
        kerror("esp32: an x86-64 image answered %d instead of ENOEXEC", rc);
        return -1;
    }

    kinfo("esp32: an x86-64 image is ENOEXEC, no byte of it ran");

    return 0;
}

int port_esp32_programs_boot(void)
{
    k_size_t hello_size = (k_size_t)(_binary_hello_mox_end - _binary_hello_mox_start);
    k_size_t cat_size = (k_size_t)(_binary_cat_mox_end - _binary_cat_mox_start);

    if (program_install(PROGRAM_PATH, _binary_hello_mox_start, hello_size) != 0) {
        return -1;
    }

    /* The reader of a file is not run at the boot - it would print the whole
     * log - but it is in /bin for the shell and for a person: a path, a read
     * and a write through the door are the read side of the machine. */
    if (program_install(CAT_PATH, _binary_cat_mox_start, cat_size) != 0) {
        return -1;
    }

    if (program_run() != 0) {
        return -1;
    }

    if (program_refuse_foreign() != 0) {
        return -1;
    }

    /* The text window is reserved from the heap at the boot; the number is
     * what the IRAM allocations have left (the pools below the window and the
     * exec pools of the chip), and it is printed so that a person can see that
     * the window did not disappear from the heap unnoticed. */
    kinfo("esp32: the programs of the board are up, the IRAM heap has %u byte(s)",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC));

    return 0;
}
