/*
 * Micro OS — the tests of the library of a program.
 *
 * The library of a program (`components/libc/user`) is not the library of the
 * kernel: it runs in the window of a program and reaches the kernel through
 * the door of the system calls. Most of its functions are a call and cannot
 * run in the process of the host, but the pure part - the classes, the
 * strings, the pattern of a name, the options of a line, the regular
 * expressions, the two halves of a path and the one user - is C without a
 * kernel under it, and this test compiles those files for the host.
 *
 * The door itself, the streams and the directory are covered by
 * `make x86-smoke` and `make x86-disk-smoke`, where a program of the machine
 * reads a file of the disk.
 */
#include "ktest.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include <fnmatch.h>
#include <regex.h>

#include "sbase_port.h"

void test_userland(void);

/* The text of /etc/passwd the door of this binary answers
 * (tests/cases/userland_door_shim.c). */
void userland_door_set_passwd(const char *text);

static void test_classes(void)
{
    KT_CHECK(isdigit('7'));
    KT_CHECK(!isdigit('a'));
    KT_CHECK(isalpha('z') && isalpha('Q'));
    KT_CHECK(isalnum('8') && !isalnum('-'));
    KT_CHECK(isspace(' ') && isspace('\n') && !isspace('x'));
    KT_CHECK(iscntrl('\t') && !iscntrl(' '));
    KT_CHECK(isprint(' ') && !isprint('\n'));
    KT_CHECK(isxdigit('F') && isxdigit('0') && !isxdigit('g'));
    KT_CHECK_INT_EQ(toupper('q'), 'Q');
    KT_CHECK_INT_EQ(tolower('Q'), 'q');
    KT_CHECK_INT_EQ(toupper('1'), '1');
}

static void test_strings(void)
{
    char *copy = strdup("hello");
    char *part = strndup("world", 3);
    char text[] = "a,b;c";

    KT_CHECK(copy != NULL && strcmp(copy, "hello") == 0);
    KT_CHECK(part != NULL && strcmp(part, "wor") == 0);
    free(copy);
    free(part);

    KT_CHECK_INT_EQ(strspn("aaab", "a"), 3);
    KT_CHECK_INT_EQ(strcspn(text, ",;"), 1);
    KT_CHECK(strpbrk(text, ";,") == text + 1);
    KT_CHECK(strpbrk(text, "z") == NULL);
    KT_CHECK_INT_EQ(strcasecmp("Hello", "hELLO"), 0);
    KT_CHECK(strcasecmp("abc", "abd") < 0);
    KT_CHECK_INT_EQ(strncasecmp("HELLO", "help", 3), 0);
    KT_CHECK(strcasestr("a HeLLo b", "hello") != NULL);
    KT_CHECK(strcasestr("nothing", "hello") == NULL);
}

static void test_paths(void)
{
    char first[] = "/etc/setting";
    char second[] = "setting";
    char third[] = "/";
    char fourth[] = "/etc/";

    KT_CHECK_STR_EQ(basename(first), "setting");
    KT_CHECK_STR_EQ(dirname(first), "/etc");
    KT_CHECK_STR_EQ(basename(second), "setting");
    KT_CHECK_STR_EQ(dirname(second), ".");
    KT_CHECK_STR_EQ(basename(third), "/");
    KT_CHECK_STR_EQ(basename(fourth), "etc");
}

static void test_patterns(void)
{
    KT_CHECK_INT_EQ(fnmatch("*.c", "main.c", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("*.c", "main.h", 0), FNM_NOMATCH);
    KT_CHECK_INT_EQ(fnmatch("a?c", "abc", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("a?c", "ac", 0), FNM_NOMATCH);
    KT_CHECK_INT_EQ(fnmatch("[abc]", "b", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("[!abc]", "d", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("[a-c]x", "bx", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("[[:digit:]]*", "12ab", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("*", "a/b", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("*", "a/b", FNM_PATHNAME), FNM_NOMATCH);
    KT_CHECK_INT_EQ(fnmatch("a\\*b", "a*b", 0), 0);
    KT_CHECK_INT_EQ(fnmatch("a\\*b", "a*b", FNM_NOESCAPE), FNM_NOMATCH);
}

static void test_options(void)
{
    char *argv[] = {"tool", "-ab", "-c", "value", "name", NULL};
    char *const *words = argv;

    optind = 1;
    opterr = 0;
    KT_CHECK_INT_EQ(getopt(5, words, "abc:"), 'a');
    KT_CHECK_INT_EQ(getopt(5, words, "abc:"), 'b');
    KT_CHECK_INT_EQ(getopt(5, words, "abc:"), 'c');
    KT_CHECK_STR_EQ(optarg, "value");
    KT_CHECK_INT_EQ(getopt(5, words, "abc:"), -1);
    KT_CHECK_INT_EQ(optind, 4);

    {
        char *end[] = {"tool", "--", "-x", NULL};
        char *const *two = end;

        optind = 1;
        KT_CHECK_INT_EQ(getopt(3, two, "x"), -1);
        KT_CHECK_INT_EQ(optind, 2);
    }

    {
        char *missing[] = {"tool", "-c", NULL};
        char *const *one = missing;

        optind = 1;
        KT_CHECK_INT_EQ(getopt(2, one, "c:"), '?');
    }

    {
        char *unknown[] = {"tool", "-z", NULL};
        char *const *one = unknown;

        optind = 1;
        KT_CHECK_INT_EQ(getopt(2, one, "a"), '?');
    }
}

static void test_expressions(void)
{
    regex_t expression;
    regmatch_t matches[3];

    KT_CHECK_INT_EQ(regcomp(&expression, "hello", 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "say hello", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "goodbye", 0, NULL, 0), REG_NOMATCH);
    regfree(&expression);

    KT_CHECK_INT_EQ(regcomp(&expression, "h.llo", 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "hallo", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "hllo", 0, NULL, 0), REG_NOMATCH);
    regfree(&expression);

    KT_CHECK_INT_EQ(regcomp(&expression, "^a[0-9]*z$", REG_EXTENDED), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "a123z", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "az", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "xa1z", 0, NULL, 0), REG_NOMATCH);
    KT_CHECK_INT_EQ(regexec(&expression, "a1", 0, NULL, 0), REG_NOMATCH);
    regfree(&expression);

    KT_CHECK_INT_EQ(regcomp(&expression, "HELLO", REG_ICASE), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "hello", 0, NULL, 0), 0);
    regfree(&expression);

    /* The group and its back reference, in the basic form of sed. */
    KT_CHECK_INT_EQ(regcomp(&expression, "\\(ab\\)\\1", 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "abab", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "abba", 0, NULL, 0), REG_NOMATCH);
    regfree(&expression);

    /* The extended alternation and the offsets of the whole match. */
    KT_CHECK_INT_EQ(regcomp(&expression, "(cat|dog)s?", REG_EXTENDED), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "two dogs", 1, matches, 0), 0);
    KT_CHECK_INT_EQ(matches[0].rm_so, 4);
    KT_CHECK_INT_EQ(matches[0].rm_eo, 8);
    regfree(&expression);

    /* A repetition that can match nothing does not run in a circle. */
    KT_CHECK_INT_EQ(regcomp(&expression, "\\(a*\\)*b", 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "aaab", 0, NULL, 0), 0);
    KT_CHECK_INT_EQ(regexec(&expression, "c", 0, NULL, 0), REG_NOMATCH);
    regfree(&expression);

    KT_CHECK(regcomp(&expression, "[abc", 0) != 0);
    regfree(&expression);
}

static void test_users(void)
{
    struct passwd *user;

    /*
     * The records of /etc/passwd, read through the door of this binary: the
     * fixture is a file of two accounts and a line that is not a record, and
     * the reader has to skip the line, find the two and keep the name of the
     * one it answered last.
     */
    userland_door_set_passwd("# the accounts of the test\n"
                             "root:0:0:/:/bin/sh\n"
                             "guest:7:7:/home/guest:/bin/sh\n"
                             "not a record\n");

    user = getpwuid(0);
    KT_CHECK(user != NULL);
    if (user != NULL) {
        KT_CHECK_STR_EQ(user->pw_name, "root");
        KT_CHECK_INT_EQ(user->pw_uid, 0);
        KT_CHECK_INT_EQ(user->pw_gid, 0);
        KT_CHECK_STR_EQ(user->pw_dir, "/");
        KT_CHECK_STR_EQ(user->pw_shell, "/bin/sh");
        KT_CHECK_STR_EQ(user->pw_passwd, "");
    }

    user = getpwuid(7);
    KT_CHECK(user != NULL);
    if (user != NULL) {
        KT_CHECK_STR_EQ(user->pw_name, "guest");
        KT_CHECK_INT_EQ(user->pw_uid, 7);
        KT_CHECK_INT_EQ(user->pw_gid, 7);
        KT_CHECK_STR_EQ(user->pw_dir, "/home/guest");
    }

    user = getpwnam("guest");
    KT_CHECK(user != NULL);
    if (user != NULL) {
        KT_CHECK_INT_EQ(user->pw_uid, 7);
    }

    KT_CHECK(getpwnam("root") != NULL);
    KT_CHECK(getpwnam("nobody") == NULL);
    KT_CHECK(getpwuid(8) == NULL);

    /* A machine without /etc/passwd has no name at all: the call answers NULL
     * and errno says why, which is what a portable source reads. */
    userland_door_set_passwd(NULL);
    errno = 0;
    KT_CHECK(getpwnam("root") == NULL);
    KT_CHECK_INT_EQ(errno, ENOENT);

    /* The credentials come through the door of the call; the shim of this
     * binary answers the root of the system, as the kernel of the host does
     * before a test of the kernel moves its task. */
    KT_CHECK_INT_EQ(getuid(), 0);
    KT_CHECK_INT_EQ(geteuid(), 0);
    KT_CHECK_INT_EQ(getgid(), 0);
    KT_CHECK_INT_EQ(setuid(0), 0);
    KT_CHECK_INT_EQ(setgid(0), 0);
}

static void test_number_order(void)
{
    KT_CHECK(sbase_numcmp("2", "10") < 0);
    KT_CHECK(sbase_numcmp("10", "2") > 0);
    KT_CHECK_INT_EQ(sbase_numcmp("1", "1.0"), 0);
    KT_CHECK_INT_EQ(sbase_numcmp("1.00", "1"), 0);
    KT_CHECK(sbase_numcmp("-3", "2") < 0);
    KT_CHECK(sbase_numcmp("-3", "-2") < 0);
    KT_CHECK(sbase_numcmp("0.05", "0.5") < 0);
    KT_CHECK(sbase_numcmp("1e3", "999") > 0);
    KT_CHECK_INT_EQ(sbase_numcmp("word", "0"), 0);
}

void test_userland(void)
{
    ktest_begin("userland");

    test_classes();
    test_strings();
    test_paths();
    test_patterns();
    test_options();
    test_expressions();
    test_users();
    test_number_order();
}
