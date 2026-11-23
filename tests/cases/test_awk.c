/*
 * Micro OS — the tests of awk, the third program of somebody else.
 *
 * The interpreter is the One True Awk of `third_party/awk`, compiled for the
 * host with the headers of the host and the same sources the guest build uses
 * (`ports/port_x86_64/CMakeLists.txt` names the archive of the image). The
 * test runs the `main` of the release as a program: every case is a child
 * process of this one, with its own stdout and stderr on a temporary file, so
 * the exit of an awk that reports an error - it calls `exit(2)` through
 * `FATAL` - is the status of the case and not the end of the suite.
 *
 * What is covered is the surface a script of the machine uses: the fields and
 * FS/RS/OFS, the arithmetic and the formatter, the arrays and `for (k in a)`,
 * `sub`/`gsub`/`match`, `getline` in its forms, `-v` and `-f`, BEGIN and END,
 * the string and mathematical functions, `next`/`nextfile`/`exit`, and the
 * comparison of a numeric string with a number and with a string, which is
 * the place POSIX is easiest to get wrong and every awk was wrong once.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The `main` of the release, renamed by the build of the test. */
int awk_main(int argc, char *argv[]);

#define DATA_FILE "microos_awk_data"
#define WORDS_FILE "microos_awk_words"
#define SEMI_FILE "microos_awk_semi"
#define SECOND_FILE "microos_awk_second"
#define PROGRAM_FILE "microos_awk_prog.awk"
#define OUTPUT_FILE "microos_awk_out"

#define OUTPUT_SIZE 4096
#define ERROR_SIZE 1024

/* One case: the words of its line, and what the program has to answer. */
struct awk_case {
    const char *name;
    const char *words[8];
    const char *output;
    int status;
};

static const struct awk_case g_cases[] = {
    /* The fields: -F, OFS and $n. */
    {"fields", {"-F:", "{ print $1, $3 }", DATA_FILE}, "root 0\nbin 1\nnik 1000\n", 0},
    {"sum-nr-nf", {"-F:", "{ s += $3 } END { print s, NR, NF }", DATA_FILE}, "1001 3 3\n", 0},
    {"default-fs", {"{ print NF, $2 }", WORDS_FILE}, "3 b\n2 9\n", 0},
    {"rs", {"-v", "RS=;", "{ print NR, $0 }", SEMI_FILE}, "1 a\n2 b\n3 c\n", 0},

    /* The arithmetic and the formatter of a double. */
    {"print-number",
     {"BEGIN { print 1/3; printf \"%.3f\\n\", 1/3; printf \"%5.2f|%d|%s\\n\", 3.14159, 42, \"x\" "
      "}"},
     "0.333333\n0.333\n 3.14|42|x\n",
     0},
    {"printf-conversions",
     {"BEGIN { printf \"%d|%i|%o|%x|%X|%c|%e|%g|%5s|\\n\", 42, 42, 8, 255, 255, 65, 12345.678, "
      "0.5, \"ab\" }"},
     "42|42|10|ff|FF|A|1.234568e+04|0.5|   ab|\n",
     0},
    {"printf-star", {"BEGIN { printf \"%*.*f\\n\", 8, 2, 3.14159 }"}, "    3.14\n", 0},

    /* The arrays: a scalar key, a key of two parts (SUBSEP), delete, in. */
    {"arrays",
     {"BEGIN { a[\"x\"] = 1; a[\"y\"] = 2; for (k in a) s += a[k]; print s; delete a[\"x\"]; "
      "print (\"x\" in a), (\"y\" in a) }"},
     "3\n0 1\n",
     0},
    {"array-subsep",
     {"BEGIN { m[1, 2] = \"v\"; print m[1, 2], (SUBSEP == \"\\034\"), length(SUBSEP) }"},
     "v 1 1\n",
     0},
    {"split",
     {"BEGIN { n = split(\"a:b:c\", p, \":\"); print n, p[2], p[3]; "
      "m = split(\"a1b22c\", q, /[0-9]+/); print m, q[1], q[3] }"},
     "3 b c\n3 a c\n",
     0},

    /* The regular expressions of a substitution and of match. */
    {"gsub", {"{ gsub(/o/, \"0\"); print }", "microos_awk_foo"}, "f00 b00\n", 0},
    {"sub-return", {"{ n = sub(/o+/, \"X\"); print n, $0 }", "microos_awk_foo"}, "1 fX boo\n", 0},
    {"gsub-target",
     {"{ n = gsub(/o/, \"0\", $2); print n, $1, $2 }", "microos_awk_foo"},
     "2 foo b00\n",
     0},
    {"match",
     {"BEGIN { s = \"hello world\"; print match(s, /o w/), RSTART, RLENGTH; "
      "print match(\"abc\", /z/), RSTART, RLENGTH }"},
     "5 5 3\n0 0 -1\n",
     0},

    /* getline: a variable from a file, and the main input through the loop. */
    {"getline-file",
     {"BEGIN { n = 0; while ((getline line < \"" DATA_FILE "\") > 0) { n++; last = line }; "
      "close(\"" DATA_FILE "\"); print n, last }"},
     "3 nik:1000:1000\n",
     0},
    {"getline-main", {"NR == 1 { getline; print }", DATA_FILE}, "bin:1:1\n", 0},

    /* The command line: -v and -f, and the two ends of a program. */
    {"dash-v", {"-v", "x=42", "-v", "name=abc", "BEGIN { print x + 1, name }"}, "43 abc\n", 0},
    {"dash-f", {"-f", PROGRAM_FILE, DATA_FILE}, "1001\n", 0},
    {"filename-fnr-nr",
     {"{ print FILENAME, FNR, NR, $0 }", DATA_FILE, SECOND_FILE},
     "microos_awk_data 1 1 root:0:0\nmicroos_awk_data 2 2 bin:1:1\n"
     "microos_awk_data 3 3 nik:1000:1000\nmicroos_awk_second 1 4 x\n"
     "microos_awk_second 2 5 y\n",
     0},
    {"nextfile",
     {"FNR == 1 { print FILENAME; nextfile }", DATA_FILE, SECOND_FILE},
     "microos_awk_data\nmicroos_awk_second\n",
     0},
    {"next", {"{ if (NR == 2) next; print }", DATA_FILE}, "root:0:0\nnik:1000:1000\n", 0},
    {"exit-status", {"NR == 2 { exit 3 }", DATA_FILE}, "", 3},
    /* The error path of the interpreter: a syntax error is a sentence on the
     * error descriptor and the status 2 of `FATAL`, and the program never
     * runs. */
    {"syntax-error", {"BEGIN {"}, "", 2},

    /* A file written and read back through the redirection of print and
     * getline; the descriptor is closed between the two. */
    {"print-to-file",
     {"BEGIN { print \"one\" > \"" OUTPUT_FILE "\"; print \"two\" >> \"" OUTPUT_FILE "\"; "
      "close(\"" OUTPUT_FILE "\"); while ((getline l < \"" OUTPUT_FILE "\") > 0) print \"got\", l; "
      "close(\"" OUTPUT_FILE "\") }"},
     "got one\ngot two\n",
     0},

    /* The comparison of a numeric string: the value of a field is a numeric
     * string and compares as a number against a number, as a string against a
     * string; a literal of the program is a string and compares as a string
     * even when it looks like a number (POSIX, "Expressions in awk"). */
    {"numeric-string-field",
     {"{ print ($1 < 9), ($1 < \"9\"), ($1 == 10), ($1 == \"10\") }", "microos_awk_ten"},
     "0 1 1 1\n",
     0},
    {"numeric-string-literal",
     {"BEGIN { print (\"10\" < 9), (10 < \"9\"), (\"10\" == 10), (\"10\" == \"10.0\"); "
      "x = \"10\"; print (x < 9), (x < \"9\"), (x == 10) }"},
     "1 1 1 0\n1 1 1\n",
     0},
    {"uninitialised", {"BEGIN { print (x == 0), (x == \"\"), length(x) }"}, "1 1 0\n", 0},

    /* The operators: the two increments, the assignments of a compound, the
     * concatenation without an operator, the conditional. */
    {"operators",
     {"BEGIN { x = 5; print x++, x, ++x; x += 2; x *= 3; print x; y = \"a\" \"b\" \"c\"; "
      "print y; print (x > 0 ? \"p\" : \"q\") }"},
     "5 6 7\n27\nabc\np\n",
     0},

    /* The functions of a string and of a number. */
    {"string-functions",
     {"BEGIN { printf \"%s|%d|%d|%s|%s\\n\", substr(\"hello\", 2, 3), index(\"hello\", \"ll\"), "
      "length(\"hello\"), tolower(\"AB\"), toupper(\"ab\") }"},
     "ell|3|5|ab|AB\n",
     0},
    {"number-functions",
     {"BEGIN { print int(-3.7), int(3.7), sprintf(\"%.2f\", 2.5) }"},
     "-3 3 2.50\n",
     0},
    {"math",
     {"BEGIN { printf \"%.4f %.4f %.4f %.4f\\n\", sqrt(2), exp(1), log(10), atan2(1, 1); "
      "srand(1); r = rand(); print (r >= 0 && r < 1) }"},
     "1.4142 2.7183 2.3026 0.7854\n1\n",
     0},

    /* OFMT is the format of print and CONVFMT the one of the conversion. */
    {"ofmt-convfmt",
     {"BEGIN { OFMT = \"%.2f\"; print 3.14159; CONVFMT = \"%.3f\"; x = 2.5; print (x \"\") }"},
     "3.14\n2.500\n",
     0},
};

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    fputs(text, file);
    fclose(file);
}

/* Run one case: the words are copied into a mutable vector (awk writes to
 * argv[0] and to the words of -v), the child writes its two streams to a
 * temporary file, and the parent reads them back after the wait. */
static void run_case(const struct awk_case *test, char *output, char *error)
{
    char words[9][256];
    char *argv[10];
    FILE *out;
    FILE *err;
    pid_t child;
    int status = 0;
    int count = 1;

    /* The name of the program is what the machine spells `/bin/awk`; awk
     * prints it in front of an error and keeps it in ARGV[0]. */
    snprintf(words[0], sizeof(words[0]), "%s", "awk");
    argv[0] = words[0];

    while (count < 9 && test->words[count - 1] != NULL) {
        snprintf(words[count], sizeof(words[count]), "%s", test->words[count - 1]);
        argv[count] = words[count];
        count++;
    }
    argv[count] = NULL;

    out = tmpfile();
    err = tmpfile();
    KT_CHECK(out != NULL && err != NULL);
    if (out == NULL || err == NULL) {
        return;
    }

    /* The buffers of the parent belong to the parent: a child that inherits a
     * byte of them would write it twice through its own descriptor. */
    fflush(NULL);

    child = fork();
    KT_CHECK(child >= 0);
    if (child == 0) {
#if defined(__SANITIZE_ADDRESS__)
        /* The interpreter of the release frees what it wants and ends with
         * `exit` of a program; the leak of a child of a test is not a leak
         * of the library, and the report would be in the stream the case
         * compares. */
        extern void __lsan_disable(void);

        __lsan_disable();
#endif
        (void)dup2(fileno(out), 1);
        (void)dup2(fileno(err), 2);
        fclose(out);
        fclose(err);
        {
            int code = awk_main(count, argv);

            _exit(code);
        }
    }

    if (child < 0) {
        fclose(out);
        fclose(err);
        return;
    }

    KT_CHECK(waitpid(child, &status, 0) == child);
    KT_CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
        KT_CHECK_INT_EQ(WEXITSTATUS(status), test->status);
    }

    output[0] = '\0';
    error[0] = '\0';
    if (fseek(out, 0, SEEK_SET) == 0) {
        size_t got = fread(output, 1, OUTPUT_SIZE - 1, out);
        output[got] = '\0';
    }
    if (fseek(err, 0, SEEK_SET) == 0) {
        size_t got = fread(error, 1, ERROR_SIZE - 1, err);
        error[got] = '\0';
    }
    fclose(out);
    fclose(err);

    if (strcmp(output, test->output) != 0) {
        ktest_checks++;
        ktest_fail(__FILE__, __LINE__, "%s: output \"%s\", expected \"%s\" (stderr \"%s\")",
                   test->name, output, test->output, error);
    }
}

void test_awk(void)
{
    char output[OUTPUT_SIZE];
    char error[ERROR_SIZE];
    size_t index;

    ktest_begin("awk: the script of the machine through the interpreter of the release");

    write_file(DATA_FILE, "root:0:0\nbin:1:1\nnik:1000:1000\n");
    write_file(WORDS_FILE, "a b  c\n10 9\n");
    write_file(SEMI_FILE, "a;b;c");
    write_file(SECOND_FILE, "x\ny\n");
    write_file(PROGRAM_FILE, "BEGIN { FS = \":\" }\n{ sum += $3 }\nEND { print sum }\n");
    write_file("microos_awk_ten", "10\n");
    write_file("microos_awk_foo", "foo boo\n");

    for (index = 0; index < sizeof(g_cases) / sizeof(g_cases[0]); index++) {
#ifdef MICROOS_TSAN
        /* The `exit` of a script runs the `exit` of the C library of the
         * host, and its hooks under ThreadSanitizer report a race of the fork
         * of a threaded process and end the child with 66. The status of a
         * script is checked by `make test`, `make test-asan` and on the
         * machine by `make x86-smoke` (`/bin/awk BEGIN{exit(3)}`). */
        if (strcmp(g_cases[index].name, "exit-status") == 0 ||
            strcmp(g_cases[index].name, "syntax-error") == 0) {
            continue;
        }
#endif
        run_case(&g_cases[index], output, error);
    }

    remove(DATA_FILE);
    remove(WORDS_FILE);
    remove(SEMI_FILE);
    remove(SECOND_FILE);
    remove(PROGRAM_FILE);
    remove(OUTPUT_FILE);
    remove("microos_awk_ten");
    remove("microos_awk_foo");
}
