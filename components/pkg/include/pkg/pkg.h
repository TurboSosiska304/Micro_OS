/*
 * Micro OS — pkg, the installer of x86-64 (ADR 0024 §5 and §6).
 *
 * A program of this system is a file of `MOX1`, and a repository is a
 * directory of an HTTP server with an index and the files it names. `pkg`
 * fetches them, checks the index and the program against the signature of
 * the repository, and puts the program in /usr/bin. It is built for x86-64
 * only: the `progs` partition of the board belongs to the firmware (ADR
 * 0018), and the board does not link this component.
 *
 * The component is in two halves. The portable half - the index, the
 * digest, the signature, the database and the plan of an install - has no
 * socket in it and is tested on the host with a fake transport and a
 * temporary volume. The console half is the command of the shell, whose
 * names, short forms and flags are the table of ADR 0024 §6.
 */
#ifndef MICRO_OS_PKG_PKG_H
#define MICRO_OS_PKG_PKG_H

#include "kernel/exec.h"
#include "kernel/types.h"
#include "net/http.h"

/* The tree of the package manager, under the root of the call. */
#define PKG_DB_DIR "/var/pkg"
#define PKG_LOCAL_DIR "/var/pkg/local"
#define PKG_INDEX_DIR "/var/pkg/index"
#define PKG_LOG_FILE "/var/pkg/log"
#define PKG_CACHE_DIR "/var/cache/pkg"
#define PKG_BINDIR "/usr/bin"
#define PKG_CONF "/etc/pkg.conf"
#define PKG_KEY_DIR "/etc/pkg/keys"

/* The repository of the release when /etc/pkg.conf says nothing. */
#define PKG_DEFAULT_REPO "microos-progs"

/*
 * The machine and the ABI a package has to name. `pkg` is a program of the
 * x86-64 port, and the ABI is the version of the header of MOX1 that this
 * kernel reads (ADR 0018): a package of another machine or of a newer ABI
 * does not install, because the bytes would either not run or run as
 * something else.
 */
#define PKG_MACHINE "x86_64"
#define PKG_ABI K_EXEC_VERSION

/* The sizes of the names and of the strings of the index. */
#define PKG_NAME_MAX 32
#define PKG_VERSION_MAX 24
#define PKG_MACHINE_MAX 16
#define PKG_FILE_MAX 64
#define PKG_SUMMARY_MAX 64
#define PKG_LICENSE_MAX 64
#define PKG_SOURCE_MAX 128
#define PKG_SHA256_HEX 65

/* The entries of one index and the packages of one command line. */
#define PKG_INDEX_MAX 32
#define PKG_NAMES_MAX 8
#define PKG_IGNORE_MAX 8

/* The bytes of a signature (Ed25519) and of a key. */
#define PKG_SIGNATURE_SIZE 64
#define PKG_KEY_SIZE 32

/*
 * The largest package this installer takes: the ceiling of an image of a
 * program and not a number of its own, so that the installer and the loader
 * refuse the same file. A package is read into one block of the allocator,
 * which is why the ceiling is the memory of the machine and not the size of
 * one transfer, and why a hostile index cannot ask for more than eight
 * megabytes of it.
 *
 * The kernel names the ceiling in kernel/exec.h. A tree whose header does
 * not have it yet - the base of this branch - repeats the number here; the
 * guard puts the name in its place as soon as the kernel has it, so the two
 * cannot drift apart.
 */
#ifdef K_EXEC_MAX_IMAGE
#define PKG_PACKAGE_MAX K_EXEC_MAX_IMAGE
#else
#define PKG_PACKAGE_MAX (8u * 1024u * 1024u)
#endif

/* The answers of the calls of this component. */
#define PKG_OK 0
#define PKG_EPARSE (-1)     /* the index is not the JSON of a repository */
#define PKG_ENOENT (-2)     /* the name is not in the index */
#define PKG_EFULL (-3)      /* the index has more packages than PKG_INDEX_MAX */
#define PKG_ESIGN (-4)      /* the signature or the digest is not the one */
#define PKG_EMACHINE (-5)   /* the package is for another machine */
#define PKG_EABI (-6)       /* the package needs another ABI of the programs */
#define PKG_EDOWNGRADE (-7) /* an older version without --allow-downgrade */
#define PKG_EFETCH (-8)     /* the file did not arrive */
#define PKG_ESIZE (-9)      /* the size of the package is not the one promised */
#define PKG_EIO (-10)       /* the volume refused a call */
#define PKG_ENOMEM (-11)    /* the buffer of the package cannot be made */
#define PKG_EEXIST (-12)    /* the name is already there and is not replaced */
#define PKG_ELICENSE (-13)  /* a copyleft package without the source of it */

/*
 * One package of an index. `license` is the SPDX string of the package
 * ("MIT", "GPL-3.0-or-later"), `source` the URL of its source and
 * `source_sha256` the digest of the archive there: a copyleft package a
 * person cannot obtain the source of does not install (ADR 0025), so the
 * three travel with the entry. They are optional in the format - a
 * permissive package needs none of them - and empty when the repository says
 * nothing.
 */
struct pkg_entry {
    char name[PKG_NAME_MAX + 1];
    char version[PKG_VERSION_MAX + 1];
    char machine[PKG_MACHINE_MAX + 1];
    char file[PKG_FILE_MAX + 1];
    char summary[PKG_SUMMARY_MAX + 1];
    char license[PKG_LICENSE_MAX + 1];
    char source[PKG_SOURCE_MAX + 1];
    char source_sha256[PKG_SHA256_HEX];
    char sha256[PKG_SHA256_HEX];
    k_u32_t abi;
    k_u64_t size;
};

/* The index of one repository, as it is stored in /var/pkg/index. */
struct pkg_index {
    char repository[PKG_NAME_MAX + 1];
    char machine[PKG_MACHINE_MAX + 1];
    k_u32_t abi;
    k_size_t count;
    struct pkg_entry entries[PKG_INDEX_MAX];
};

/* One installed package, as /var/pkg/local keeps it. The license and the
 * source are kept as the index named them, so that a person asks the machine
 * what it runs with `pkg -Qi`; a record written before the field existed (or
 * by a local install, which has no index to read) leaves both empty. */
struct pkg_record {
    char name[PKG_NAME_MAX + 1];
    char version[PKG_VERSION_MAX + 1];
    char machine[PKG_MACHINE_MAX + 1];
    char file[PKG_FILE_MAX + 1];
    char license[PKG_LICENSE_MAX + 1];
    char source[PKG_SOURCE_MAX + 1];
    char sha256[PKG_SHA256_HEX];
    k_u32_t abi;
    k_u64_t size;
    bool explicit_install;
};

/*
 * The rule of the ceiling, in the one place that keeps it: PKG_OK when a
 * package of `size` bytes may be read and installed, PKG_ESIZE when it is
 * past PKG_PACKAGE_MAX. The install of a file, the install of a name from
 * the index and the parse of the index ask this and not a copy of the rule;
 * the parse answers PKG_EPARSE of its own, because a document that names a
 * package this machine cannot hold is not the index of a repository.
 */
int pkg_size_check(k_u64_t size);

/*
 * Read the JSON of an index. The object is the one of the format of ADR
 * 0024: repository, machine, abi, and the array `packages`, each with name,
 * version, machine, abi, size, sha256, file and summary, and the optional
 * license, source and source_sha256 of ADR 0025. A field this parser does
 * not know is skipped, so a repository grows a field without breaking older
 * machines; a field that is needed and missing, a wrong type, or a digest
 * that is not 64 hexadecimal digits is PKG_EPARSE. The three fields of ADR
 * 0025 are optional here - the rule of the install is the one that reads
 * them, because a repository of permissive programs has no source to name.
 */
int pkg_index_parse(const char *text, k_size_t size, struct pkg_index *out);

/* The place of a name in an index, -1 when it is not there. */
int pkg_index_find(const struct pkg_index *index, const char *name);

/*
 * The index of a repository as it was stored by `pkg -Sy`: the bytes of
 * /var/pkg/index/<repo>.json, verified against /var/pkg/index/<repo>.sig
 * and the key of the repository. PKG_ENOENT when no index was stored.
 */
int pkg_index_load(const char *repo, struct pkg_index *out);

/* The console command. `pkg_main` is what the shell calls; the tests call
 * it as well, with a temporary volume under --root. */
int pkg_register(void);
int pkg_main(int argc, char **argv);

/*
 * The transport of the downloads. The port fills it with its sockets; the
 * tests put a fake one there. NULL is "there is no network", which a command
 * with --offline or a cached file does not need.
 */
void pkg_set_transport(const struct http_transport *transport);
const struct http_transport *pkg_transport(void);

/* The key of a repository: /etc/pkg/keys/<repo>.pub, or the key of the
 * image when the file is not there. NULL when neither exists. */
const k_u8_t *pkg_repository_key(const char *repo);

/* The digest of a buffer, as sixty-four hexadecimal digits. */
void pkg_sha256_hex(const void *data, k_size_t size, char hex[PKG_SHA256_HEX]);

/* True when the text is sixty-four hexadecimal digits: the digest of a file
 * of the repository, a package or the archive of its source. */
bool pkg_digest_is_hex(const char *text);

/* 0 when the signature is the one of the bytes and the key. */
int pkg_signature_check(const k_u8_t key[PKG_KEY_SIZE], const void *data, k_size_t size,
                        const k_u8_t signature[PKG_SIGNATURE_SIZE]);

#endif /* MICRO_OS_PKG_PKG_H */
