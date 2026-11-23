/*
 * Micro OS — the accounts of the machine: /etc/passwd and /etc/shadow.
 *
 * The two files are the truth and the kernel has no table of names: a lookup
 * opens the file, reads it and answers a record. That costs a read of a few
 * hundred bytes at a `login` and nothing at any other moment, and it buys the
 * property the tree wants — a person who edits /etc with `edit` has the new
 * account at the next login, and the library of a program reads the same
 * bytes as the shell.
 *
 * The hash is the SHA-256 of the salt and the password, salted and written
 * `$sha256$<salt>$<64 hex digits>`. The digest is the one of the loader
 * (ports/port_x86_64/loader/sha256.c), the same code that verifies a signed
 * image, compiled into this component a second time: one hash of the tree and
 * not two that agree by accident. It is not bcrypt and not argon2 — a fast
 * hash, which is honest for a machine with no login over the network and
 * which is recorded in docs/debts.md.
 *
 * A file that cannot be read whole is not read in part: a line that is not a
 * record stops the lookup, because half of a file of accounts is a different
 * set of users.
 */
#include "config/users.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "sha256.h"

/* The two names of the files inside the directory of the configuration. */
#define USERS_PASSWD_NAME "passwd"
#define USERS_SHADOW_NAME "shadow"

/* The fields of a record of each file. */
#define USERS_PASSWD_FIELDS 5u
#define USERS_SHADOW_FIELDS 2u

/* The longest salt of a hash, and the length of a digest in hex digits. */
#define USERS_SALT_MAX 16u
#define USERS_DIGEST_HEX 64u

/* The prefix of a hash of this system. */
#define USERS_HASH_PREFIX "$sha256$"

/* One line or one field of a line, as a counted string of a text that is not
 * NUL terminated at its end. */
struct field {
    const char *at;
    k_size_t len;
};

/* --- reading a file ------------------------------------------------------ */

/*
 * The whole file into `buf`, NUL terminated. Returns 0, USERS_ENOENT when
 * there is no such file, USERS_EFORMAT when the file does not fit, or
 * USERS_EIO for any other failure. The codes of the VFS do not travel out of
 * this function: a caller of the accounts speaks the language above.
 */
static int read_file(const char *path, char *buf, k_size_t size)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_size_t used = 0;
    int rc;

    if (size < 2) {
        return USERS_EFORMAT;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc == KERR_NOENT) {
        return USERS_ENOENT;
    }
    if (rc != 0) {
        kwarn("users: cannot open %s (rc=%d)", path, rc);
        return USERS_EIO;
    }

    for (;;) {
        int n;

        if (used + 1 >= size) {
            /* One byte more would not fit: the file is larger than the
             * buffer, and a cut file of accounts is not the file. */
            char probe;

            n = vfs_read(&file, &probe, 1);
            rc = (n > 0) ? USERS_EFORMAT : (n < 0 ? USERS_EIO : 0);
            break;
        }

        n = vfs_read(&file, buf + used, size - 1 - used);
        if (n < 0) {
            rc = USERS_EIO;
            break;
        }
        if (n == 0) {
            rc = 0;
            break;
        }

        used += (k_size_t)n;
    }

    vfs_close(&file);
    buf[used] = '\0';

    if (rc == USERS_EFORMAT) {
        kwarn("users: %s is longer than %u bytes, it is not read", path, (unsigned)(size - 1));
    }

    return rc;
}

/* The next line of a text that is walked in place. A line ends at '\n'; a
 * '\r' before it is dropped, so a file written on a laptop reads the same as
 * one written on the board. */
static bool next_line(const char **at, struct field *line)
{
    const char *start = *at;
    const char *end;

    if (start == NULL || start[0] == '\0') {
        return false;
    }

    end = strchr(start, '\n');
    if (end == NULL) {
        end = start + strlen(start);
    }

    line->at = start;
    line->len = (k_size_t)(end - start);

    while (line->len > 0 && line->at[line->len - 1] == '\r') {
        line->len--;
    }

    *at = (end[0] == '\n') ? end + 1 : end;

    return true;
}

/* A line copied into a string: false when it does not fit. */
static bool line_text(const struct field *line, char *out, k_size_t size)
{
    if (line->len + 1 > size) {
        return false;
    }

    memcpy(out, line->at, line->len);
    out[line->len] = '\0';

    return true;
}

/* A line that carries no record: empty, or a comment of a person. */
static bool line_is_noise(const struct field *line)
{
    return line->len == 0 || line->at[0] == '#';
}

/*
 * The fields of a line, at most `count` of them. False when the line has more
 * fields than a record of this file has: that is a line of another format and
 * not one to guess at.
 */
static bool split_fields(const struct field *line, struct field *fields, k_size_t count,
                         k_size_t *found)
{
    k_size_t at;
    k_size_t used = 0;
    k_size_t start = 0;

    for (at = 0; at <= line->len; at++) {
        if (at < line->len && line->at[at] != ':') {
            continue;
        }

        if (used >= count) {
            return false;
        }

        fields[used].at = line->at + start;
        fields[used].len = at - start;
        used++;

        start = at + 1;
    }

    *found = used;

    return true;
}

/* A field as a decimal number of fifteen bits: the width of a uid in a record
 * of a filesystem (kernel/ksys.h). False on anything else. */
static bool field_number(const struct field *field, k_u32_t *out)
{
    k_u32_t value = 0;
    k_size_t i;

    if (field->len == 0 || field->len > 5) {
        return false;
    }

    for (i = 0; i < field->len; i++) {
        char c = field->at[i];

        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + (k_u32_t)(c - '0');
    }

    if (value > KSYS_UID_MAX) {
        return false;
    }

    *out = value;

    return true;
}

/* A field copied into a string of `size`: false when it does not fit. */
static bool field_text(const struct field *field, char *out, k_size_t size)
{
    if (field->len + 1 > size) {
        return false;
    }

    memcpy(out, field->at, field->len);
    out[field->len] = '\0';

    return true;
}

/* A name: not empty, not a comment, and no path or space in it. A ':' cannot
 * reach here because the splitter cut the line at it. */
static bool name_is_valid(const struct field *field)
{
    k_size_t i;

    if (field->len == 0 || field->len >= USERS_NAME_MAX || field->at[0] == '#') {
        return false;
    }

    for (i = 0; i < field->len; i++) {
        char c = field->at[i];

        if (c == '/' || c == ' ' || c == '\t') {
            return false;
        }
    }

    return true;
}

/* --- the records --------------------------------------------------------- */

bool users_parse_passwd(const char *line, struct user_account *out)
{
    struct field record;
    struct field fields[USERS_PASSWD_FIELDS];
    k_size_t found = 0;
    struct user_account account;

    if (line == NULL || out == NULL) {
        return false;
    }

    record.at = line;
    record.len = strlen(line);

    while (record.len > 0 &&
           (record.at[record.len - 1] == '\r' || record.at[record.len - 1] == '\n')) {
        record.len--;
    }

    if (line_is_noise(&record)) {
        return false;
    }

    if (!split_fields(&record, fields, USERS_PASSWD_FIELDS, &found)) {
        return false;
    }
    if (found != USERS_PASSWD_FIELDS) {
        return false;
    }

    memset(&account, 0, sizeof(account));

    if (!name_is_valid(&fields[0])) {
        return false;
    }
    if (!field_text(&fields[0], account.name, sizeof(account.name))) {
        return false;
    }
    if (!field_number(&fields[1], &account.uid)) {
        return false;
    }
    if (!field_number(&fields[2], &account.gid)) {
        return false;
    }
    if (!field_text(&fields[3], account.home, sizeof(account.home))) {
        return false;
    }
    if (!field_text(&fields[4], account.shell, sizeof(account.shell))) {
        return false;
    }

    *out = account;

    return true;
}

bool users_parse_shadow(const char *line, char *name, k_size_t name_size, char *hash,
                        k_size_t hash_size)
{
    struct field record;
    struct field fields[USERS_SHADOW_FIELDS];
    k_size_t found = 0;

    if (line == NULL || name == NULL || hash == NULL) {
        return false;
    }

    record.at = line;
    record.len = strlen(line);

    while (record.len > 0 &&
           (record.at[record.len - 1] == '\r' || record.at[record.len - 1] == '\n')) {
        record.len--;
    }

    if (line_is_noise(&record)) {
        return false;
    }

    if (!split_fields(&record, fields, USERS_SHADOW_FIELDS, &found)) {
        return false;
    }
    if (found != USERS_SHADOW_FIELDS) {
        return false;
    }
    if (!name_is_valid(&fields[0])) {
        return false;
    }
    if (!field_text(&fields[0], name, name_size)) {
        return false;
    }
    if (!field_text(&fields[1], hash, hash_size)) {
        return false;
    }

    return true;
}

/* --- the hash ------------------------------------------------------------ */

/* The digest of the salt and the password, in the sixty-four digits of the
 * file. `out` is not terminated. */
static void digest_hex(const char *salt, const char *password, char out[USERS_DIGEST_HEX])
{
    struct microos_sha256_ctx context;
    k_u8_t raw[MICROOS_SHA256_SIZE];
    static const char digits[] = "0123456789abcdef";
    k_size_t i;

    microos_sha256_init(&context);
    microos_sha256_update(&context, (const uint8_t *)salt, strlen(salt));
    microos_sha256_update(&context, (const uint8_t *)password, strlen(password));
    microos_sha256_final(&context, raw);

    for (i = 0; i < MICROOS_SHA256_SIZE; i++) {
        out[i * 2] = digits[raw[i] >> 4];
        out[i * 2 + 1] = digits[raw[i] & 0x0Fu];
    }
}

bool users_hash_make(const char *password, const char *salt, char *out, k_size_t size)
{
    char digest[USERS_DIGEST_HEX];
    k_size_t salt_len;
    k_size_t at;

    if (password == NULL || salt == NULL || out == NULL) {
        return false;
    }

    salt_len = strlen(salt);
    if (salt_len == 0 || salt_len > USERS_SALT_MAX || strchr(salt, '$') != NULL) {
        return false;
    }

    if (strlen(USERS_HASH_PREFIX) + salt_len + 1 + USERS_DIGEST_HEX + 1 > size) {
        return false;
    }

    digest_hex(salt, password, digest);

    at = strlen(USERS_HASH_PREFIX);
    memcpy(out, USERS_HASH_PREFIX, at);
    memcpy(out + at, salt, salt_len);
    at += salt_len;
    out[at++] = '$';
    memcpy(out + at, digest, USERS_DIGEST_HEX);
    at += USERS_DIGEST_HEX;
    out[at] = '\0';

    return true;
}

bool users_hash_check(const char *hash, const char *password)
{
    char salt[USERS_SALT_MAX + 1];
    char digest[USERS_DIGEST_HEX];
    const char *mark;
    const char *digest_at;
    k_size_t salt_len;
    k_u8_t differs = 0;
    k_size_t i;

    if (hash == NULL || password == NULL) {
        return false;
    }

    /* A locked account: no password opens it, and an empty one is not a
     * password either. */
    if (hash[0] == '\0' || hash[0] == '*' || hash[0] == '!') {
        return false;
    }

    if (strncmp(hash, USERS_HASH_PREFIX, strlen(USERS_HASH_PREFIX)) != 0) {
        return false;
    }

    mark = hash + strlen(USERS_HASH_PREFIX);
    digest_at = strchr(mark, '$');
    if (digest_at == NULL || digest_at == mark) {
        return false;
    }

    salt_len = (k_size_t)(digest_at - mark);
    if (salt_len > USERS_SALT_MAX || strlen(digest_at + 1) != USERS_DIGEST_HEX) {
        return false;
    }

    memcpy(salt, mark, salt_len);
    salt[salt_len] = '\0';

    digest_hex(salt, password, digest);

    /* The comparison does not stop at the first digit that differs: a caller
     * that measures it learns nothing about the password. */
    for (i = 0; i < USERS_DIGEST_HEX; i++) {
        differs |= (k_u8_t)(digest[i] ^ digest_at[1 + i]);
    }

    return differs == 0;
}

/* --- the lookup ---------------------------------------------------------- */

/* The record of the line of /etc/passwd that matches, by name or by uid. */
static int find_passwd(const char *name, k_u32_t uid, bool by_uid, struct user_account *out)
{
    char text[USERS_FILE_MAX];
    char record[USERS_LINE_MAX];
    const char *at;
    struct field line;
    int rc;

    rc = read_file(USERS_PASSWD_PATH, text, sizeof(text));
    if (rc != 0) {
        return rc;
    }

    at = text;

    while (next_line(&at, &line)) {
        struct user_account account;

        if (line_is_noise(&line)) {
            continue;
        }

        if (!line_text(&line, record, sizeof(record)) || !users_parse_passwd(record, &account)) {
            return USERS_EFORMAT;
        }

        if (by_uid ? (account.uid == uid) : (strcmp(account.name, name) == 0)) {
            *out = account;
            return USERS_OK;
        }
    }

    return USERS_ENOENT;
}

int users_find_name(const char *name, struct user_account *out)
{
    if (name == NULL || out == NULL) {
        return USERS_ENOENT;
    }

    return find_passwd(name, 0, false, out);
}

int users_find_uid(k_u32_t uid, struct user_account *out)
{
    if (out == NULL) {
        return USERS_ENOENT;
    }

    return find_passwd(NULL, uid, true, out);
}

int users_find_hash(const char *name, char *hash, k_size_t size)
{
    char text[USERS_FILE_MAX];
    char record[USERS_LINE_MAX];
    char found[USERS_NAME_MAX];
    char found_hash[USERS_LINE_MAX];
    const char *at;
    struct field line;
    int rc;

    if (name == NULL || hash == NULL) {
        return USERS_ENOENT;
    }

    rc = read_file(USERS_SHADOW_PATH, text, sizeof(text));
    if (rc != 0) {
        return rc;
    }

    at = text;

    while (next_line(&at, &line)) {
        if (line_is_noise(&line)) {
            continue;
        }

        if (!line_text(&line, record, sizeof(record)) ||
            !users_parse_shadow(record, found, sizeof(found), found_hash, sizeof(found_hash))) {
            return USERS_EFORMAT;
        }

        if (strcmp(found, name) == 0) {
            if (strlen(found_hash) + 1 > size) {
                return USERS_EFORMAT;
            }
            memcpy(hash, found_hash, strlen(found_hash) + 1);
            return USERS_OK;
        }
    }

    return USERS_ENOENT;
}

int users_authenticate(const char *name, const char *password, struct user_account *out)
{
    char hash[USERS_LINE_MAX];
    struct user_account account;
    int rc;

    if (name == NULL || password == NULL || out == NULL) {
        return USERS_ENOENT;
    }

    rc = users_find_name(name, &account);
    if (rc != USERS_OK) {
        return rc;
    }

    rc = users_find_hash(account.name, hash, sizeof(hash));
    if (rc != USERS_OK) {
        /* An account without a line in /etc/shadow has no password at all,
         * and a password nobody set is not a password that opens it. */
        return USERS_EAUTH;
    }

    if (!users_hash_check(hash, password)) {
        return USERS_EAUTH;
    }

    *out = account;

    return USERS_OK;
}

/* --- the act ------------------------------------------------------------- */

int users_apply(const struct user_account *account, bool home)
{
    struct task *self = task_self();

    if (account == NULL || self == NULL) {
        return USERS_EPERM;
    }

    /*
     * The directory first, and as the person who is still root: a home that
     * the new user cannot read is still the directory of the shell, and a
     * task that dropped the uid before the walk would be refused by the mode
     * of its own directory.
     */
    if (home && account->home[0] != '\0') {
        struct vfs_stat info;

        if (vfs_stat(account->home, &info) == 0 && info.type == 2) {
            task_set_cwd(self, account->home);
        } else {
            kwarn("users: the home %s of %s is not a directory", account->home, account->name);
        }
    }

    task_set_creds(self, account->uid, account->gid);

    return USERS_OK;
}

/* --- the defaults -------------------------------------------------------- */

/* `dir` and `name` joined with a slash, when the path fits. */
static bool join_path(const char *dir, const char *name, char *path, k_size_t size)
{
    k_size_t dlen = strlen(dir);
    k_size_t nlen = strlen(name);
    bool slash = (dlen > 0 && dir[dlen - 1] != '/');

    if (dlen + (slash ? 1u : 0u) + nlen + 1 > size) {
        return false;
    }

    memcpy(path, dir, dlen);
    if (slash) {
        path[dlen++] = '/';
    }
    memcpy(path + dlen, name, nlen + 1);

    return true;
}

static bool file_is_there(const char *path)
{
    struct vfs_stat info;

    return vfs_stat(path, &info) == 0;
}

/* A text written into a file with a mode; 1 when it was written, 0 when the
 * file of a person is already there, a negative error otherwise. */
static int write_default(const char *path, const char *text, unsigned mode)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    int rc;

    if (file_is_there(path)) {
        return 0;
    }

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        kwarn("users: cannot write %s (rc=%d)", path, rc);
        return rc;
    }

    rc = vfs_write(&file, text, strlen(text));
    (void)vfs_close(&file);

    if (rc < 0) {
        kwarn("users: cannot write %s (rc=%d)", path, rc);
        return rc;
    }

    (void)vfs_chmod(path, mode);

    return 1;
}

int users_defaults(const char *dir)
{
    char path[VFS_PATH_MAX];
    char shadow[USERS_LINE_MAX];
    char hash[USERS_LINE_MAX];
    const char *at;
    struct field line;
    k_size_t used;
    int written = 0;
    int rc;

    if (dir == NULL) {
        return USERS_EIO;
    }

    rc = vfs_mkdir_p(dir);
    if (rc != 0) {
        kwarn("users: cannot create %s (rc=%d)", dir, rc);
        return USERS_EIO;
    }

    if (!join_path(dir, USERS_PASSWD_NAME, path, sizeof(path))) {
        return USERS_EIO;
    }

    rc = write_default(path, USERS_DEFAULT_PASSWD, 0644u);
    if (rc < 0) {
        return USERS_EIO;
    }
    written += rc;

    /*
     * The homes of the accounts of the default file, made when the file was
     * made: a person who logs in for the first time has a directory to land
     * in, and the directory is not made again after somebody removed it on
     * purpose.
     */
    if (rc > 0) {
        char record[USERS_LINE_MAX];

        at = USERS_DEFAULT_PASSWD;

        while (next_line(&at, &line)) {
            struct user_account account;

            if (line_is_noise(&line) || !line_text(&line, record, sizeof(record)) ||
                !users_parse_passwd(record, &account)) {
                continue;
            }
            if (account.home[0] != '\0' && strcmp(account.home, "/") != 0) {
                (void)vfs_mkdir_p(account.home);
            }
        }
    }

    if (!join_path(dir, USERS_SHADOW_NAME, path, sizeof(path))) {
        return USERS_EIO;
    }

    if (file_is_there(path)) {
        return written;
    }

    if (!users_hash_make(USERS_DEFAULT_PASSWORD, USERS_DEFAULT_SALT, hash, sizeof(hash))) {
        return USERS_EIO;
    }

    /* `root:<hash>` and an account without a password: `guest:*` is locked,
     * so only root becomes guest, and nobody logs in as guest at the console
     * until a person writes a hash of its own. */
    used = 0;
    memcpy(shadow, "root:", 5);
    used = 5;
    memcpy(shadow + used, hash, strlen(hash));
    used += strlen(hash);
    memcpy(shadow + used, "\nguest:*\n", 9);
    used += 9;
    shadow[used] = '\0';

    rc = write_default(path, shadow, 0600u);
    if (rc < 0) {
        return USERS_EIO;
    }

    return written + rc;
}
