/*
 * Micro OS — the accounts of the machine.
 *
 * A person is a line of two files of /etc. `/etc/passwd` names the account:
 * the name, the uid, the gid, the home directory and the shell, in the five
 * fields of this system (the password is not one of them, and there is no
 * GECOS field because nothing of this machine shows one). `/etc/shadow` keeps
 * the hash of the password and nothing else, so a program that lists the
 * accounts reads a file with nothing worth hiding and the hash is a file of
 * its own.
 *
 * The files are the truth: a name is not a table compiled into the kernel,
 * and a person who edits /etc/passwd with `edit` gets the new account at the
 * next `login`. The library of a program reads the same file
 * (`components/libc/user/src/pwd.c`), so `getpwnam` and `login` cannot
 * disagree about a name.
 *
 * The hash is salted SHA-256, `$sha256$<salt>$<64 hex digits>`, over the
 * bytes of the salt followed by the bytes of the password. It is not a
 * key-derivation function: a fast hash of a short password is a hash an
 * attacker with the file can try a dictionary against (docs/debts.md). The
 * format has room for a better one, and a line that is not `$sha256$...` is
 * refused instead of guessed at.
 */
#ifndef MICRO_OS_CONFIG_USERS_H
#define MICRO_OS_CONFIG_USERS_H

#include "kernel/types.h"

/* The two files of the accounts, and the file of the groups (ADR 0044). */
#define USERS_PASSWD_PATH "/etc/passwd"
#define USERS_SHADOW_PATH "/etc/shadow"
#define USERS_GROUP_PATH "/etc/group"

/* The limits of a record: a name of fifteen characters, a path of a home and
 * a shell of 47, a line of 159 and a file of 511 bytes. The file of a
 * machine of a few people is a few lines; one that does not fit is refused
 * rather than read in part, because a half read file of accounts is a
 * different set of users. */
#define USERS_NAME_MAX 16
#define USERS_FIELD_MAX 48
#define USERS_LINE_MAX 160
#define USERS_FILE_MAX 512

/* The answers of this file. They are not the errno codes of the kernel: a
 * caller of `login` says a sentence, and the sentence is chosen here. */
#define USERS_OK 0
#define USERS_ENOENT (-1)  /* no such account, or no line in /etc/shadow */
#define USERS_EAUTH (-2)   /* the password is not the one of the record */
#define USERS_EPERM (-3)   /* the caller may not become that user */
#define USERS_EFORMAT (-4) /* a line of a file is not a record */
#define USERS_EIO (-5)     /* the file cannot be read at all */

/* One line of /etc/passwd. */
struct user_account {
    char name[USERS_NAME_MAX];
    char home[USERS_FIELD_MAX];
    char shell[USERS_FIELD_MAX];
    k_u32_t uid;
    k_u32_t gid;
};

/*
 * One line of /etc/group, `name:gid:member,member` (ADR 0044 §4). The members
 * are kept as the line wrote them: a task has one gid and no supplementary
 * list, so nothing of this system reads the list, and a machine that has one
 * is a change of `struct task` and of every entrance. The format is the one
 * of POSIX so that the file of this machine is a file of every machine.
 */
struct user_group {
    char name[USERS_NAME_MAX];
    char members[USERS_FIELD_MAX];
    k_u32_t gid;
};

/* The default salt of a record this tree writes, and the password of root of
 * an image of this tree. They are the two halves of the recipe of the first
 * account: `printf 'mox1%s' microos | sha256sum` is the digest of it. */
#define USERS_DEFAULT_SALT "mox1"
#define USERS_DEFAULT_PASSWORD "microos"

/*
 * The text of the default /etc/passwd, for a boot that has none. The
 * /etc/shadow of that boot is made beside it: the hash of the password below
 * for root, and `*` for every other account of the file. /etc/group of that
 * boot names the primary group of each account with no member of its own,
 * because a member list is a list of supplementary groups and a task of this
 * system has none (ADR 0044 §4).
 */
#define USERS_DEFAULT_PASSWD                                                                       \
    "root:0:0:/:/bin/sh\n"                                                                         \
    "guest:7:7:/home/guest:/bin/sh\n"

#define USERS_DEFAULT_GROUP "root:0:\nguest:7:\n"

/*
 * One line of /etc/passwd into an account. The fields are
 * name:uid:gid:home:shell; an empty line and a line that begins with '#' are
 * not a record and answer false, and so does a line whose fields do not fit,
 * whose uid or gid is not a decimal number of fifteen bits, or that has a
 * sixth field. A name is not empty, has no '/' and no ':'.
 */
bool users_parse_passwd(const char *line, struct user_account *out);

/*
 * One line of /etc/shadow into a name and the hash of it. False when the line
 * is not `name:hash` or when either part does not fit.
 */
bool users_parse_shadow(const char *line, char *name, k_size_t name_size, char *hash,
                        k_size_t hash_size);

/*
 * One line of /etc/group into a group. The fields are `name:gid:members`; an
 * empty line and a line that begins with '#' are not a record, and neither is
 * a line with a fourth field, a name that does not fit or a gid that is not a
 * decimal number of fifteen bits. The members are a comma separated list of
 * names, each of the shape of a name, or an empty field for a group with no
 * member.
 */
bool users_parse_group(const char *line, struct user_group *out);

/* The digest of a password under a salt, as a line of /etc/shadow writes it:
 * `$sha256$<salt>$<64 hex digits>`. False when the answer does not fit. */
bool users_hash_make(const char *password, const char *salt, char *out, k_size_t size);

/*
 * Whether a password is the one of a hash of /etc/shadow. An empty hash, `*`
 * and a hash that begins with '!' are a locked account and answer false for
 * every password; a hash that is not of the form above answers false too. The
 * comparison of the digest does not stop at the first byte that differs.
 */
bool users_hash_check(const char *hash, const char *password);

/* The record of a name and the record of a uid; the first match of the file
 * wins. `out` is written only on USERS_OK. */
int users_find_name(const char *name, struct user_account *out);
int users_find_uid(k_u32_t uid, struct user_account *out);

/*
 * The record of a group, by name or by gid, from /etc/group and by the same
 * rule as the two above: a line that is not a record stops the read with
 * USERS_EFORMAT, because half of a file of groups is a different set of
 * groups. USERS_ENOENT when the file has no such line, which is not the same
 * as a group with no member.
 */
int users_find_group(const char *name, struct user_group *out);
int users_find_gid(k_u32_t gid, struct user_group *out);

/* The hash of a name, as /etc/shadow keeps it. USERS_ENOENT when the file has
 * no line of that name, which is not the same as an empty hash. */
int users_find_hash(const char *name, char *hash, k_size_t size);

/* The name and the password of a person against the two files: USERS_OK,
 * USERS_ENOENT (no such account) or USERS_EAUTH (a locked account or another
 * password). `out` is written only on USERS_OK. */
int users_authenticate(const char *name, const char *password, struct user_account *out);

/*
 * The act: the home directory of the account when `home` is true, then the
 * credentials of the task. The directory is entered first and as root, so
 * that a home the new user cannot read still becomes the directory of the
 * shell; the credentials go together (task_set_creds) and the gid is written
 * before the uid.
 */
int users_apply(const struct user_account *account, bool home);

/*
 * Write the two files of the accounts and the home directory of every account
 * into `dir` when they are not there. A file of a person is never replaced.
 * Returns the number of files written, or a negative error.
 */
int users_defaults(const char *dir);

/* Register `login`, `su` and `id` with the shell. The boot calls it with
 * config_register(); `passwd` is not written yet (docs/debts.md). */
int users_register(void);

#endif /* MICRO_OS_CONFIG_USERS_H */
