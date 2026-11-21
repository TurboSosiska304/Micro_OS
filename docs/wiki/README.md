# The Micro OS wiki

Micro OS is a Linux-like kernel for small machines: one portable core, and a
port per kind of machine. This wiki is the **user guide** - how to build the
system, drive its shell, find a file, run a program, and fix what does not
work.

The canonical documents stay in [`docs/`](../README.md): the architecture, the
roadmap, the ABI and the decisions of the project. This wiki links to them
rather than copying them, and when a page here and a document there disagree,
the document is right. There is no Russian translation of the wiki yet; the
canonical Russian documents are in [`docs/ru/`](../ru/README.md).

## Start here

| If you want to... | Read |
|---|---|
| see what the system is and run it in ten minutes | [getting-started.md](getting-started.md) |
| build the host, x86-64 or ESP32 image | [build.md](build.md) |
| type a command at the prompt | [shell.md](shell.md) |
| understand the filesystems and `/etc` | [files.md](files.md) |
| understand `exec`, `MOX1` and ring 3 | [programs.md](programs.md) |
| know which machine does what | [ports.md](ports.md) |
| read the syscall contract | [kernel-abi.md](kernel-abi.md) |
| run the checks | [testing.md](testing.md) |
| fix a build or a boot that failed | [troubleshooting.md](troubleshooting.md) |

## The pages

| Section | Page | What it answers |
|---|---|---|
| **Start here** | [getting-started.md](getting-started.md) | what the system is, the first commands |
| **Build and run** | [build.md](build.md) | host, x86-64, ESP32, dependencies |
| **Shell** | [shell.md](shell.md) | the commands, `PATH`, `stat`, `fsck`, `httpd` |
| **Files** | [files.md](files.md) | MicroFS, `/etc`, `mkfs`/`fsck`, disks |
| **Programs** | [programs.md](programs.md) | `MOX1`, `/bin`, `exec`, ring 3, `fork` |
| **Ports** | [ports.md](ports.md) | ESP32, x86-64, host, null: what runs where |
| **Kernel and ABI** | [kernel-abi.md](kernel-abi.md) | the calls, the rights, `EFAULT`, contract 1.7 |
| **Testing** | [testing.md](testing.md) | `make ci`, the smoke tests, the device test |
| **Troubleshooting** | [troubleshooting.md](troubleshooting.md) | symptom, cause, what to do |
| **Contributing** | [../../CONTRIBUTING.md](../../CONTRIBUTING.md) | the rules, commits, branches, style |

The design behind all of this is in [`docs/architecture.md`](../architecture.md),
the plan in [`docs/roadmap.md`](../roadmap.md), and the list of what the tree
knows it does not do yet in [`docs/debts.md`](../debts.md). The ABI is written
down in [`docs/syscall-abi.md`](../syscall-abi.md); the wiki page is a guide to
it, not a second copy.

## Serving the wiki as a site

`mkdocs.yml` in the root of the repository is a draft of a site: it is not a
part of the build and `make ci` does not need it. The pages are plain Markdown
and GitHub renders them as they are. To serve them locally:

```bash
python3 -m pip install --user mkdocs mkdocs-material
mkdocs serve                  # http://127.0.0.1:8000
mkdocs build                  # writes site/, which is generated
```

`docs_dir` is `docs/wiki`, and MkDocs reads this file as the home page of the
site. The theme is `material`; if `mkdocs-material` is not installed, replace
`theme: material` with `theme: readthedocs` in `mkdocs.yml` and plain `mkdocs`
serves the pages. The wiki links into the rest of `docs/` on purpose, so a
build may warn about targets that live outside the site; the links themselves
are checked against the repository by the script below.

## Checking the links

```bash
python3 tools/wiki_check.py                  # docs/wiki, every link and anchor
python3 tools/wiki_check.py docs/wiki/shell.md  # one page
python3 tools/wiki_check.py --verbose        # one line per page
```

The script fails on a target that does not exist and on an anchor that no
heading of the target file produces. It is run by hand today; the natural
place for it is `make integrity` (or a step of `make ci` next to the other
repository checks), which is a change of the `Makefile` and not a part of this
wiki.

## What the wiki does not do

The wiki does not replace the design documents, the ADRs or the phase reports:
it explains how to use the system, not why it is shaped the way it is. It also
does not promise what the tree does not do - a feature that is planned is
marked as planned, and the roadmap is the place where its status is kept.
