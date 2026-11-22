/*
 * Micro OS — the vendored PCRE2, a match and its groups.
 *
 * The library is third-party code, but the choices of third_party/pcre2's
 * config.h are ours: the 8-bit width, Unicode with the fixed tables, no JIT
 * and the matcher bounded for a program of this machine. The test locks
 * those choices and the behaviour a program leans on - a match, a group, an
 * anchor and a UTF-8 subject - so a new vendored release cannot change one
 * of them quietly.
 *
 * The test runs on the host, whose libc is the platform of the library: no
 * compatibility header of the program build is on its include path. That is
 * the second half of the check. The same config.h and the same sources run
 * in the image of /bin/pcre2grep.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdint.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

/*
 * Compile `pattern` and match `subject` once. The answer of the match is the
 * number of the pairs of the ovector (the whole match and its groups), and
 * the two offsets of the whole match are written where the caller asks. The
 * code and the match data are released here: every case owns its pattern.
 */
static int match_once(const char *pattern, uint32_t options, const char *subject, size_t *from,
                      size_t *to)
{
    PCRE2_UCHAR message[256];
    PCRE2_SIZE error_offset;
    PCRE2_SIZE *ovector;
    pcre2_match_data *data;
    pcre2_code *code;
    int error_code;
    int answer;

    code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, options, &error_code,
                         &error_offset, NULL);

    if (code == NULL) {
        ktest_checks++;
        if (pcre2_get_error_message(error_code, message, sizeof(message)) < 0) {
            ktest_fail(__FILE__, __LINE__, "the pattern was refused with error %d", error_code);
        } else {
            ktest_fail(__FILE__, __LINE__, "the pattern was refused: %s at offset %zu", message,
                       error_offset);
        }
        return -1000;
    }

    data = pcre2_match_data_create_from_pattern(code, NULL);
    if (data == NULL) {
        ktest_checks++;
        ktest_fail(__FILE__, __LINE__, "pcre2_match_data_create_from_pattern answered NULL");
        pcre2_code_free(code);
        return -1001;
    }

    answer = pcre2_match(code, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);

    if (answer >= 0) {
        ovector = pcre2_get_ovector_pointer(data);
        if (from != NULL) {
            *from = ovector[0];
        }
        if (to != NULL) {
            *to = ovector[1];
        }
    }

    pcre2_match_data_free(data);
    pcre2_code_free(code);

    return answer;
}

/* The group `number` of a match, as a copy in the caller's buffer. The
 * answer is the length of the group, or -1 when it did not take part. */
static int group_text(const char *pattern, uint32_t options, const char *subject, int number,
                      char *text, size_t size)
{
    PCRE2_SIZE *ovector;
    pcre2_match_data *data;
    pcre2_code *code;
    PCRE2_SIZE error_offset;
    int error_code;
    int answer;
    size_t length;

    code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, options, &error_code,
                         &error_offset, NULL);
    if (code == NULL) {
        return -1;
    }

    data = pcre2_match_data_create_from_pattern(code, NULL);
    if (data == NULL) {
        pcre2_code_free(code);
        return -1;
    }

    answer = pcre2_match(code, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);
    if (answer <= number) {
        pcre2_match_data_free(data);
        pcre2_code_free(code);
        return -1;
    }

    ovector = pcre2_get_ovector_pointer(data);
    if (ovector[2 * number] == PCRE2_UNSET) {
        pcre2_match_data_free(data);
        pcre2_code_free(code);
        return -1;
    }

    length = ovector[2 * number + 1] - ovector[2 * number];
    if (length + 1 > size) {
        length = size - 1;
    }
    memcpy(text, subject + ovector[2 * number], length);
    text[length] = '\0';

    pcre2_match_data_free(data);
    pcre2_code_free(code);

    return (int)length;
}

static void test_match_and_no_match(void)
{
    size_t from = 0;
    size_t to = 0;
    int answer;

    answer = match_once("h.llo", 0, "hello world", &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 0);
    KT_CHECK_INT_EQ(to, 5);

    /* `.` is one character, not one byte of anything: `help` has the wrong
     * second letter and does not match. */
    answer = match_once("h.llo", 0, "help", NULL, NULL);
    KT_CHECK_INT_EQ(answer, PCRE2_ERROR_NOMATCH);

    /* The case option is the one of `pcre2grep -i`. */
    answer = match_once("h.llo", PCRE2_CASELESS, "HELLO", &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 0);
    KT_CHECK_INT_EQ(to, 5);

    /* A match is a substring, not the whole subject: the offsets are the
     * answer and not a length. */
    answer = match_once("x86", 0, "hostname=x86_64", &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 9);
    KT_CHECK_INT_EQ(to, 12);
}

static void test_groups(void)
{
    char text[32];
    int answer;

    answer = group_text("^(h(.)llo)$", 0, "hello", 0, text, sizeof(text));
    KT_CHECK_INT_EQ(answer, 5);
    KT_CHECK_STR_EQ(text, "hello");

    answer = group_text("^(h(.)llo)$", 0, "hello", 1, text, sizeof(text));
    KT_CHECK_INT_EQ(answer, 5);
    KT_CHECK_STR_EQ(text, "hello");

    answer = group_text("^(h(.)llo)$", 0, "hello", 2, text, sizeof(text));
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_STR_EQ(text, "e");

    /* A group that did not take part is unset, not an empty string. */
    answer = group_text("^(a)|(b)$", 0, "b", 1, text, sizeof(text));
    KT_CHECK_INT_EQ(answer, -1);
}

static void test_anchors(void)
{
    size_t from = 0;
    size_t to = 0;
    int answer;

    /* The subject of pcre2grep is one line, so `^` and `$` are its ends. */
    answer = match_once("^console=(vga|serial)$", 0, "console=vga", &from, &to);
    KT_CHECK_INT_EQ(answer, 2);
    KT_CHECK_INT_EQ(from, 0);
    KT_CHECK_INT_EQ(to, 11);

    answer = match_once("^console=", 0, "console=vga", &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 0);

    answer = match_once("vga$", 0, "console=vga", &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(to, 11);

    /* A newline is inside the subject here, and without PCRE2_MULTILINE the
     * anchors are the ends of the whole subject. */
    answer = match_once("^world$", 0, "hello\nworld", NULL, NULL);
    KT_CHECK_INT_EQ(answer, PCRE2_ERROR_NOMATCH);

    answer = match_once("^world$", PCRE2_MULTILINE, "hello\nworld", NULL, NULL);
    KT_CHECK_INT_EQ(answer, 1);
}

static void test_utf8_bytes(void)
{
    /* Unicode is not in the vendored configuration: it costs about 120 KiB
     * of tables, and a program image may be at most 64 KiB (KSYS_MAX_IO).
     * A UTF-8 subject is therefore a string of bytes, and a `.` is one byte
     * of it, not one character. The test locks that shape so a later release
     * cannot turn Unicode on in the image without a plan for the size. */
    static const char hello[] = "h\xc3\xa9llo";
    static const char twice[] = "\xc3\xa9\xc3\xa9";
    char text[32];
    size_t from = 0;
    size_t to = 0;
    int answer;

    answer = match_once("^h..llo$", 0, hello, &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 0);
    KT_CHECK_INT_EQ(to, sizeof(hello) - 1);

    answer = match_once("^\xc3\xa9\xc3\xa9$", 0, twice, &from, &to);
    KT_CHECK_INT_EQ(answer, 1);
    KT_CHECK_INT_EQ(from, 0);
    KT_CHECK_INT_EQ(to, sizeof(twice) - 1);

    answer = group_text("(\xc3\xa9)", 0, hello, 1, text, sizeof(text));
    KT_CHECK_INT_EQ(answer, 2);
    KT_CHECK_INT_EQ((unsigned char)text[0], 0xc3u);
    KT_CHECK_INT_EQ((unsigned char)text[1], 0xa9u);

    /* The option of a Unicode build is refused, not ignored: a pattern with
     * it would mean one thing here and another in a library that has the
     * tables. */
    {
        PCRE2_SIZE error_offset = 0;
        int error_code = 0;
        pcre2_code *code = pcre2_compile((PCRE2_SPTR) "^\\p{L}+$", PCRE2_ZERO_TERMINATED,
                                         PCRE2_UTF | PCRE2_UCP, &error_code, &error_offset, NULL);

        KT_CHECK(code == NULL);
        KT_CHECK(error_code > 0);
        KT_CHECK(error_offset <= strlen("^\\p{L}+$"));
    }
}

static void test_errors(void)
{
    PCRE2_UCHAR message[256];
    PCRE2_SIZE error_offset = 0;
    pcre2_code *code;
    int error_code = 0;

    /* An unclosed group names the offset of the mistake. */
    code = pcre2_compile((PCRE2_SPTR) "(hello", PCRE2_ZERO_TERMINATED, 0, &error_code,
                         &error_offset, NULL);
    KT_CHECK(code == NULL);
    /* The errors of the compiler are positive numbers, unlike the errors of
     * the matcher, and the offset is inside the pattern. */
    KT_CHECK(error_code > 0);
    KT_CHECK(error_offset <= strlen("(hello"));
    KT_CHECK(pcre2_get_error_message(error_code, message, sizeof(message)) > 0);
    KT_CHECK(strstr((const char *)message, "parenthesis") != NULL);
}

static void test_configuration(void)
{
    uint32_t value = 0;

    /* The choices of third_party/pcre2/config.h, read back from the library
     * that the image of pcre2grep carries. */
    KT_CHECK_INT_EQ(pcre2_config(PCRE2_CONFIG_UNICODE, &value), 0);
    KT_CHECK_INT_EQ(value, 0);

    KT_CHECK_INT_EQ(pcre2_config(PCRE2_CONFIG_JIT, &value), 0);
    KT_CHECK_INT_EQ(value, 0);

    KT_CHECK_INT_EQ(pcre2_config(PCRE2_CONFIG_LINKSIZE, &value), 0);
    KT_CHECK_INT_EQ(value, 2);

    KT_CHECK_INT_EQ(pcre2_config(PCRE2_CONFIG_NEWLINE, &value), 0);
    KT_CHECK_INT_EQ(value, 2);

    KT_CHECK_INT_EQ(pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &value), 0);
    KT_CHECK_INT_EQ(value, 1000000u);

    /* The release the tree carries, read from the library and not from the
     * README. */
    {
        char version[32];

        /* A string setting answers its length, a number answers 0. */
        KT_CHECK(pcre2_config(PCRE2_CONFIG_VERSION, version) > 0);
        KT_CHECK(strstr(version, "10.42") != NULL);
    }
}

void test_pcre2(void)
{
    test_match_and_no_match();
    test_groups();
    test_anchors();
    test_utf8_bytes();
    test_errors();
    test_configuration();
}
