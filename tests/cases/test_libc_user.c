/*
 * Micro OS — the tests of the POSIX edge of the library of a program.
 *
 * The library of a program (`components/libc/user`) is not the library of the
 * kernel: it runs in the window of a program and reaches the kernel through
 * the door of the system calls. The functions of this file need that door for
 * one thing only - the tree of the fixture, which `libc_user_door_shim.c`
 * answers - and are otherwise portable C: the environment, the canonical name
 * of a path, the reverse of `strftime`, the options of a line, the reader of
 * a format and the names of a pattern. The binary is a test of its own, in
 * the way `userland` is, because its include path is the one of a program.
 *
 * The door of the real machine is covered by `make x86-smoke`, where a
 * program of the machine calls the kernel; the scanner and the streams of
 * `/bin/streams` are the second half of that gate.
 */
#include "ktest.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The fixture of the tree of the shim: names the tests of `realpath` and
 * `glob` ask about. */
void libc_user_door_set_input(const char *text);

void test_libc_user(void);

/* --- the environment ---------------------------------------------------- */

static void test_environment(void)
{
    char *words[] = {"HOME=/root", "PATH=/bin", "EMPTY=", NULL};
    char own[] = "OWN=one";
    int status = -1;

    environ = words;
    KT_CHECK_STR_EQ(getenv("HOME"), "/root");
    KT_CHECK_STR_EQ(getenv("PATH"), "/bin");
    KT_CHECK_STR_EQ(getenv("EMPTY"), "");
    KT_CHECK(getenv("MISSING") == NULL);
    KT_CHECK(getenv("HOM") == NULL); /* a prefix is not a name */

    /* A name that is there is not touched when the caller says no. */
    KT_CHECK_INT_EQ(setenv("HOME", "/home/guest", 0), 0);
    KT_CHECK_STR_EQ(getenv("HOME"), "/root");

    KT_CHECK_INT_EQ(setenv("HOME", "/home/guest", 1), 0);
    KT_CHECK_STR_EQ(getenv("HOME"), "/home/guest");

    KT_CHECK_INT_EQ(setenv("NEW", "value", 0), 0);
    KT_CHECK_STR_EQ(getenv("NEW"), "value");
    KT_CHECK_INT_EQ(setenv("NEW", "other", 0), 0);
    KT_CHECK_STR_EQ(getenv("NEW"), "value");
    KT_CHECK_INT_EQ(setenv("NEW", "other", 1), 0);
    KT_CHECK_STR_EQ(getenv("NEW"), "other");

    KT_CHECK_INT_EQ(unsetenv("NEW"), 0);
    KT_CHECK(getenv("NEW") == NULL);
    KT_CHECK_INT_EQ(unsetenv("NEW"), 0); /* a name that is not there is fine */

    /* The words the program was entered with are still there. */
    KT_CHECK_STR_EQ(getenv("PATH"), "/bin");

    /* `putenv` takes the word of the caller and does not copy it. */
    KT_CHECK_INT_EQ(putenv(own), 0);
    KT_CHECK_STR_EQ(getenv("OWN"), "one");
    KT_CHECK_INT_EQ(putenv(own), 0); /* a second call replaces the same word */
    KT_CHECK_STR_EQ(getenv("OWN"), "one");
    KT_CHECK_INT_EQ(unsetenv("OWN"), 0);
    KT_CHECK(getenv("OWN") == NULL);

    /* The vector of the program was not the one of the library: the library
     * adopted it and the caller's array is not written by the calls. */
    KT_CHECK(environ != words);
    KT_CHECK_STR_EQ(words[0], "HOME=/root");

    /* The bad shapes of a name. */
    errno = 0;
    KT_CHECK_INT_EQ(setenv("", "x", 1), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(setenv("A=B", "x", 1), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(setenv(NULL, "x", 1), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(setenv("A", NULL, 1), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);

    errno = 0;
    KT_CHECK_INT_EQ(unsetenv(""), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(unsetenv("A=B"), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(unsetenv(NULL), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);

    errno = 0;
    KT_CHECK_INT_EQ(putenv(NULL), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(putenv((char *)"NOEQUALS"), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);
    errno = 0;
    KT_CHECK_INT_EQ(putenv((char *)"=value"), -1);
    KT_CHECK_INT_EQ(errno, EINVAL);

    /* An environment of no word at all is a vector of one NULL, and the first
     * `setenv` grows it. */
    environ = NULL;
    KT_CHECK_INT_EQ(setenv("ONLY", "one", 1), 0);
    KT_CHECK_STR_EQ(getenv("ONLY"), "one");
    KT_CHECK_INT_EQ(unsetenv("ONLY"), 0);

    /* The child of a fork has the memory of the parent, so it reads the same
     * vector: the copy on write of the port carries the words the parent
     * grew. */
    KT_CHECK_INT_EQ(setenv("SHARED", "yes", 1), 0);
#ifndef MICROOS_TSAN
    {
        pid_t child = fork();

        if (child == 0) {
            _exit((getenv("SHARED") != NULL && strcmp(getenv("SHARED"), "yes") == 0) ? 0 : 1);
        }
        KT_CHECK(child > 0);
        KT_CHECK_INT_EQ(waitpid(child, &status, 0), child);
        KT_CHECK_INT_EQ(status, 0);
    }
#endif

    /* Put the vector back for the rest of the file: the next call adopts the
     * array of the test again. */
    environ = words;
    unsetenv("SHARED");
}

/* --- the canonical name of a path --------------------------------------- */

static void test_realpath(void)
{
    char buffer[PATH_MAX];
    char *allocated;
    char too_long[PATH_MAX + 64];

    KT_CHECK_STR_EQ(realpath("/etc//setting", buffer), "/etc/setting");
    KT_CHECK_STR_EQ(realpath("/etc/./setting", buffer), "/etc/setting");
    KT_CHECK_STR_EQ(realpath("/etc/../etc/setting", buffer), "/etc/setting");
    KT_CHECK_STR_EQ(realpath("etc/setting", buffer), "/etc/setting");
    KT_CHECK_STR_EQ(realpath("/", buffer), "/");
    KT_CHECK_STR_EQ(realpath("//", buffer), "/");
    KT_CHECK_STR_EQ(realpath("/etc/", buffer), "/etc");
    KT_CHECK_STR_EQ(realpath("/tmp/../etc/./../etc/setting", buffer), "/etc/setting");

    allocated = realpath("/bin/ls", NULL);
    KT_CHECK(allocated != NULL);
    if (allocated != NULL) {
        KT_CHECK_STR_EQ(allocated, "/bin/ls");
        free(allocated);
    }

    errno = 0;
    KT_CHECK(realpath("/etc/missing", buffer) == NULL);
    KT_CHECK_INT_EQ(errno, ENOENT);

    errno = 0;
    KT_CHECK(realpath("", buffer) == NULL);
    KT_CHECK_INT_EQ(errno, ENOENT);

    errno = 0;
    KT_CHECK(realpath(NULL, buffer) == NULL);
    KT_CHECK_INT_EQ(errno, EINVAL);

    memset(too_long, 'a', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1] = '\0';
    errno = 0;
    KT_CHECK(realpath(too_long, buffer) == NULL);
    KT_CHECK_INT_EQ(errno, ENAMETOOLONG);
}

/* --- the reverse of strftime -------------------------------------------- */

static const struct tm g_moment = {.tm_sec = 7,
                                   .tm_min = 42,
                                   .tm_hour = 15,
                                   .tm_mday = 5,
                                   .tm_mon = 2,
                                   .tm_year = 126,
                                   .tm_wday = 4,
                                   .tm_yday = 63,
                                   .tm_isdst = 0};

static void check_round_trip(const char *format)
{
    char text[128];
    struct tm parsed;
    char *rest;

    memset(&parsed, 0, sizeof(parsed));
    parsed.tm_mday = 99;

    KT_CHECK(strftime(text, sizeof(text), format, &g_moment) > 0);
    rest = strptime(text, format, &parsed);
    KT_CHECK(rest != NULL);
    if (rest != NULL) {
        KT_CHECK_INT_EQ(*rest, '\0');
    }
}

static void test_strptime(void)
{
    char text[128];
    struct tm parsed;
    struct tm back;
    char *rest;

    check_round_trip("%Y-%m-%d %H:%M:%S");
    check_round_trip("%F %T");
    check_round_trip("%a %b %e %H:%M:%S %Y");
    check_round_trip("%c");
    check_round_trip("%d/%m/%y");
    check_round_trip("%D");
    check_round_trip("%I:%M %p");
    check_round_trip("%r");
    check_round_trip("%R");
    check_round_trip("%j %Y");
    check_round_trip("%C%y-%m-%d");
    check_round_trip("%u %w");
    check_round_trip("%Z %z");

    /* The fields of the format come back, and the ones it does not name are
     * left as the caller had them. */
    memset(&parsed, 0, sizeof(parsed));
    parsed.tm_sec = 11;
    KT_CHECK(strptime("2026-03-05 15:42", "%Y-%m-%d %H:%M", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_year, 126);
    KT_CHECK_INT_EQ(parsed.tm_mon, 2);
    KT_CHECK_INT_EQ(parsed.tm_mday, 5);
    KT_CHECK_INT_EQ(parsed.tm_hour, 15);
    KT_CHECK_INT_EQ(parsed.tm_min, 42);
    KT_CHECK_INT_EQ(parsed.tm_sec, 11);

    /* The hour of the twelve-hour clock with the two words of the day. */
    KT_CHECK(strptime("12:05 AM", "%I:%M %p", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_hour, 0);
    KT_CHECK(strptime("12:05 PM", "%I:%M %p", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_hour, 12);
    KT_CHECK(strptime("07:05 pm", "%I:%M %p", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_hour, 19);

    /* The names without the case of a letter, and the short name of a long
     * conversion. */
    KT_CHECK(strptime("thursday march", "%A %B", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_wday, 4);
    KT_CHECK_INT_EQ(parsed.tm_mon, 2);
    KT_CHECK(strptime("Thu Mar", "%a %b", &parsed) != NULL);

    /* The century and the year of two digits together. */
    KT_CHECK(strptime("2026", "%C%y", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_year, 126);
    KT_CHECK(strptime("69", "%y", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_year, 69);
    KT_CHECK(strptime("68", "%y", &parsed) != NULL);
    KT_CHECK_INT_EQ(parsed.tm_year, 168);

    /* The text after the conversion is the rest of the line. */
    rest = strptime("2026 the year", "%Y", &parsed);
    KT_CHECK(rest != NULL);
    if (rest != NULL) {
        KT_CHECK_STR_EQ(rest, " the year");
    }

    /* The shapes that are not a moment. */
    KT_CHECK(strptime("nonsense", "%Y", &parsed) == NULL);
    KT_CHECK(strptime("2026-13-01", "%Y-%m-%d", &parsed) == NULL);
    KT_CHECK(strptime("2026", "%Y-%m", &parsed) == NULL);
    errno = 0;
    KT_CHECK(strptime("2026", "%Q", &parsed) == NULL);
    KT_CHECK_INT_EQ(errno, EINVAL);
    KT_CHECK(strptime(NULL, "%Y", &parsed) == NULL);
    KT_CHECK(strptime("2026", NULL, &parsed) == NULL);
    KT_CHECK(strptime("2026", "%Y", NULL) == NULL);

    /* A moment of the formatter is the moment of the reader. */
    memset(&back, 0, sizeof(back));
    (void)strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &g_moment);
    KT_CHECK(strptime(text, "%Y-%m-%d %H:%M:%S", &back) != NULL);
    KT_CHECK_INT_EQ(back.tm_year, g_moment.tm_year);
    KT_CHECK_INT_EQ(back.tm_mon, g_moment.tm_mon);
    KT_CHECK_INT_EQ(back.tm_mday, g_moment.tm_mday);
    KT_CHECK_INT_EQ(back.tm_hour, g_moment.tm_hour);
    KT_CHECK_INT_EQ(back.tm_min, g_moment.tm_min);
    KT_CHECK_INT_EQ(back.tm_sec, g_moment.tm_sec);
}

/* --- the long options of a line ----------------------------------------- */

static int g_flag_s;

static void test_long_options(void)
{
    static const struct option table[] = {
        {"all", no_argument, NULL, 'a'},
        {"file", required_argument, NULL, 'f'},
        {"level", optional_argument, NULL, 'l'},
        {"verbose", no_argument, &g_flag_s, 7},
        {NULL, 0, NULL, 0},
    };

    {
        char *argv[] = {"tool", "--all", "--file=one", "--lev=2", "--verbose", NULL};
        char *const *words = argv;
        int index = -1;

        optind = 1;
        opterr = 0;
        g_flag_s = 0;

        KT_CHECK_INT_EQ(getopt_long(5, words, "af:l:", table, &index), 'a');
        KT_CHECK_INT_EQ(index, 0);

        KT_CHECK_INT_EQ(getopt_long(5, words, "af:l:", table, &index), 'f');
        KT_CHECK_STR_EQ(optarg, "one");
        KT_CHECK_INT_EQ(index, 1);

        KT_CHECK_INT_EQ(getopt_long(5, words, "af:l:", table, &index), 'l');
        KT_CHECK_STR_EQ(optarg, "2");
        KT_CHECK_INT_EQ(index, 2);

        KT_CHECK_INT_EQ(getopt_long(5, words, "af:l:", table, &index), 0);
        KT_CHECK_INT_EQ(g_flag_s, 7);
        KT_CHECK_INT_EQ(index, 3);

        KT_CHECK_INT_EQ(getopt_long(5, words, "af:l:", table, &index), -1);
        KT_CHECK_INT_EQ(optind, 5);
    }

    /* The value in the next word, and the end of the options. */
    {
        char *argv[] = {"tool", "--file", "two", "--", "--file", NULL};
        char *const *words = argv;

        optind = 1;
        opterr = 0;
        KT_CHECK_INT_EQ(getopt_long(5, words, "f:", table, NULL), 'f');
        KT_CHECK_STR_EQ(optarg, "two");
        KT_CHECK_INT_EQ(getopt_long(5, words, "f:", table, NULL), -1);
        KT_CHECK_INT_EQ(optind, 4);
    }

    /* A name nobody declared, a prefix two names share, a value an option
     * does not take, and a value that is not there. */
    {
        static const struct option two[] = {
            {"quiet", no_argument, NULL, 'q'},
            {"quote", no_argument, NULL, 'u'},
            {NULL, 0, NULL, 0},
        };
        char *unknown[] = {"tool", "--nope", NULL};
        char *ambiguous[] = {"tool", "--qu", NULL};
        char *extra[] = {"tool", "--all=1", NULL};
        char *missing[] = {"tool", "--file", NULL};

        optind = 1;
        opterr = 0;
        KT_CHECK_INT_EQ(getopt_long(2, (char *const *)unknown, "af:l:", table, NULL), '?');

        optind = 1;
        KT_CHECK_INT_EQ(getopt_long(2, (char *const *)ambiguous, "qu", two, NULL), '?');

        optind = 1;
        KT_CHECK_INT_EQ(getopt_long(2, (char *const *)extra, "af:l:", table, NULL), '?');

        optind = 1;
        KT_CHECK_INT_EQ(getopt_long(2, (char *const *)missing, "af:l:", table, NULL), '?');
    }

    /* `getopt_long_only` reads a word of one dash as a name and falls back to
     * the letters of `getopt`. */
    {
        char *argv[] = {"tool", "-all", "-q", "-f", "x", NULL};
        char *const *words = argv;

        optind = 1;
        opterr = 0;
        KT_CHECK_INT_EQ(getopt_long_only(5, words, "qf:", table, NULL), 'a');
        KT_CHECK_INT_EQ(getopt_long_only(5, words, "qf:", table, NULL), 'q');
        KT_CHECK_INT_EQ(getopt_long_only(5, words, "qf:", table, NULL), 'f');
        KT_CHECK_STR_EQ(optarg, "x");
        KT_CHECK_INT_EQ(getopt_long_only(5, words, "qf:", table, NULL), -1);
    }

    /* An operand stops the walk, and `getopt` and the long reader agree. */
    {
        char *argv[] = {"tool", "name", "--all", NULL};
        char *const *words = argv;

        optind = 1;
        opterr = 0;
        KT_CHECK_INT_EQ(getopt_long(3, words, "a", table, NULL), -1);
        KT_CHECK_INT_EQ(optind, 1);
    }
}

/* --- the reader of a format --------------------------------------------- */

static int call_vsscanf(const char *text, const char *format, ...)
{
    va_list args;
    int answer;

    va_start(args, format);
    answer = vsscanf(text, format, args);
    va_end(args);

    return answer;
}

static int call_vfscanf(FILE *stream, const char *format, ...)
{
    va_list args;
    int answer;

    va_start(args, format);
    answer = vfscanf(stream, format, args);
    va_end(args);

    return answer;
}

static int call_vscanf(const char *format, ...)
{
    va_list args;
    int answer;

    va_start(args, format);
    answer = vscanf(format, args);
    va_end(args);

    return answer;
}

/* The compiler knows the C library of the host by name and refuses a NULL
 * format at the call; these names hide the NULL from it so that the reader of
 * the library is the one that answers. */
static const char *null_text(void)
{
    return NULL;
}

static FILE *null_stream(void)
{
    return NULL;
}

/* The same for the guard of a NULL text: a call through a pointer is not the
 * builtin of the compiler, and the function of the library is the one that
 * decides. */
static int scan_null_text(const char *text)
{
    int (*scan)(const char *, const char *, ...) = sscanf;
    int value = 0;

    return scan(text, "%d", &value);
}

static int scan_null_stream(FILE *stream)
{
    int (*scan)(FILE *, const char *, ...) = fscanf;
    int value = 0;

    return scan(stream, "%d", &value);
}

static int scan_null_format(const char *text)
{
    int (*scan)(const char *, const char *, ...) = sscanf;
    int value = 0;

    return scan(text, null_text(), &value);
}

static void test_scanf(void)
{
    int first = 0;
    int second = 0;
    unsigned int hex = 0;
    unsigned int octal = 0;
    char word[32];
    char bytes[8];
    int used = -1;
    void *pointer = NULL;
    signed char small = 0;
    short half = 0;
    long long wide = 0;

    KT_CHECK_INT_EQ(sscanf("42 -7 1f", "%d %d %x", &first, &second, &hex), 3);
    KT_CHECK_INT_EQ(first, 42);
    KT_CHECK_INT_EQ(second, -7);
    KT_CHECK_INT_EQ(hex, 0x1f);

    KT_CHECK_INT_EQ(sscanf("0x1f 077 12", "%x %o %i", &hex, &octal, &first), 3);
    KT_CHECK_INT_EQ(hex, 0x1f);
    KT_CHECK_INT_EQ(octal, 077);
    KT_CHECK_INT_EQ(first, 12);

    KT_CHECK_INT_EQ(sscanf(" 12", "%d", &first), 1);
    KT_CHECK_INT_EQ(first, 12);

    /* The width of a field and the rest of the text after it. */
    KT_CHECK_INT_EQ(sscanf("12345", "%3d%d", &first, &second), 2);
    KT_CHECK_INT_EQ(first, 123);
    KT_CHECK_INT_EQ(second, 45);

    /* The `*` drops an answer but reads it. */
    KT_CHECK_INT_EQ(sscanf("12 34", "%*d %d", &first), 1);
    KT_CHECK_INT_EQ(first, 34);

    /* `%n` is the place of the reader and is not a conversion of the answer. */
    KT_CHECK_INT_EQ(sscanf("12ab", "%d%n", &first, &used), 1);
    KT_CHECK_INT_EQ(first, 12);
    KT_CHECK_INT_EQ(used, 2);

    /* `%c` takes its bytes and no space, `%s` skips the space. */
    KT_CHECK_INT_EQ(sscanf("abc", "%2c", bytes), 1);
    KT_CHECK_INT_EQ(bytes[0], 'a');
    KT_CHECK_INT_EQ(bytes[1], 'b');
    KT_CHECK_INT_EQ(sscanf("   abcdef", "%5s", word), 1);
    KT_CHECK_STR_EQ(word, "abcde");

    /* The lengths of the numbers. */
    KT_CHECK_INT_EQ(sscanf("9 -300 123456789", "%hhd %hd %lld", &small, &half, &wide), 3);
    KT_CHECK_INT_EQ(small, 9);
    KT_CHECK_INT_EQ(half, -300);
    KT_CHECK_INT_EQ(wide, 123456789ll);

    KT_CHECK_INT_EQ(sscanf("0x10", "%p", &pointer), 1);
    KT_CHECK((uintptr_t)pointer == 0x10u);

    KT_CHECK_INT_EQ(sscanf("100%", "%d%%", &first), 1);
    KT_CHECK_INT_EQ(first, 100);

#ifdef ULIBC_WITH_FLOAT
    {
        double real = 0.0;

        KT_CHECK_INT_EQ(sscanf("3.5 -2e3", "%lf %lg", &real, &real), 2);
        KT_CHECK_INT_EQ(sscanf("1.25", "%lf", &real), 1);
        KT_CHECK(real > 1.24 && real < 1.26);
    }
#endif

    /* The end of the input is EOF before any conversion, and a mismatch is 0. */
    KT_CHECK_INT_EQ(sscanf("", "%d", &first), EOF);
    KT_CHECK_INT_EQ(sscanf("abc", "%d", &first), 0);
    KT_CHECK_INT_EQ(sscanf("12", "%d %d", &first, &second), 1);
    KT_CHECK_INT_EQ(sscanf(" abc", " %d", &first), 0);

    /* The stream of the fixture: the same reader over `fgetc`, and the end of
     * the file after the last conversion. */
    libc_user_door_set_input("7 alpha 2.5 rest\n");
    KT_CHECK_INT_EQ(fscanf(stdin, "%d %31s", &first, word), 2);
    KT_CHECK_INT_EQ(first, 7);
    KT_CHECK_STR_EQ(word, "alpha");
    KT_CHECK_INT_EQ(call_vfscanf(stdin, "%31s", word), 1);
    KT_CHECK_STR_EQ(word, "2.5");
    KT_CHECK_INT_EQ(fscanf(stdin, "%31s", word), 1);
    KT_CHECK_STR_EQ(word, "rest");
    KT_CHECK_INT_EQ(fscanf(stdin, "%31s", word), EOF);

    libc_user_door_set_input("31");
    KT_CHECK_INT_EQ(scanf("%d", &first), 1);
    KT_CHECK_INT_EQ(first, 31);

    libc_user_door_set_input("41");
    KT_CHECK_INT_EQ(call_vscanf("%d", &first), 1);
    KT_CHECK_INT_EQ(first, 41);

    KT_CHECK_INT_EQ(call_vsscanf("52 x", "%d %31s", &first, word), 2);
    KT_CHECK_INT_EQ(first, 52);
    KT_CHECK_STR_EQ(word, "x");

    /* A NULL text or stream is the end of the input, not a fault. */
    KT_CHECK_INT_EQ(scan_null_text(null_text()), EOF);
    KT_CHECK_INT_EQ(scan_null_format("1"), EOF);
    KT_CHECK_INT_EQ(scan_null_stream(null_stream()), EOF);
}

/* --- the names of a pattern --------------------------------------------- */

static void test_glob(void)
{
    glob_t found;

    memset(&found, 0, sizeof(found));

    KT_CHECK_INT_EQ(glob("/tmp/*.c", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 2);
    if (found.gl_pathc == 2) {
        KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/a.c");
        KT_CHECK_STR_EQ(found.gl_pathv[1], "/tmp/b.c");
        KT_CHECK(found.gl_pathv[2] == NULL);
    }
    globfree(&found);

    /* The set, the question mark and the dot of a hidden name. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/[ab]*", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 3);
    globfree(&found);

    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/?x*", 0, NULL, &found), GLOB_NOMATCH);
    KT_CHECK_INT_EQ(glob("/tmp/.h*", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 1);
    KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/.hidden");
    globfree(&found);

    /* `*` does not answer a name with a leading dot. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/*", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 3);
    globfree(&found);

    /* A relative pattern keeps the names it was given, without a leading
     * slash; the directory of the program is the root of the fixture. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("etc/*", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 2);
    if (found.gl_pathc == 2) {
        KT_CHECK_STR_EQ(found.gl_pathv[0], "etc/passwd");
        KT_CHECK_STR_EQ(found.gl_pathv[1], "etc/setting");
    }
    globfree(&found);

    /* The directory at the end of the pattern and GLOB_MARK. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 1);
    KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/");
    globfree(&found);

    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp", GLOB_MARK, NULL, &found), 0);
    KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/");
    globfree(&found);

    /* The literal name that is not there, with and without GLOB_NOCHECK. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/missing*", 0, NULL, &found), GLOB_NOMATCH);
    KT_CHECK_INT_EQ(found.gl_pathc, 0);
    KT_CHECK(found.gl_pathv == NULL);
    globfree(&found);

    KT_CHECK_INT_EQ(glob("/tmp/missing*", GLOB_NOCHECK, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 1);
    KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/missing*");
    globfree(&found);

    /* The order of the directory without the sort. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/*", GLOB_NOSORT, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 3);
    if (found.gl_pathc == 3) {
        KT_CHECK_STR_EQ(found.gl_pathv[0], "/tmp/a.c");
        KT_CHECK_STR_EQ(found.gl_pathv[1], "/tmp/b.c");
        KT_CHECK_STR_EQ(found.gl_pathv[2], "/tmp/ab.txt");
    }
    globfree(&found);

    /* The room of the caller in front of the names. */
    memset(&found, 0, sizeof(found));
    found.gl_offs = 2;
    KT_CHECK_INT_EQ(glob("/bin/*", GLOB_DOOFFS, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 3);
    if (found.gl_pathv != NULL) {
        KT_CHECK(found.gl_pathv[0] == NULL);
        KT_CHECK(found.gl_pathv[1] == NULL);
        KT_CHECK_STR_EQ(found.gl_pathv[2], "/bin/cat");
    }
    globfree(&found);
    KT_CHECK(found.gl_pathv == NULL);

    /* The names of the previous call stay in front of the new ones, and a
     * pattern that added none is NOMATCH all the same. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp/*.c", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 2);
    KT_CHECK_INT_EQ(glob("/etc/*", GLOB_APPEND, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 4);
    KT_CHECK_INT_EQ(glob("/tmp/missing*", GLOB_APPEND, NULL, &found), GLOB_NOMATCH);
    KT_CHECK_INT_EQ(found.gl_pathc, 4);
    globfree(&found);

    /* The bad shapes of a call. */
    KT_CHECK_INT_EQ(glob(NULL, 0, NULL, &found), GLOB_NOMATCH);
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/tmp", 0, NULL, NULL), GLOB_NOMATCH);
    globfree(NULL);

    /* A path that names the root and the fold inside a component. */
    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 1);
    KT_CHECK_STR_EQ(found.gl_pathv[0], "/");
    globfree(&found);

    memset(&found, 0, sizeof(found));
    KT_CHECK_INT_EQ(glob("/etc/../tmp/*.c", 0, NULL, &found), 0);
    KT_CHECK_INT_EQ(found.gl_pathc, 2);
    globfree(&found);
}

/* --- the numbers with a sign --------------------------------------------- */

static void test_signed_numbers(void)
{
    char *end = NULL;
    long long wide;

    ktest_begin("libc: a negative number of a conversion is a number and not ERANGE");

    /* The conversion answers the magnitude and the sign apart, and this is the
     * bug the library had until 1.7: a magnitude negated before the test of
     * the range is 2^64-7 for -7, which is ERANGE for every negative number. */
    KT_CHECK_INT_EQ((long long)atoll("-7"), -7LL);
    KT_CHECK_INT_EQ((long long)strtol("-42", NULL, 10), -42L);
    KT_CHECK_INT_EQ((long long)atoi("-9"), -9);
    KT_CHECK_INT_EQ((long long)strtoll("-1234567890123", NULL, 10), -1234567890123LL);
    KT_CHECK_INT_EQ((long long)strtoll("7", NULL, 10), 7LL);
    KT_CHECK_INT_EQ((long long)strtoll("-0", NULL, 10), 0LL);
    KT_CHECK_INT_EQ((long long)strtoll("-2147483648", NULL, 10), -2147483648LL);
    KT_CHECK_INT_EQ((long long)strtoll("-9223372036854775808", NULL, 10), LLONG_MIN);

    /* A magnitude past the range is still an error, and the sign survives. */
    errno = 0;
    KT_CHECK_INT_EQ((long long)strtoll("9223372036854775808", NULL, 10), LLONG_MAX);
    KT_CHECK_INT_EQ(errno, ERANGE);
    errno = 0;
    KT_CHECK_INT_EQ((long long)strtoll("-9223372036854775809", NULL, 10), LLONG_MIN);
    KT_CHECK_INT_EQ(errno, ERANGE);

    wide = strtoll("-1000000", &end, 16);
    KT_CHECK_INT_EQ((long long)wide, -16777216LL);
    KT_CHECK(end != NULL && *end == '\0');
}

void test_libc_user(void)
{
    ktest_begin("libc_user");

    test_environment();
    test_realpath();
    test_strptime();
    test_signed_numbers();
    test_long_options();
    test_scanf();
    test_glob();
}
