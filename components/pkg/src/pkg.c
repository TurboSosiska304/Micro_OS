/*
 * Micro OS — `pkg`, the console of the installer (ADR 0024 §6).
 *
 * The names, the short forms and the flags are the table of the ADR: the
 * muscle memory of pacman without the build of yay, because a program of
 * this system is static MOX1 and an install script would be the code of a
 * stranger run as root. The three things the ADR refuses are visible here:
 * no flag turns the signature off, no dependency is solved, and nothing of a
 * package runs at install time.
 *
 * The command is one file of the shell today. When the calls it needs are
 * all in the table of the system calls it becomes a program of /usr/bin, and
 * the engine below is already the one that would move with it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/version.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "net/net.h"
#include "net/tls.h"
#include "pkg/pkg.h"
#include "pkg_private.h"
#include "sha256.h"

#define PKG_URL_MAX 192
#define PKG_PATH_MAX VFS_PATH_MAX

/* The colour of the `::` lines: always, never, or only when asked for. */
#define PKG_COLOR_AUTO 0
#define PKG_COLOR_ALWAYS 1
#define PKG_COLOR_NEVER 2

static const struct http_transport *g_pkg_transport;

/*
 * The index of a repository and the list of the installed packages live in
 * the component and not on the stack of the command: one index is
 * PKG_INDEX_MAX entries of a few hundred bytes, and the stack of the shell
 * task is 16 KB (it holds the console, the client of HTTP and the
 * filesystem below it). The component is one command of one shell, exactly
 * like the volume it writes.
 */
static struct pkg_index g_index;
static struct pkg_record g_installed[PKG_INDEX_MAX];

void pkg_set_transport(const struct http_transport *transport)
{
    g_pkg_transport = transport;
}

const struct http_transport *pkg_transport(void)
{
    if (g_pkg_transport == NULL) {
        g_pkg_transport = net_http_transport();
    }

    return g_pkg_transport;
}

/* --- the options of the command line --- */

enum pkg_command {
    PKG_CMD_NONE = 0,
    PKG_CMD_INSTALL, /* -S */
    PKG_CMD_LOCAL,   /* -U */
    PKG_CMD_REMOVE   /* -R */
};

struct pkg_options {
    enum pkg_command command;

    bool sync;
    bool upgrade;
    bool query;
    bool info;
    bool search;
    bool files;
    bool check;
    bool clean;
    bool remove_files;
    bool explicit_only;

    bool noconfirm;
    bool needed;
    bool download_only;
    bool dry_run;
    bool allow_downgrade;
    bool force;
    bool offline;
    bool help;
    bool version;
    bool quiet;
    bool verbose;

    int color;
    const char *root;
    const char *repo;
    const char *cachedir;

    const char *names[PKG_NAMES_MAX];
    k_size_t name_count;
    const char *ignore[PKG_IGNORE_MAX];
    k_size_t ignore_count;
};

static void usage(void)
{
    kshell_printf("usage: pkg <operation> [options] [name...]\n"
                  "\n"
                  "  sync                     -Sy   refresh the index of every repository\n"
                  "  upgrade                  -Su   a newer version of every installed package\n"
                  "  sync --upgrade           -Syu  both, in that order\n"
                  "  install NAME...          -S    fetch, verify and install\n"
                  "  install ./x.mox          -U    the same for a file on the machine\n"
                  "  remove NAME...           -R    remove (-Rn also its files)\n"
                  "  search TERM              -Ss   the name and the summary of a match\n"
                  "  info NAME                -Si / -Qi  the package in a repository / installed\n"
                  "  list                     -Q    the installed packages (-Qe explicit only)\n"
                  "  files NAME               -Ql   the files a package installed\n"
                  "  check                    -Qk   the digest of every installed file\n"
                  "  clean                    -Sc   the cache\n"
                  "  repo add URL             a repository and its key, in /etc/pkg.conf\n"
                  "  log                      what was installed, removed and when\n"
                  "\n"
                  "  --noconfirm --needed --download-only --dry-run --allow-downgrade\n"
                  "  --ignore NAME --force --offline --root DIR --repo NAME --cachedir DIR\n"
                  "  --color auto|always|never --quiet --verbose --help --version\n");
}

static bool option_value(int argc, char **argv, int *index, const char *inline_value,
                         const char **out)
{
    if (inline_value != NULL) {
        *out = inline_value;
        return true;
    }

    if (*index + 1 >= argc) {
        return false;
    }

    (*index)++;
    *out = argv[*index];

    return true;
}

/* A long option: the name, the value of "=VALUE", or NULL when it is not
 * this one. A name of the table that is a prefix of another is not a match:
 * "=" or the end of the argument closes it. */
static const char *long_option(const char *arg, const char *name, bool *inline_value)
{
    k_size_t len = strlen(name);

    if (strncmp(arg, name, len) != 0) {
        return NULL;
    }

    if (arg[len] == '=') {
        *inline_value = true;
        return arg + len + 1;
    }

    if (arg[len] == '\0') {
        *inline_value = false;
        return arg + len;
    }

    return NULL;
}

static bool parse_long(struct pkg_options *options, int argc, char **argv, int *index)
{
    const char *arg = argv[*index];
    const char *value = NULL;
    bool inline_value = false;
    const struct {
        const char *name;
        bool *flag;
    } flags[] = {
        {"--noconfirm", &options->noconfirm},
        {"--needed", &options->needed},
        {"--download-only", &options->download_only},
        {"--dry-run", &options->dry_run},
        {"--allow-downgrade", &options->allow_downgrade},
        {"--force", &options->force},
        {"--offline", &options->offline},
        {"--quiet", &options->quiet},
        {"--verbose", &options->verbose},
    };
    k_size_t i;

    if (strcmp(arg, "--help") == 0) {
        options->help = true;
        return true;
    }

    if (strcmp(arg, "--version") == 0) {
        options->version = true;
        return true;
    }

    for (i = 0; i < K_ARRAY_SIZE(flags); i++) {
        if (long_option(arg, flags[i].name, &inline_value) == NULL) {
            continue;
        }

        if (inline_value) {
            return false;
        }

        *flags[i].flag = true;

        return true;
    }

    if (long_option(arg, "--ignore", &inline_value) != NULL) {
        if (!option_value(argc, argv, index, inline_value ? arg + strlen("--ignore=") : NULL,
                          &value) ||
            options->ignore_count >= PKG_IGNORE_MAX) {
            return false;
        }

        options->ignore[options->ignore_count++] = value;
        return true;
    }

    if (long_option(arg, "--root", &inline_value) != NULL) {
        if (!option_value(argc, argv, index, inline_value ? arg + strlen("--root=") : NULL,
                          &value)) {
            return false;
        }

        options->root = value;
        return true;
    }

    if (long_option(arg, "--repo", &inline_value) != NULL) {
        if (!option_value(argc, argv, index, inline_value ? arg + strlen("--repo=") : NULL,
                          &value)) {
            return false;
        }

        options->repo = value;
        return true;
    }

    if (long_option(arg, "--cachedir", &inline_value) != NULL) {
        if (!option_value(argc, argv, index, inline_value ? arg + strlen("--cachedir=") : NULL,
                          &value)) {
            return false;
        }

        options->cachedir = value;
        return true;
    }

    if (long_option(arg, "--color", &inline_value) != NULL) {
        if (!option_value(argc, argv, index, inline_value ? arg + strlen("--color=") : NULL,
                          &value)) {
            return false;
        }

        if (strcmp(value, "always") == 0) {
            options->color = PKG_COLOR_ALWAYS;
        } else if (strcmp(value, "never") == 0) {
            options->color = PKG_COLOR_NEVER;
        } else if (strcmp(value, "auto") == 0) {
            options->color = PKG_COLOR_AUTO;
        } else {
            return false;
        }

        return true;
    }

    return false;
}

static bool parse_short(struct pkg_options *options, const char *arg)
{
    k_size_t i;

    for (i = 1; arg[i] != '\0'; i++) {
        switch (arg[i]) {
        case 'S':
            options->command = PKG_CMD_INSTALL;
            break;
        case 'y':
            options->sync = true;
            break;
        case 'u':
            options->upgrade = true;
            break;
        case 'U':
            options->command = PKG_CMD_LOCAL;
            break;
        case 'R':
            options->command = PKG_CMD_REMOVE;
            break;
        case 'Q':
            options->query = true;
            break;
        case 'i':
            options->info = true;
            break;
        case 's':
            options->search = true;
            break;
        case 'l':
            options->files = true;
            break;
        case 'k':
            options->check = true;
            break;
        case 'c':
            options->clean = true;
            break;
        case 'e':
            options->explicit_only = true;
            break;
        case 'n':
            options->remove_files = true;
            break;
        case 'h':
            options->help = true;
            break;
        default:
            return false;
        }
    }

    return true;
}

static bool parse_arguments(struct pkg_options *options, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strncmp(arg, "--", 2) == 0) {
            if (!parse_long(options, argc, argv, &i)) {
                return false;
            }

            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            if (!parse_short(options, arg)) {
                return false;
            }

            continue;
        }

        if (options->name_count >= PKG_NAMES_MAX) {
            return false;
        }

        options->names[options->name_count++] = arg;
    }

    return true;
}

/* --- the repository and its files --- */

static int repository_url(const char *repo, char *out, k_size_t size)
{
    char text[1024];
    k_size_t got = 0;
    k_size_t start = 0;
    k_size_t i;

    if (repo == NULL || out == NULL) {
        return PKG_ENOENT;
    }

    if (pkg_read(PKG_CONF, text, sizeof(text) - 1u, &got) != PKG_OK) {
        return PKG_ENOENT;
    }

    text[got] = '\0';

    for (i = 0; i <= got; i++) {
        if (i < got && text[i] != '\n') {
            continue;
        }

        text[i] = '\0';

        {
            char *line = text + start;
            char *space = strchr(line, ' ');
            char *url = NULL;

            if (line[0] != '#' && space != NULL) {
                *space = '\0';
                url = space + 1;

                while (*url == ' ' || *url == '\t') {
                    url++;
                }

                if (strcmp(line, repo) == 0 && *url != '\0') {
                    snprintf(out, size, "%s", url);
                    return PKG_OK;
                }
            }
        }

        start = i + 1;
    }

    return PKG_ENOENT;
}

static void repository_address(char *out, k_size_t size, const char *base, const char *file)
{
    k_size_t len = strlen(base);
    bool slash = len > 0 && base[len - 1] == '/';

    snprintf(out, size, "%s%s%s", base, slash ? "" : "/", file);
}

static const char *default_cache(void)
{
    return PKG_CACHE_DIR;
}

static const char *selected_repo(const struct pkg_options *options)
{
    return options->repo != NULL ? options->repo : PKG_DEFAULT_REPO;
}

/* --- the index of a repository, stored and verified --- */

int pkg_index_load(const char *repo, struct pkg_index *out)
{
    static char text[PKG_INDEX_TEXT_MAX + 1];
    char json_path[PKG_PATH_MAX];
    char sig_path[PKG_PATH_MAX];
    k_u8_t signature[PKG_SIGNATURE_SIZE];
    const k_u8_t *key;
    k_size_t got = 0;
    int nc;
    int rc;

    if (repo == NULL || out == NULL) {
        return PKG_EPARSE;
    }

    nc = snprintf(json_path, sizeof(json_path), "%s/%s.json", PKG_INDEX_DIR, repo);
    if (nc < 0 || (k_size_t)nc >= sizeof(json_path)) {
        return PKG_EPARSE;
    }

    nc = snprintf(sig_path, sizeof(sig_path), "%s/%s.json.sig", PKG_INDEX_DIR, repo);
    if (nc < 0 || (k_size_t)nc >= sizeof(sig_path)) {
        return PKG_EPARSE;
    }

    rc = pkg_read(json_path, text, PKG_INDEX_TEXT_MAX, &got);
    if (rc != PKG_OK) {
        return rc;
    }

    text[got] = '\0';

    if (pkg_signature_load(sig_path, signature) != PKG_OK) {
        return PKG_ESIGN;
    }

    key = pkg_repository_key(repo);
    if (key == NULL) {
        return PKG_ESIGN;
    }

    rc = pkg_signature_check(key, text, got, signature);
    if (rc != PKG_OK) {
        return rc;
    }

    return pkg_index_parse(text, got, out);
}

/* --- the plan of an install --- */

struct pkg_plan {
    struct pkg_entry entry;
    struct pkg_record installed;
    bool have_installed;
    bool upgrade;
    bool downgrade;
    bool reinstall;
};

static bool ignored(const struct pkg_options *options, const char *name)
{
    k_size_t i;

    for (i = 0; i < options->ignore_count; i++) {
        if (strcmp(options->ignore[i], name) == 0) {
            return true;
        }
    }

    return false;
}

/* The answer of a failed call, in one sentence. */
static void say_error(int code)
{
    switch (code) {
    case PKG_EPARSE:
        pkg_error("the index is not one this machine reads");
        break;
    case PKG_ENOENT:
        pkg_error("no such package");
        break;
    case PKG_EFULL:
        pkg_error("the index has more packages than this machine keeps");
        break;
    case PKG_ESIGN:
        pkg_error("the signature is not the one of the repository");
        break;
    case PKG_EMACHINE:
        pkg_error("the package is for another machine");
        break;
    case PKG_EABI:
        pkg_error("the package needs another version of the programs");
        break;
    case PKG_EDOWNGRADE:
        pkg_error("a downgrade needs --allow-downgrade");
        break;
    case PKG_EFETCH:
        pkg_error("the file did not arrive");
        break;
    case PKG_ESIZE:
        pkg_error("the file is not the size the index promises");
        break;
    case PKG_ENOMEM:
        pkg_error("there is no memory for the package");
        break;
    case PKG_EEXIST:
        pkg_error("the name is already there");
        break;
    default:
        pkg_error("the volume refused the call");
        break;
    }
}

static int plan_for(const struct pkg_options *options, const struct pkg_entry *entry,
                    struct pkg_plan *plan)
{
    int comparison;

    memset(plan, 0, sizeof(*plan));
    plan->entry = *entry;

    if (pkg_db_read(entry->name, &plan->installed) != PKG_OK) {
        return PKG_OK;
    }

    plan->have_installed = true;
    comparison = kernel_version_compare(entry->version, plan->installed.version);

    if (comparison == 0) {
        plan->reinstall = true;
    } else if (comparison > 0) {
        plan->upgrade = true;
    } else {
        plan->downgrade = true;

        if (!options->allow_downgrade) {
            return PKG_EDOWNGRADE;
        }
    }

    return PKG_OK;
}

/* --- the bytes of one package --- */

static int download_to(const char *url, const char *path)
{
    struct http_stream stream;
    int rc;

    pkg_say("download %s", url);

    rc = net_http_download_to(pkg_transport(), url, path, false, &stream);
    if (rc != HTTP_OK) {
        if (rc == HTTP_TLS) {
            /* The reason of net/tls.h: the roots, the clock, the chain or
             * the name. A mirror over https that this machine cannot trust
             * is a sentence and not a bare "download failed". */
            pkg_error("%s", net_tls_last_error());
        } else if (rc == HTTP_ERROR && stream.status != 0) {
            pkg_error("the server answered %d", stream.status);
        } else {
            pkg_error("the download failed");
        }

        return PKG_EFETCH;
    }

    if (g_pkg_verbose) {
        pkg_say("%llu byte(s)", (unsigned long long)stream.length);
    }

    return PKG_OK;
}

/* A file of the repository to a path of the tree, made visible by a rename:
 * half an index is not an index. */
static int fetch_to(const char *url, const char *path)
{
    char temp[PKG_PATH_MAX];
    char temp_real[PKG_PATH_MAX];
    int nc;
    int rc;

    nc = snprintf(temp, sizeof(temp), "%s.new", path);
    if (nc < 0 || (k_size_t)nc >= sizeof(temp) ||
        pkg_join(temp_real, sizeof(temp_real), temp) != PKG_OK) {
        return PKG_EIO;
    }

    (void)pkg_remove(temp);

    rc = download_to(url, temp_real);
    if (rc != PKG_OK) {
        (void)pkg_remove(temp);
        return rc;
    }

    rc = pkg_rename(temp, path);
    if (rc != PKG_OK) {
        (void)pkg_remove(temp);
        return PKG_EIO;
    }

    return PKG_OK;
}

/*
 * The bytes verified arrive at the name of the program in one step: the file
 * is written beside it and a rename makes it visible. A volume without
 * rename still installs, and says that the window is not closed (ADR 0024
 * §5) - the bytes are still verified before the old program is touched.
 */
static int commit_file(const char *target, const void *data, k_size_t size)
{
    char temp[PKG_PATH_MAX];
    int nc;
    int rc;

    nc = snprintf(temp, sizeof(temp), "%s.new", target);
    if (nc < 0 || (k_size_t)nc >= sizeof(temp)) {
        return PKG_EIO;
    }

    (void)pkg_remove(temp);

    rc = pkg_write(temp, data, size);
    if (rc != PKG_OK) {
        (void)pkg_remove(temp);
        return rc;
    }

    rc = pkg_rename(temp, target);

    if (rc == PKG_OK) {
        return PKG_OK;
    }

    (void)pkg_remove(temp);

    if (rc == KERR_NOTSUP || rc == KERR_XDEV || rc == KERR_INVAL) {
        pkg_warn("this volume cannot rename: %s is replaced in place", target);
        return pkg_write(target, data, size);
    }

    return rc;
}

static int write_record(const struct pkg_plan *plan)
{
    struct pkg_record record;

    if (pkg_mkdir_p(PKG_LOCAL_DIR) != PKG_OK) {
        return PKG_EIO;
    }

    memset(&record, 0, sizeof(record));
    memcpy(record.name, plan->entry.name, sizeof(record.name));
    memcpy(record.version, plan->entry.version, sizeof(record.version));
    memcpy(record.machine, plan->entry.machine, sizeof(record.machine));
    memcpy(record.file, plan->entry.file, sizeof(record.file));
    memcpy(record.sha256, plan->entry.sha256, sizeof(record.sha256));
    record.abi = plan->entry.abi;
    record.size = plan->entry.size;
    record.explicit_install = true;

    return pkg_db_write(&record);
}

static int verify_and_install(struct pkg_options *options, const struct pkg_plan *plan,
                              const char *source, const char *repo)
{
    char signature_path[PKG_PATH_MAX];
    k_u8_t signature[PKG_SIGNATURE_SIZE];
    const k_u8_t *key;
    char target[PKG_PATH_MAX];
    k_u8_t *data;
    k_size_t size;
    k_u64_t file_size = 0;
    int nc;
    int rc;

    rc = pkg_size_at(source, &file_size);
    if (rc != PKG_OK) {
        pkg_error("%s is not there", source);
        return PKG_EFETCH;
    }

    if (pkg_size_check(file_size) != PKG_OK) {
        pkg_error("%s is larger than a package of this machine (%u MiB)", source,
                  (unsigned)(PKG_PACKAGE_MAX >> 20));
        return PKG_ESIZE;
    }

    size = (k_size_t)file_size;
    data = kmm_alloc(size, KMM_PRIORITY_SYSTEM);
    if (data == NULL) {
        return PKG_ENOMEM;
    }

    rc = pkg_read_at(source, data, size, &size);
    if (rc != PKG_OK) {
        kmm_free(data);
        return rc;
    }

    nc = snprintf(signature_path, sizeof(signature_path), "%s.sig", source);
    if (nc < 0 || (k_size_t)nc >= sizeof(signature_path)) {
        kmm_free(data);
        return PKG_EIO;
    }

    {
        k_size_t signature_size = 0;

        if (pkg_read_at(signature_path, signature, PKG_SIGNATURE_SIZE, &signature_size) != PKG_OK ||
            signature_size != PKG_SIGNATURE_SIZE) {
            pkg_error("%s has no signature", source);
            kmm_free(data);
            return PKG_ESIGN;
        }
    }

    key = pkg_repository_key(repo);
    if (key == NULL) {
        pkg_error("there is no key for %s", repo);
        kmm_free(data);
        return PKG_ESIGN;
    }

    rc = pkg_package_check(&plan->entry, signature, key, data, size);
    if (rc != PKG_OK) {
        kmm_free(data);
        say_error(rc);
        return rc;
    }

    pkg_say("signature ok");

    if (options->download_only) {
        kmm_free(data);
        return PKG_OK;
    }

    if (pkg_mkdir_p(PKG_BINDIR) != PKG_OK) {
        kmm_free(data);
        return PKG_EIO;
    }

    nc = snprintf(target, sizeof(target), "%s/%s", PKG_BINDIR, plan->entry.name);
    if (nc < 0 || (k_size_t)nc >= sizeof(target)) {
        kmm_free(data);
        return PKG_EIO;
    }

    rc = commit_file(target, data, size);
    kmm_free(data);

    if (rc != PKG_OK) {
        say_error(rc);
        return rc;
    }

    /*
     * A program of the system is one the shell runs: the mode of a new file
     * is 0644 and the search of PATH asks for the execute bits. The mode is
     * set after the rename, so what is visible is never a file that is a
     * program and cannot run.
     */
    if (pkg_chmod(target, 0755u) != PKG_OK) {
        pkg_warn("%s cannot be made executable", target);
    }

    if (write_record(plan) != PKG_OK) {
        pkg_warn("the record of %s was not written", plan->entry.name);
    }

    pkg_log("install", plan->entry.name, plan->entry.version);
    pkg_say("installed %s (%s)", target, plan->entry.version);

    return PKG_OK;
}

/* The package of an index entry in the cache, fetched when it is not there.
 * `source` is the real path of the file, root and all. */
static int cache_package(struct pkg_options *options, const struct pkg_plan *plan, const char *repo,
                         char *source, k_size_t size)
{
    const char *cachedir = options->cachedir != NULL ? options->cachedir : default_cache();
    char base[PKG_URL_MAX];
    char url[PKG_URL_MAX];
    char remote[PKG_URL_MAX];
    char tree[PKG_PATH_MAX];
    char signature[PKG_PATH_MAX];
    int nc;
    int rc;

    nc = snprintf(tree, sizeof(tree), "%s/%s", cachedir, plan->entry.file);
    if (nc < 0 || (k_size_t)nc >= sizeof(tree)) {
        return PKG_EIO;
    }

    nc = snprintf(signature, sizeof(signature), "%s.sig", tree);
    if (nc < 0 || (k_size_t)nc >= sizeof(signature)) {
        return PKG_EIO;
    }

    if (pkg_exists(tree)) {
        return pkg_join(source, size, tree);
    }

    if (options->offline) {
        pkg_error("%s is not in the cache and --offline was given", plan->entry.file);
        return PKG_EFETCH;
    }

    rc = repository_url(repo, base, sizeof(base));
    if (rc != PKG_OK) {
        pkg_error("there is no repository %s in %s", repo, PKG_CONF);
        return rc;
    }

    if (pkg_mkdir_p(cachedir) != PKG_OK) {
        return PKG_EIO;
    }

    repository_address(url, sizeof(url), base, plan->entry.file);
    rc = fetch_to(url, tree);
    if (rc != PKG_OK) {
        return rc;
    }

    if (!pkg_exists(signature)) {
        nc = snprintf(remote, sizeof(remote), "%s.sig", plan->entry.file);
        if (nc < 0 || (k_size_t)nc >= sizeof(remote)) {
            return PKG_EIO;
        }

        repository_address(url, sizeof(url), base, remote);

        rc = fetch_to(url, signature);
        if (rc != PKG_OK) {
            return rc;
        }
    }

    return pkg_join(source, size, tree);
}

static void say_plan(const struct pkg_plan *plan)
{
    if (plan->downgrade) {
        pkg_warn("%s: a downgrade %s -> %s", plan->entry.name, plan->installed.version,
                 plan->entry.version);
    }

    if (!plan->have_installed) {
        pkg_say("install %s %s", plan->entry.name, plan->entry.version);
    } else if (plan->upgrade) {
        pkg_say("upgrade %s %s -> %s", plan->entry.name, plan->installed.version,
                plan->entry.version);
    } else if (plan->reinstall) {
        pkg_say("reinstall %s %s", plan->entry.name, plan->entry.version);
    }
}

static int install_entry(struct pkg_options *options, const char *repo,
                         const struct pkg_entry *entry)
{
    struct pkg_plan plan;
    char source[PKG_PATH_MAX];
    int rc;

    if (strcmp(entry->machine, PKG_MACHINE) != 0) {
        pkg_error("%s is for %s, this machine is %s", entry->name, entry->machine, PKG_MACHINE);
        return PKG_EMACHINE;
    }

    if (entry->abi != PKG_ABI) {
        pkg_error("%s needs the ABI %u, this kernel speaks %u", entry->name, (unsigned)entry->abi,
                  (unsigned)PKG_ABI);
        return PKG_EABI;
    }

    rc = plan_for(options, entry, &plan);
    if (rc != PKG_OK) {
        say_error(rc);
        return rc;
    }

    if (plan.have_installed && !plan.downgrade) {
        if (plan.reinstall && options->needed) {
            pkg_say("%s is up to date", entry->name);
            return PKG_OK;
        }

        if (plan.reinstall && !options->force) {
            pkg_say("%s is up to date (--force reinstalls it)", entry->name);
            return PKG_OK;
        }
    }

    say_plan(&plan);

    if (options->dry_run) {
        return PKG_OK;
    }

    rc = cache_package(options, &plan, repo, source, sizeof(source));
    if (rc != PKG_OK) {
        return rc;
    }

    return verify_and_install(options, &plan, source, repo);
}

/* --- the names of the command line --- */

/* The version of a name as "NAME=VERSION", or the name and a NULL pin. */
static const char *name_without_pin(const char *arg, const char **pin)
{
    static char name[PKG_NAME_MAX + 1];
    const char *equals = strchr(arg, '=');
    k_size_t len;

    *pin = NULL;

    if (equals == NULL) {
        return arg;
    }

    len = (k_size_t)(equals - arg);
    if (len == 0 || len > PKG_NAME_MAX) {
        return arg;
    }

    memcpy(name, arg, len);
    name[len] = '\0';
    *pin = equals + 1;

    return name;
}

static int load_index_or_say(const char *repo, struct pkg_index *index)
{
    int rc = pkg_index_load(repo, index);

    if (rc == PKG_ENOENT) {
        pkg_error("there is no index of %s: run pkg -Sy", repo);
    } else if (rc != PKG_OK) {
        say_error(rc);
    }

    return rc;
}

/* --- the commands --- */

static int command_install(struct pkg_options *options)
{
    struct pkg_index *index = &g_index;
    const char *repo = selected_repo(options);
    k_size_t i;

    if (options->name_count == 0) {
        usage();
        return KSHELL_USAGE;
    }

    if (load_index_or_say(repo, index) != PKG_OK) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < options->name_count; i++) {
        const char *pin = NULL;
        const char *name = name_without_pin(options->names[i], &pin);
        int found = pkg_index_find(index, name);

        if (found < 0) {
            pkg_error("%s is not in the index of %s", name, repo);
            return KSHELL_ERROR;
        }

        if (pin != NULL && strcmp(index->entries[found].version, pin) != 0) {
            pkg_error("%s is %s in the index, not %s", name, index->entries[found].version, pin);
            return KSHELL_ERROR;
        }

        if (install_entry(options, repo, &index->entries[found]) != PKG_OK) {
            return KSHELL_ERROR;
        }
    }

    return KSHELL_OK;
}

/*
 * A file of the machine is named as a repository names it:
 * NAME-VERSION-MACHINE.mox, and its signature is beside it with `.sig`. The
 * name is what a person copies, and the shape of it is what lets the
 * installer know what it has without an index.
 */
static int local_entry(const char *path, struct pkg_entry *entry, char *signature,
                       k_size_t signature_size)
{
    const char *leaf = strrchr(path, '/');
    char stem[PKG_FILE_MAX + 1];
    char *first;
    char *second;
    const char *machine;
    const char *version;
    k_size_t size;
    int nc;
    int rc;

    leaf = leaf != NULL ? leaf + 1 : path;
    size = strlen(leaf);

    if (size <= strlen(".mox") || strcmp(leaf + size - 4, ".mox") != 0) {
        pkg_error("%s is not a .mox file", path);
        return PKG_EPARSE;
    }

    if (size >= sizeof(stem)) {
        return PKG_EPARSE;
    }

    memcpy(stem, leaf, size + 1);
    stem[size - 4] = '\0';

    second = strrchr(stem, '-');
    if (second == NULL) {
        pkg_error("%s is not NAME-VERSION-MACHINE.mox", leaf);
        return PKG_EPARSE;
    }

    machine = second + 1;
    *second = '\0';

    first = strrchr(stem, '-');
    if (first == NULL) {
        pkg_error("%s is not NAME-VERSION-MACHINE.mox", leaf);
        return PKG_EPARSE;
    }

    version = first + 1;
    *first = '\0';

    if (strcmp(machine, PKG_MACHINE) != 0) {
        pkg_error("%s is for %s, this machine is %s", leaf, machine, PKG_MACHINE);
        return PKG_EMACHINE;
    }

    if (strlen(stem) > PKG_NAME_MAX || strlen(version) > PKG_VERSION_MAX ||
        strlen(leaf) > PKG_FILE_MAX) {
        pkg_error("%s is a longer name than a package has", leaf);
        return PKG_EPARSE;
    }

    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, stem, strlen(stem) + 1);
    memcpy(entry->version, version, strlen(version) + 1);
    memcpy(entry->machine, machine, strlen(machine) + 1);
    memcpy(entry->file, leaf, strlen(leaf) + 1);
    memcpy(entry->summary, "a file of this machine", sizeof("a file of this machine"));
    entry->abi = PKG_ABI;

    {
        k_u8_t *data;
        k_size_t got;

        rc = pkg_size_at(path, &entry->size);
        if (rc != PKG_OK) {
            pkg_error("%s is not there", path);
            return rc;
        }

        if (pkg_size_check(entry->size) != PKG_OK) {
            pkg_error("%s is larger than a package of this machine (%u MiB)", leaf,
                      (unsigned)(PKG_PACKAGE_MAX >> 20));
            return PKG_ESIZE;
        }

        data = kmm_alloc((k_size_t)entry->size, KMM_PRIORITY_SYSTEM);
        if (data == NULL) {
            return PKG_ENOMEM;
        }

        got = (k_size_t)entry->size;

        if (pkg_read_at(path, data, got, &got) != PKG_OK) {
            kmm_free(data);
            return PKG_EIO;
        }

        pkg_sha256_hex(data, got, entry->sha256);
        kmm_free(data);
    }

    nc = snprintf(signature, signature_size, "%s.sig", path);
    if (nc < 0 || (k_size_t)nc >= signature_size) {
        return PKG_EPARSE;
    }

    return PKG_OK;
}

static int command_local(struct pkg_options *options)
{
    k_size_t i;

    if (options->name_count == 0) {
        usage();
        return KSHELL_USAGE;
    }

    for (i = 0; i < options->name_count; i++) {
        struct pkg_entry entry;
        struct pkg_plan plan;
        char signature[PKG_PATH_MAX];
        char real[PKG_PATH_MAX];
        char joined[PKG_PATH_MAX];
        int rc;

        /*
         * The file is named by the person and not by the root of the
         * install: it is resolved against the directory of the shell and
         * only takes the root when it does not already begin with it.
         */
        if (!kshell_resolve_path(options->names[i], real, sizeof(real))) {
            pkg_error("%s is too long", options->names[i]);
            return KSHELL_ERROR;
        }

        if (g_pkg_root[0] != '\0' && strncmp(real, g_pkg_root, strlen(g_pkg_root)) != 0) {
            int nc = snprintf(joined, sizeof(joined), "%s%s", g_pkg_root, real);

            if (nc < 0 || (k_size_t)nc >= sizeof(joined)) {
                return KSHELL_ERROR;
            }

            memcpy(real, joined, strlen(joined) + 1);
        }

        rc = local_entry(real, &entry, signature, sizeof(signature));
        if (rc != PKG_OK) {
            say_error(rc);
            return KSHELL_ERROR;
        }

        rc = plan_for(options, &entry, &plan);
        if (rc != PKG_OK) {
            say_error(rc);
            return KSHELL_ERROR;
        }

        if (plan.have_installed && plan.reinstall && !options->force) {
            pkg_say("%s is up to date (--force reinstalls it)", entry.name);
            continue;
        }

        say_plan(&plan);

        if (options->dry_run) {
            continue;
        }

        if (verify_and_install(options, &plan, real, PKG_DEFAULT_REPO) != PKG_OK) {
            return KSHELL_ERROR;
        }
    }

    return KSHELL_OK;
}

static int command_sync(struct pkg_options *options, struct pkg_index *cached)
{
    const char *repo = selected_repo(options);
    char base[PKG_URL_MAX];
    char json_path[PKG_PATH_MAX];
    char sig_path[PKG_PATH_MAX];
    char url[PKG_URL_MAX];
    int nc;
    int rc;

    if (options->offline) {
        return (pkg_index_load(repo, cached) == PKG_OK) ? PKG_OK : PKG_EFETCH;
    }

    rc = repository_url(repo, base, sizeof(base));
    if (rc != PKG_OK) {
        pkg_error("there is no repository %s in %s", repo, PKG_CONF);
        return KSHELL_ERROR;
    }

    if (pkg_mkdir_p(PKG_INDEX_DIR) != PKG_OK) {
        return KSHELL_ERROR;
    }

    pkg_say("refreshing %s", repo);

    nc = snprintf(json_path, sizeof(json_path), "%s/%s.json", PKG_INDEX_DIR, repo);
    if (nc < 0 || (k_size_t)nc >= sizeof(json_path)) {
        return KSHELL_ERROR;
    }

    nc = snprintf(sig_path, sizeof(sig_path), "%s/%s.json.sig", PKG_INDEX_DIR, repo);
    if (nc < 0 || (k_size_t)nc >= sizeof(sig_path)) {
        return KSHELL_ERROR;
    }

    /* The signature first: a server that has no key material for the index
     * costs no download of it. */
    repository_address(url, sizeof(url), base, "index.json.sig");
    if (fetch_to(url, sig_path) != PKG_OK) {
        pkg_error("the signature of the index did not arrive");
        return KSHELL_ERROR;
    }

    repository_address(url, sizeof(url), base, "index.json");
    if (fetch_to(url, json_path) != PKG_OK) {
        pkg_error("the index did not arrive");
        return KSHELL_ERROR;
    }

    rc = pkg_index_load(repo, cached);
    if (rc != PKG_OK) {
        say_error(rc);
        return KSHELL_ERROR;
    }

    pkg_say("%u package(s) in the index", (unsigned)cached->count);

    return KSHELL_OK;
}

static int command_upgrade(struct pkg_options *options, const char *repo,
                           const struct pkg_index *index)
{
    struct pkg_record *installed = g_installed;
    k_size_t count = 0;
    k_size_t i;

    if (pkg_db_list(installed, K_ARRAY_SIZE(g_installed), &count) != PKG_OK) {
        return KSHELL_OK; /* nothing is installed: nothing to upgrade */
    }

    for (i = 0; i < count; i++) {
        int found;

        if (ignored(options, installed[i].name)) {
            pkg_say("%s is ignored", installed[i].name);
            continue;
        }

        found = pkg_index_find(index, installed[i].name);
        if (found < 0) {
            continue;
        }

        if (kernel_version_compare(index->entries[found].version, installed[i].version) <= 0) {
            continue;
        }

        if (install_entry(options, repo, &index->entries[found]) != PKG_OK) {
            return KSHELL_ERROR;
        }
    }

    return KSHELL_OK;
}

/* --- the queries --- */

static void print_record(const struct pkg_record *record)
{
    pkg_plain("%s %s", record->name, record->version);
}

static void print_record_info(const struct pkg_record *record)
{
    pkg_plain("Name           : %s", record->name);
    pkg_plain("Version        : %s", record->version);
    pkg_plain("Machine        : %s", record->machine);
    pkg_plain("ABI            : %u", (unsigned)record->abi);
    pkg_plain("Size           : %llu", (unsigned long long)record->size);
    pkg_plain("SHA-256        : %s", record->sha256);
    pkg_plain("Explicit       : %s", record->explicit_install ? "yes" : "no");
    pkg_plain("File           : %s/%s", PKG_BINDIR, record->name);
}

static int check_record(const struct pkg_record *record)
{
    char path[PKG_PATH_MAX];
    k_u8_t buffer[512];
    k_u8_t raw[MICROOS_SHA256_SIZE];
    struct microos_sha256_ctx context;
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char real[PKG_PATH_MAX];
    char digest[PKG_SHA256_HEX];
    static const char digits[] = "0123456789abcdef";
    k_size_t i;
    int n;

    snprintf(path, sizeof(path), "%s/%s", PKG_BINDIR, record->name);

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    if (vfs_open(real, VFS_O_RDONLY, &file, priv) != 0) {
        pkg_plain("%s: missing", record->name);
        return PKG_ENOENT;
    }

    microos_sha256_init(&context);

    for (;;) {
        n = vfs_read(&file, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }

        microos_sha256_update(&context, buffer, (size_t)n);
    }

    (void)vfs_close(&file);

    microos_sha256_final(&context, raw);

    for (i = 0; i < MICROOS_SHA256_SIZE; i++) {
        digest[i * 2u] = digits[raw[i] >> 4];
        digest[i * 2u + 1u] = digits[raw[i] & 0x0Fu];
    }

    digest[PKG_SHA256_HEX - 1u] = '\0';

    if (strcmp(digest, record->sha256) != 0) {
        pkg_plain("%s: the digest is not the one recorded", record->name);
        return PKG_ESIGN;
    }

    return PKG_OK;
}

static int query_record(struct pkg_options *options, const struct pkg_record *record)
{
    if (options->check) {
        return check_record(record);
    }

    if (options->files) {
        pkg_plain("%s/%s", PKG_BINDIR, record->name);
        return PKG_OK;
    }

    if (options->info) {
        print_record_info(record);
        return PKG_OK;
    }

    print_record(record);

    return PKG_OK;
}

static int command_query(struct pkg_options *options)
{
    struct pkg_record *installed = g_installed;
    k_size_t count = 0;
    k_size_t i;
    int failed = 0;

    if (options->name_count > 0) {
        for (i = 0; i < options->name_count; i++) {
            struct pkg_record record;

            if (pkg_db_read(options->names[i], &record) != PKG_OK) {
                pkg_error("%s is not installed", options->names[i]);
                return KSHELL_ERROR;
            }

            if (query_record(options, &record) != PKG_OK) {
                failed++;
            }
        }

        if (failed > 0) {
            pkg_error("%d file(s) do not match", failed);
            return KSHELL_ERROR;
        }

        return KSHELL_OK;
    }

    if (pkg_db_list(installed, K_ARRAY_SIZE(g_installed), &count) != PKG_OK) {
        return KSHELL_OK;
    }

    for (i = 0; i < count; i++) {
        if (options->explicit_only && !installed[i].explicit_install) {
            continue;
        }

        if (query_record(options, &installed[i]) != PKG_OK) {
            failed++;
        }
    }

    if (failed > 0) {
        pkg_error("%d file(s) do not match", failed);
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int command_info(struct pkg_options *options)
{
    const char *repo = selected_repo(options);
    struct pkg_index *index = &g_index;
    k_size_t i;
    int rc;

    if (options->name_count == 0) {
        usage();
        return KSHELL_USAGE;
    }

    rc = load_index_or_say(repo, index);
    if (rc != PKG_OK) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < options->name_count; i++) {
        int found = pkg_index_find(index, options->names[i]);
        const struct pkg_entry *entry;

        if (found < 0) {
            pkg_error("%s is not in the index of %s", options->names[i], repo);
            return KSHELL_ERROR;
        }

        entry = &index->entries[found];

        pkg_plain("Repository     : %s", repo);
        pkg_plain("Name           : %s", entry->name);
        pkg_plain("Version        : %s", entry->version);
        pkg_plain("Machine        : %s", entry->machine);
        pkg_plain("ABI            : %u", (unsigned)entry->abi);
        pkg_plain("Size           : %llu", (unsigned long long)entry->size);
        pkg_plain("SHA-256        : %s", entry->sha256);
        pkg_plain("Summary        : %s", entry->summary);
    }

    return KSHELL_OK;
}

static int command_search(struct pkg_options *options)
{
    const char *repo = selected_repo(options);
    struct pkg_index *index = &g_index;
    const char *term;
    k_size_t i;
    int rc;

    if (options->name_count == 0) {
        usage();
        return KSHELL_USAGE;
    }

    term = options->names[0];

    rc = load_index_or_say(repo, index);
    if (rc != PKG_OK) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < index->count; i++) {
        const struct pkg_entry *entry = &index->entries[i];

        if (strstr(entry->name, term) == NULL && strstr(entry->summary, term) == NULL) {
            continue;
        }

        pkg_plain("%s/%s %s    %s", repo, entry->name, entry->version, entry->summary);
    }

    return KSHELL_OK;
}

static int command_remove(struct pkg_options *options)
{
    k_size_t i;

    if (options->name_count == 0) {
        usage();
        return KSHELL_USAGE;
    }

    for (i = 0; i < options->name_count; i++) {
        const char *name = options->names[i];
        struct pkg_record record;
        char path[PKG_PATH_MAX];

        if (pkg_db_read(name, &record) != PKG_OK) {
            pkg_error("%s is not installed", name);
            return KSHELL_ERROR;
        }

        if (options->dry_run) {
            pkg_say("remove %s %s", name, record.version);
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", PKG_BINDIR, name);

        if (options->remove_files) {
            pkg_say("remove %s", path);
        }

        (void)pkg_remove(path);

        if (pkg_db_remove(name) != PKG_OK) {
            pkg_warn("the record of %s stayed", name);
        }

        pkg_log("remove", name, record.version);
        pkg_say("removed %s", name);
    }

    return KSHELL_OK;
}

static int command_clean(struct pkg_options *options)
{
    const char *cachedir = options->cachedir != NULL ? options->cachedir : default_cache();
    struct vfs_dirent entries[PKG_INDEX_MAX];
    k_size_t count = 0;
    k_size_t i;
    k_size_t removed = 0;

    if (pkg_list(cachedir, entries, K_ARRAY_SIZE(entries), &count) != PKG_OK) {
        pkg_say("the cache is empty");
        return KSHELL_OK;
    }

    for (i = 0; i < count; i++) {
        char path[PKG_PATH_MAX];

        if (entries[i].type != 1) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", cachedir, entries[i].name);

        if (options->dry_run) {
            pkg_say("remove %s", path);
            continue;
        }

        if (pkg_remove(path) == PKG_OK) {
            removed++;
        }
    }

    pkg_say("%u file(s) removed from the cache", (unsigned)removed);

    return KSHELL_OK;
}

static int command_repo(struct pkg_options *options)
{
    const char *url;
    const char *repo = selected_repo(options);
    char line[PKG_URL_MAX];
    int n;

    if (options->name_count < 3 || strcmp(options->names[1], "add") != 0) {
        kshell_printf("usage: pkg repo add <url>\n");
        return KSHELL_USAGE;
    }

    url = options->names[2];

    n = snprintf(line, sizeof(line), "%s %s\n", repo, url);
    if (n < 0 || (k_size_t)n >= sizeof(line)) {
        return KSHELL_ERROR;
    }

    if (pkg_append(PKG_CONF, line, (k_size_t)n) != PKG_OK) {
        pkg_error("%s cannot be written", PKG_CONF);
        return KSHELL_ERROR;
    }

    pkg_say("repository %s is %s", repo, url);

    return KSHELL_OK;
}

static int command_log(void)
{
    char text[512];
    k_size_t got = 0;

    if (pkg_read(PKG_LOG_FILE, text, sizeof(text) - 1u, &got) != PKG_OK) {
        pkg_say("the log is empty");
        return KSHELL_OK;
    }

    text[got] = '\0';
    kshell_printf("%s", text);

    return KSHELL_OK;
}

/* --- the root and the command --- */

static bool set_root(const char *root)
{
    k_size_t len;

    g_pkg_root[0] = '\0';

    if (root == NULL) {
        return true;
    }

    len = strlen(root);

    while (len > 1 && root[len - 1] == '/') {
        len--;
    }

    if (len == 0) {
        return true;
    }

    if (len >= sizeof(g_pkg_root)) {
        return false;
    }

    memcpy(g_pkg_root, root, len);
    g_pkg_root[len] = '\0';

    if (g_pkg_root[0] != '/') {
        return false;
    }

    if (strcmp(g_pkg_root, "/") == 0) {
        g_pkg_root[0] = '\0';
    }

    return true;
}

int pkg_main(int argc, char **argv)
{
    struct pkg_options options;
    struct pkg_index *index = &g_index;
    const char *repo;
    bool have_index = false;
    int rc;

    memset(&options, 0, sizeof(options));
    options.color = PKG_COLOR_AUTO;

    if (!parse_arguments(&options, argc, argv)) {
        usage();
        return KSHELL_USAGE;
    }

    if (options.help) {
        usage();
        return KSHELL_OK;
    }

    if (options.version) {
        kshell_printf("pkg %s\n", kernel_version());
        return KSHELL_OK;
    }

    if (!set_root(options.root)) {
        pkg_error("--root wants an absolute path");
        return KSHELL_USAGE;
    }

    g_pkg_quiet = options.quiet;
    g_pkg_verbose = options.verbose;
    g_pkg_color = (options.color == PKG_COLOR_ALWAYS);

    repo = selected_repo(&options);

    /* The two words the table has no short form for. They are read before
     * the letters, because "add" and "log" are names of a package as well. */
    if (options.name_count > 0 && strcmp(options.names[0], "repo") == 0) {
        return command_repo(&options);
    }

    if (options.name_count > 0 && strcmp(options.names[0], "log") == 0) {
        return command_log();
    }

    if (options.clean) {
        return command_clean(&options);
    }

    if (options.sync) {
        rc = command_sync(&options, index);
        if (rc != KSHELL_OK) {
            return rc;
        }

        have_index = true;

        if (!options.upgrade && options.command == PKG_CMD_NONE && options.name_count == 0 &&
            !options.query && !options.info && !options.search && !options.files &&
            !options.check) {
            return KSHELL_OK;
        }
    }

    if (options.upgrade && options.command != PKG_CMD_LOCAL) {
        if (!have_index) {
            if (load_index_or_say(repo, index) != PKG_OK) {
                return KSHELL_ERROR;
            }

            have_index = true;
        }

        if (command_upgrade(&options, repo, index) != KSHELL_OK) {
            return KSHELL_ERROR;
        }

        if (options.name_count == 0) {
            return KSHELL_OK;
        }
    }

    if (options.search) {
        return command_search(&options);
    }

    if (options.check || options.files || (options.info && options.query)) {
        return command_query(&options);
    }

    if (options.info) {
        return command_info(&options);
    }

    /* `-Sy` has done its work and has no name to install; `-S` without a
     * name is an operation of the table and not an install of nothing. */
    if (options.command == PKG_CMD_INSTALL && options.name_count == 0) {
        return KSHELL_OK;
    }

    switch (options.command) {
    case PKG_CMD_LOCAL:
        return command_local(&options);
    case PKG_CMD_INSTALL:
        return command_install(&options);
    case PKG_CMD_REMOVE:
        return command_remove(&options);
    default:
        break;
    }

    if (options.query) {
        return command_query(&options);
    }

    usage();
    return KSHELL_USAGE;
}

/*
 * The help of the command lives in the table of the shell and not in the
 * handler: `pkg --help`, `pkg -h` and `help pkg` are answered there, and the
 * three cannot disagree with the flags the parser accepts (ADR 0024 §6).
 */
static const struct kshell_help g_pkg_help = {
    .summary = "the installer of programs (ADR 0024)",
    .usage = "pkg <operation> [options] [name...]",
    .detail = "  sync                     -Sy   refresh the index of every repository\n"
              "  upgrade                  -Su   a newer version of every installed package\n"
              "  sync --upgrade           -Syu  both, in that order\n"
              "  install NAME...          -S    fetch, verify and install\n"
              "  install ./x.mox          -U    the same for a file on the machine\n"
              "  remove NAME...           -R    remove (-Rn also its files)\n"
              "  search TERM              -Ss   the name and the summary of a match\n"
              "  info NAME                -Si / -Qi  the package in a repository / installed\n"
              "  list                     -Q    the installed packages (-Qe explicit only)\n"
              "  files NAME               -Ql   the files a package installed\n"
              "  check                    -Qk   the digest of every installed file\n"
              "  clean                    -Sc   the cache\n"
              "  repo add URL             a repository and its key, in /etc/pkg.conf\n"
              "  log                      what was installed, removed and when\n"
              "\n"
              "  --noconfirm --needed --download-only --dry-run --allow-downgrade\n"
              "  --ignore NAME --force --offline --root DIR --repo NAME --cachedir DIR\n"
              "  --color auto|always|never --quiet --verbose --help --version\n"
              "\n"
              "A package without a valid signature does not install, and --nosignature\n"
              "is not a flag. A volume without rename installs in place, with a warning.\n",
    .owns_short_h = false,
};

int pkg_register(void)
{
    return kshell_register_help("pkg", &g_pkg_help, pkg_main);
}
