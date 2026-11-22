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
| know what the first major release is and promises | [release-1.0.md](release-1.0.md) |
| see what 1.1 changed on the board | [release-1.1.md](release-1.1.md) |
| see what 1.2 brought to x86-64 | [release-1.2.md](release-1.2.md) |
| see what the system is and run it in ten minutes | [getting-started.md](getting-started.md) |
| build the host, x86-64 or ESP32 image | [build.md](build.md) |
| type a command at the prompt | [shell.md](shell.md) |
| understand the filesystems and `/etc` | [files.md](files.md) |
| set the screen, the font and the console | [settings.md](settings.md) |
| see what a screen is on each machine | [screen.md](screen.md) |
| understand the network of the system | [network.md](network.md) |
| install a program from a repository | [pkg.md](pkg.md) |
| understand `exec`, `MOX1` and ring 3 | [programs.md](programs.md) |
| know which machine does what | [ports.md](ports.md) |
| bring Micro OS to another machine | [porting.md](porting.md) |
| port a program of somebody else (zlib, Lua, sbase) | [../porting-programs.md](../porting-programs.md) |
| move a machine from 0.x to 1.0 | [`docs/migration.md`](../migration.md) |
| read the syscall contract | [kernel-abi.md](kernel-abi.md) |
| run the checks | [testing.md](testing.md) |
| fix a build or a boot that failed | [troubleshooting.md](troubleshooting.md) |

## The pages

| Section | Page | What it answers |
|---|---|---|
| **The 1.0 release** | [release-1.0.md](release-1.0.md) | what 1.0 is, promises and does not |
| **The 1.1 release** | [release-1.1.md](release-1.1.md) | the last major changes of the board, and the first phase of many cores |
| **The 1.2 release** | [release-1.2.md](release-1.2.md) | the screen of x86-64, the stream and the installer, the ISO, and the ports |
| **Start here** | [getting-started.md](getting-started.md) | what the system is, the first commands |
| **Build and run** | [build.md](build.md) | host, x86-64, ESP32, dependencies |
| **Shell** | [shell.md](shell.md) | the commands, `PATH`, `stat`, `fsck`, `httpd` |
| **Files** | [files.md](files.md) | MicroFS, `/etc`, `mkfs`/`fsck`, disks |
| **Settings** | [settings.md](settings.md) | the systems of `/etc`, `config get`/`set`, which machine honours what |
| **Screen** | [screen.md](screen.md) | the TFT, the VGA, the framebuffer of 1.2, what a desktop needs |
| **Network** | [network.md](network.md) | DNS, TCP, HTTP, NTP, the stream and TLS of 1.2 |
| **Packages** | [pkg.md](pkg.md) | `pkg`, the flags, the repositories, the signature |
| **Programs** | [programs.md](programs.md) | `MOX1`, `/bin`, `exec`, ring 3, `fork`, PCRE2 |
| **Ports** | [ports.md](ports.md) | ESP32, x86-64, host, null: what runs where |
| **Porting** | [porting.md](porting.md) | the contract, the bring-up order, the checks |
| **Porting a program** | [../porting-programs.md](../porting-programs.md) | `third_party/`, the port layer, the archive, the tests, the license |
| **Kernel and ABI** | [kernel-abi.md](kernel-abi.md) | the calls, the rights, `EFAULT`, contract 1.9 |
| **Testing** | [testing.md](testing.md) | `make ci`, the smoke tests, the device test |
| **Troubleshooting** | [troubleshooting.md](troubleshooting.md) | symptom, cause, what to do |
| **Contributing** | [../../CONTRIBUTING.md](../../CONTRIBUTING.md) | the rules, commits, branches, style |

The design behind all of this is in [`docs/architecture.md`](../architecture.md),
the plan in [`docs/roadmap.md`](../roadmap.md), and the list of what the tree
knows it does not do yet in [`docs/debts.md`](../debts.md). The ABI is written
down in [`docs/syscall-abi.md`](../syscall-abi.md); the wiki page is a guide to
it, not a second copy.

## The site of the wiki

The pages of this directory are plain Markdown and GitHub renders them as they
are. The **site** - the theme, `mkdocs.yml`, the builder of the versions and
the workflow that publishes them - lives on the `wiki` branch, which is a leaf
of the history: it merges `mainline` for the pages and the releases from their
tags, and it is never merged back into `mainline`, a release candidate or a
feature branch. This tree carries the pages and the link check that
`make integrity` runs, and nothing else of the site.

A branch of the kernel mentions the site with a link or a line of Markdown, as
this paragraph does; it does not carry its code.

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
