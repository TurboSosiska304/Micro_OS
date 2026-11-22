/*
 * Micro OS — the index of a repository (ADR 0024 §1).
 *
 * The index is JSON because a repository is published by a script and read
 * by a person as well as by this machine, and because the shape is the one
 * of every other repository of the world. The parser is small on purpose:
 * it knows the object of the format, skips a field it does not know (so a
 * repository grows without breaking an older kernel), and refuses a document
 * that is not the one it expects instead of guessing.
 */
#include <stdio.h>
#include <string.h>

#include "pkg/pkg.h"
#include "pkg_private.h"

/* The deepest object of the format: the top level and the packages. */
#define PKG_JSON_DEPTH_MAX 8

struct json_cursor {
    const char *p;
    const char *end;
};

static void json_ws(struct json_cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static int json_expect(struct json_cursor *c, char what)
{
    json_ws(c);

    if (c->p >= c->end || *c->p != what) {
        return PKG_EPARSE;
    }

    c->p++;

    return PKG_OK;
}

static int json_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

/* A string, with the escapes of JSON that can be a name of a package. */
static int json_string(struct json_cursor *c, char *out, k_size_t size)
{
    k_size_t used = 0;

    json_ws(c);

    if (c->p >= c->end || *c->p != '"') {
        return PKG_EPARSE;
    }

    c->p++;

    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;

        if (ch == '\\') {
            char escaped;

            if (c->p >= c->end) {
                return PKG_EPARSE;
            }

            escaped = *c->p++;

            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                ch = escaped;
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                /*
                 * A name of a package is ASCII. A code point that needs a
                 * UTF-16 escape is refused rather than cut to a byte: a
                 * second name of the same program is a different file.
                 */
                int high;
                int low;

                if (c->p + 4 > c->end) {
                    return PKG_EPARSE;
                }

                high = json_hex(c->p[0]);
                low = json_hex(c->p[1]);

                if (high < 0 || low < 0 || json_hex(c->p[2]) < 0 || json_hex(c->p[3]) < 0) {
                    return PKG_EPARSE;
                }

                ch = (char)((high << 4) | low);

                if (ch == '\0' || (k_u8_t)ch >= 0x80u) {
                    return PKG_EPARSE;
                }

                c->p += 4;
                break;
            }
            default:
                return PKG_EPARSE;
            }
        }

        if (used + 1 >= size) {
            return PKG_EPARSE;
        }

        out[used++] = ch;
    }

    if (c->p >= c->end) {
        return PKG_EPARSE;
    }

    c->p++;

    if (out != NULL) {
        out[used] = '\0';
    }

    return PKG_OK;
}

static int json_number(struct json_cursor *c, k_u64_t *out)
{
    k_u64_t value = 0;
    int digits = 0;

    json_ws(c);

    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        k_u64_t digit = (k_u64_t)(*c->p - '0');

        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10u) {
            return PKG_EPARSE;
        }

        value = value * 10u + digit;
        digits++;
        c->p++;
    }

    if (digits == 0) {
        return PKG_EPARSE;
    }

    if (out != NULL) {
        *out = value;
    }

    return PKG_OK;
}

/* A value this parser does not need: a string, a number, an object, an
 * array or one of the three words of the language. */
static int json_skip(struct json_cursor *c, int depth);

static int json_skip_container(struct json_cursor *c, char close, int depth)
{
    int rc;

    if (depth > PKG_JSON_DEPTH_MAX) {
        return PKG_EPARSE;
    }

    for (;;) {
        json_ws(c);

        if (c->p >= c->end) {
            return PKG_EPARSE;
        }

        if (*c->p == close) {
            c->p++;
            return PKG_OK;
        }

        if (close == '}') {
            char key[PKG_NAME_MAX + 1];

            rc = json_string(c, key, sizeof(key));
            if (rc != PKG_OK) {
                return rc;
            }

            rc = json_expect(c, ':');
            if (rc != PKG_OK) {
                return rc;
            }
        }

        rc = json_skip(c, depth + 1);
        if (rc != PKG_OK) {
            return rc;
        }

        json_ws(c);

        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }

        if (c->p < c->end && *c->p == close) {
            c->p++;
            return PKG_OK;
        }

        return PKG_EPARSE;
    }
}

static int json_skip(struct json_cursor *c, int depth)
{
    json_ws(c);

    if (c->p >= c->end) {
        return PKG_EPARSE;
    }

    if (*c->p == '"') {
        /* A string longer than the window is still a string to skip: the
         * cursor walks it, the bytes are not kept. */
        for (;;) {
            char ch = *c->p;

            if (ch == '"') {
                c->p++;
                return PKG_OK;
            }

            if (ch == '\\') {
                c->p++;
                if (c->p >= c->end) {
                    return PKG_EPARSE;
                }
            }

            c->p++;
        }
    }

    if (*c->p == '{') {
        c->p++;
        return json_skip_container(c, '}', depth);
    }

    if (*c->p == '[') {
        c->p++;
        return json_skip_container(c, ']', depth);
    }

    if ((k_size_t)(c->end - c->p) >= 4 && strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        return PKG_OK;
    }

    if ((k_size_t)(c->end - c->p) >= 5 && strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        return PKG_OK;
    }

    if ((k_size_t)(c->end - c->p) >= 4 && strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return PKG_OK;
    }

    return json_number(c, NULL);
}

static bool digest_is_hex(const char *text)
{
    k_size_t i;

    for (i = 0; i < PKG_SHA256_HEX - 1u; i++) {
        if (json_hex(text[i]) < 0) {
            return false;
        }
    }

    return text[PKG_SHA256_HEX - 1u] == '\0';
}

/* The fields that are defaulted by the index or derived from the others. */
static int entry_finish(struct pkg_entry *entry, const struct pkg_index *index, bool have_abi,
                        bool have_size)
{
    if (entry->name[0] == '\0' || entry->version[0] == '\0') {
        return PKG_EPARSE;
    }

    if (!have_abi) {
        entry->abi = index->abi;
    }

    if (!have_size) {
        return PKG_EPARSE;
    }

    if (entry->machine[0] == '\0') {
        memcpy(entry->machine, index->machine, sizeof(entry->machine));
    }

    if (entry->file[0] == '\0') {
        int n = snprintf(entry->file, sizeof(entry->file), "%s-%s-%s.mox", entry->name,
                         entry->version, entry->machine);

        if (n < 0 || (k_size_t)n >= sizeof(entry->file)) {
            return PKG_EPARSE;
        }
    }

    if (!digest_is_hex(entry->sha256)) {
        return PKG_EPARSE;
    }

    if (entry->size > PKG_PACKAGE_MAX) {
        return PKG_EPARSE;
    }

    return PKG_OK;
}

static int parse_entry(struct json_cursor *c, struct pkg_index *index)
{
    struct pkg_entry *entry;
    bool have_abi = false;
    bool have_size = false;
    int rc;

    if (index->count >= PKG_INDEX_MAX) {
        return PKG_EFULL;
    }

    entry = &index->entries[index->count];
    memset(entry, 0, sizeof(*entry));

    rc = json_expect(c, '{');
    if (rc != PKG_OK) {
        return rc;
    }

    for (;;) {
        char key[PKG_NAME_MAX + 1];

        json_ws(c);

        if (c->p < c->end && *c->p == '}') {
            c->p++;
            break;
        }

        rc = json_string(c, key, sizeof(key));
        if (rc != PKG_OK) {
            return rc;
        }

        rc = json_expect(c, ':');
        if (rc != PKG_OK) {
            return rc;
        }

        if (strcmp(key, "name") == 0) {
            rc = json_string(c, entry->name, sizeof(entry->name));
        } else if (strcmp(key, "version") == 0) {
            rc = json_string(c, entry->version, sizeof(entry->version));
        } else if (strcmp(key, "machine") == 0) {
            rc = json_string(c, entry->machine, sizeof(entry->machine));
        } else if (strcmp(key, "file") == 0) {
            rc = json_string(c, entry->file, sizeof(entry->file));
        } else if (strcmp(key, "summary") == 0) {
            rc = json_string(c, entry->summary, sizeof(entry->summary));
        } else if (strcmp(key, "sha256") == 0) {
            rc = json_string(c, entry->sha256, sizeof(entry->sha256));
        } else if (strcmp(key, "abi") == 0) {
            k_u64_t value = 0;

            rc = json_number(c, &value);
            if (rc == PKG_OK) {
                entry->abi = (k_u32_t)value;
                have_abi = true;
            }
        } else if (strcmp(key, "size") == 0) {
            rc = json_number(c, &entry->size);
            have_size = (rc == PKG_OK);
        } else {
            rc = json_skip(c, 1);
        }

        if (rc != PKG_OK) {
            return rc;
        }

        json_ws(c);

        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }

        if (c->p < c->end && *c->p == '}') {
            c->p++;
            break;
        }

        return PKG_EPARSE;
    }

    rc = entry_finish(entry, index, have_abi, have_size);
    if (rc != PKG_OK) {
        return rc;
    }

    index->count++;

    return PKG_OK;
}

static int parse_packages(struct json_cursor *c, struct pkg_index *index)
{
    int rc;

    rc = json_expect(c, '[');
    if (rc != PKG_OK) {
        return rc;
    }

    for (;;) {
        json_ws(c);

        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return PKG_OK;
        }

        rc = parse_entry(c, index);
        if (rc != PKG_OK) {
            return rc;
        }

        json_ws(c);

        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }

        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return PKG_OK;
        }

        return PKG_EPARSE;
    }
}

int pkg_index_parse(const char *text, k_size_t size, struct pkg_index *out)
{
    struct json_cursor c;
    int rc;

    if (text == NULL || out == NULL || size == 0) {
        return PKG_EPARSE;
    }

    memset(out, 0, sizeof(*out));

    c.p = text;
    c.end = text + size;

    rc = json_expect(&c, '{');
    if (rc != PKG_OK) {
        return rc;
    }

    for (;;) {
        char key[PKG_NAME_MAX + 1];

        json_ws(&c);

        if (c.p < c.end && *c.p == '}') {
            c.p++;
            break;
        }

        rc = json_string(&c, key, sizeof(key));
        if (rc != PKG_OK) {
            return rc;
        }

        rc = json_expect(&c, ':');
        if (rc != PKG_OK) {
            return rc;
        }

        if (strcmp(key, "repository") == 0) {
            rc = json_string(&c, out->repository, sizeof(out->repository));
        } else if (strcmp(key, "machine") == 0) {
            rc = json_string(&c, out->machine, sizeof(out->machine));
        } else if (strcmp(key, "abi") == 0) {
            k_u64_t value = 0;

            rc = json_number(&c, &value);
            if (rc == PKG_OK) {
                out->abi = (k_u32_t)value;
            }
        } else if (strcmp(key, "packages") == 0) {
            rc = parse_packages(&c, out);
        } else {
            rc = json_skip(&c, 1);
        }

        if (rc != PKG_OK) {
            return rc;
        }

        json_ws(&c);

        if (c.p < c.end && *c.p == ',') {
            c.p++;
            continue;
        }

        if (c.p < c.end && *c.p == '}') {
            c.p++;
            break;
        }

        return PKG_EPARSE;
    }

    if (out->machine[0] == '\0' || out->abi == 0u) {
        return PKG_EPARSE;
    }

    return PKG_OK;
}

int pkg_index_find(const struct pkg_index *index, const char *name)
{
    k_size_t i;

    if (index == NULL || name == NULL) {
        return -1;
    }

    for (i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}
