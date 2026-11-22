/*
 * Micro OS — the tests of bc and dc, the calculators of the port.
 *
 * The release is one library and one `main`: the name of the program in
 * `argv[0]` picks the engine (`bc` or `dc`), and the same sources are the
 * two images of `/bin` of the x86-64 port. The host build compiles those
 * sources with the headers of the host and runs the command as a process of
 * the test machine: the vector of arguments, the files of `-f`, descriptor 0,
 * the output and the status are the ones of a program of a Unix-like system,
 * and a crash of the calculator cannot take the test binary with it.
 *
 * The numbers are the lines of the release. The host machine of this tree
 * has no `bc` in its PATH (`which bc` answers nothing), so every expected
 * text is written here: a power of 31 digits, the arctangent of 1 with the
 * math library of `-l`, a base read and a base written, a square root, the
 * language of the release (`if`, `for`, `define`, an array, a string and the
 * two forms of a comment), and the RPN of `dc`.
 *
 * The build of the guest (`/bin/bc`, `/bin/dc`) is the same sources with the
 * library of the program and the port layer of `third_party/bc/port`; the
 * checks of the machine are `make x86-smoke` and `make x86-disk-smoke`.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The host build of the release: the same arguments as the program of /bin. */
#ifndef MICROOS_BC_HOST
#error "the test needs the path of the host build of bc (MICROOS_BC_HOST)"
#endif

#define OUTPUT_MAX 4096

/* A case: the arguments of the command, the text of descriptor 0 and the
 * lines the command has to print. */
struct sample {
    const char *name;
    const char *arguments[8];
    const char *input;
    const char *expected;
    int status;
};

/* The three cases of a script file need the name of the file in an argument,
 * which is a fact of the run and not of this table; the runner fills it. */
static char g_script_path[128];

static int read_all(int fd, char *buffer, size_t capacity)
{
    size_t length = 0;
    long count;

    while (length + 1 < capacity) {
        count = read(fd, buffer + length, capacity - 1 - length);
        if (count <= 0) {
            break;
        }
        length += (size_t)count;
    }

    buffer[length] = '\0';

    return (int)length;
}

/*
 * Run the command with the arguments of `case`, its input on descriptor 0
 * and both output descriptors in a file of /tmp. The child is a process of
 * the host that execs the calculator at once, so the kernel of the test is
 * not in the child and a fault of the release ends one case.
 */
static int run_case(const struct sample *test, char *output, size_t capacity)
{
    char input_path[] = "/tmp/microos-bc-in-XXXXXX";
    char output_path[] = "/tmp/microos-bc-out-XXXXXX";
    char *argv[9];
    int input_fd;
    int output_fd;
    int status = 0;
    pid_t child = -1;
    size_t index;

    input_fd = mkstemp(input_path);
    output_fd = mkstemp(output_path);
    KT_CHECK(input_fd >= 0);
    KT_CHECK(output_fd >= 0);
    if (input_fd < 0 || output_fd < 0) {
        return -1;
    }

    if (test->input != NULL && test->input[0] != '\0') {
        size_t length = strlen(test->input);

        KT_CHECK_INT_EQ(write(input_fd, test->input, length), (long)length);
    }
    KT_CHECK_INT_EQ(lseek(input_fd, 0, SEEK_SET), 0);
    KT_CHECK_INT_EQ(lseek(output_fd, 0, SEEK_SET), 0);

    for (index = 0; index < 8 && test->arguments[index] != NULL; index++) {
        argv[index] = (char *)test->arguments[index];
    }
    argv[index] = NULL;

    child = fork();
    KT_CHECK(child >= 0);

    if (child == 0) {
        (void)dup2(input_fd, STDIN_FILENO);
        (void)dup2(output_fd, STDOUT_FILENO);
        (void)dup2(output_fd, STDERR_FILENO);
        (void)close(input_fd);
        (void)close(output_fd);

        execv(MICROOS_BC_HOST, argv);

        _exit(127);
    }

    if (child > 0) {
        pid_t waited;

        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0);

        KT_CHECK(WIFEXITED(status));
        if (WIFEXITED(status)) {
            status = WEXITSTATUS(status);
        } else {
            status = -1;
        }
    }

    (void)lseek(output_fd, 0, SEEK_SET);
    (void)read_all(output_fd, output, capacity);

    (void)close(input_fd);
    (void)close(output_fd);
    (void)unlink(input_path);
    (void)unlink(output_path);

    return status;
}

/* The output of a case holds the expected text, and the status is the one the
 * release answers: 0 for a good expression, 1 for a run-time error. */
static void check_case(const struct sample *test)
{
    char output[OUTPUT_MAX];
    int status;

    ktest_begin(test->name);

    status = run_case(test, output, sizeof(output));

    KT_CHECK_STR_EQ(output, test->expected);
    KT_CHECK_INT_EQ(status, test->status);
}

static void test_arithmetic(void)
{
    static const struct sample cases[] = {
        {"bc: an expression of the command line",
         {"bc", "-e", "2^100", NULL},
         "",
         "1267650600228229401496703205376\n",
         0},
        {"bc: scale truncates a division", {"bc", "-e", "scale=5; 1/3", NULL}, "", ".33333\n", 0},
        {"bc: scale 0 truncates to the integer", {"bc", "-e", "scale=0; 7/2", NULL}, "", "3\n", 0},
        {"bc: ibase reads another base",
         {"bc", "-e", "ibase=16; obase=A; FF", NULL},
         "",
         "255\n",
         0},
        {"bc: obase writes another base", {"bc", "-e", "obase=2; 10", NULL}, "", "1010\n", 0},
        {"bc: sqrt at the scale of the line",
         {"bc", "-e", "scale=20; sqrt(2)", NULL},
         "",
         "1.41421356237309504880\n",
         0},
        {"bc: the math library of -l",
         {"bc", "-l", "-e", "scale=10; 4*a(1)", NULL},
         "",
         "3.1415926532\n",
         0},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        check_case(&cases[index]);
    }
}

static void test_language(void)
{
    static const struct sample cases[] = {
        {"bc: a function, a loop and an if",
         {"bc", "-e",
          "define sum(n) { auto i, s; s=0; for (i=1; i<=n; i++) s+=i; return (s) }\n"
          "sum(10)\n"
          "if (sum(10) == 55) print \"the sum is right\\n\"\n"
          "i=0\nwhile (i<3) i+=1\ni",
          NULL},
         "",
         "55\nthe sum is right\n3\n",
         0},
        {"bc: an array and a string",
         {"bc", "-e", "a[1]=10; a[2]=32; a[1]+a[2]; print \"hello\\n\"", NULL},
         "",
         "42\nhello\n",
         0},
        {"bc: the two comments of the language",
         {"bc", "-e", "/* a comment */\n# a line of a comment\n2+3\n", NULL},
         "",
         "5\n",
         0},
        {"bc: the POSIX mode of -s", {"bc", "-s", "-e", "scale=4; 2/3", NULL}, "", ".6666\n", 0},
        {"bc: a zero is a run-time error",
         {"bc", "-e", "1/0", NULL},
         "",
         "\nMath error: divide by 0\n"
         "    0: (main)\n\n",
         1},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        check_case(&cases[index]);
    }
}

static void test_stdin_and_files(void)
{
    char output[OUTPUT_MAX];
    int status;
    FILE *file;

    ktest_begin("bc: a script of descriptor 0");

    {
        static const struct sample stdin_case = {
            "bc: a script of descriptor 0", {"bc", NULL}, "2+3\nscale=2; 10/4\n", "5\n2.50\n", 0,
        };

        status = run_case(&stdin_case, output, sizeof(output));
        KT_CHECK_STR_EQ(output, stdin_case.expected);
        KT_CHECK_INT_EQ(status, stdin_case.status);
    }

    /* A file of the host with a name of this run, read by `-f` and by the
     * operand form of the release. The release reads descriptor 0 after its
     * files; the empty input is the end of that read. */
    snprintf(g_script_path, sizeof(g_script_path), "/tmp/microos-bc-script-XXXXXX");
    {
        int fd = mkstemp(g_script_path);

        KT_CHECK(fd >= 0);
        if (fd < 0) {
            return;
        }
        file = fdopen(fd, "w");
        KT_CHECK(file != NULL);
        if (file != NULL) {
            fprintf(file, "/* a file of the host */\nscale=3\n10/3\n");
            fclose(file);
        }
    }

    ktest_begin("bc: a script of a file with -f");
    {
        char *argv[] = {"bc", "-f", g_script_path, NULL};
        struct sample file_case = {"bc: a script of a file with -f", {NULL}, "", "3.333\n", 0};

        for (size_t i = 0; i < 3; i++) {
            file_case.arguments[i] = argv[i];
        }
        check_case(&file_case);
    }

    {
        char *argv[] = {"bc", g_script_path, NULL};
        struct sample file_case = {"bc: a script named as an operand", {NULL}, "", "3.333\n", 0};

        for (size_t i = 0; i < 2; i++) {
            file_case.arguments[i] = argv[i];
        }
        check_case(&file_case);
    }

    (void)unlink(g_script_path);
}

static void test_dc(void)
{
    static const struct sample cases[] = {
        {"dc: a sum on the stack", {"dc", "-e", "2 3 + p", NULL}, "", "5\n", 0},
        {"dc: a division at the default scale", {"dc", "-e", "10 4 / p", NULL}, "", "2\n", 0},
        {"dc: a scale and a division", {"dc", "-e", "5 k 1 3 / p", NULL}, "", ".33333\n", 0},
        {"dc: a power", {"dc", "-e", "2 10 ^ p", NULL}, "", "1024\n", 0},
        {"dc: a script of descriptor 0", {"dc", NULL}, "2 3 + p\n10 4 / p\n", "5\n2\n", 0},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        check_case(&cases[index]);
    }
}

void test_bc(void)
{
    test_arithmetic();
    test_language();
    test_stdin_and_files();
    test_dc();
}
