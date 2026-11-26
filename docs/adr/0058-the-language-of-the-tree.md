# ADR 0058 - The language of the tree: C, and Rust only behind the C ABI

* Status: accepted
* Date: 2026-11-26

## Context

The question "could this be Rust?" returns every few months, and it deserves a
written answer rather than a conversation that ends in "someday".

What the tree is, counted on 26 November 2026: 24 700 lines of C in
`components/kernel/`, 56 100 in `ports/`, 104 200 in `components/` together,
49 800 in `tests/`. A rewrite is therefore not a port of the kernel - it is a
new project that keeps the name.

What Rust would buy, and what it would not. The defects of 2.0 and 2.0.1 were
not errors of memory: the SMAP window of `path_copy()`, the order of the reset
of an e1000e, the read-only root of a stick and the write of S5 are logic,
hardware sequencing and policy. Safe Rust does not check MMIO, DMA or the order
in which a chipset wants its registers; those stay `unsafe` in any language. The
bugs a borrow checker would have caught are the ones our gates and ASan/TSan
already catch.

What Rust would cost: the board is Xtensa, and Rust for it is the LLVM fork of
esp-rs while the board is built by ESP-IDF 5.3 with GCC - a rewrite of the
port, not a flag; three bare-metal targets and a host substrate, each with
`no_std`, an allocator over `kmm`, a panic handler and a cargo step in a CMake
build; and a test framework of 49 800 lines of C that would have to be taught
a second language or rewritten.

## Decision

**The kernel and the ports are C.** C11 today, C23 from 2.1, measured and not
guessed: the host build (GCC 16.2.1) and the x86-64 port (clang 23.1.5)
compile with `-std=c23` and `CMAKE_C_EXTENSIONS=OFF` with zero errors
(`docs/roadmap-to-3.0.md`, 10b). The board stays on the dialect of ESP-IDF 5.3
until its toolchain knows C23, and the common code stays in the subset both
accept.

**Rust is allowed only as an island**, and only when all of this is true:

1. the component has a hostile input surface (a parser, a verifier, a protocol
   of the network) - the place where a memory error is a security error;
2. it sits behind a C ABI and the rest of the tree does not see its types;
3. it builds for every target it is enabled on, inside the same CMake build;
4. it carries its own tests and the gate of the release it lands in;
5. an ADR of its own names it, the way this one names the rule.

No rewrite of the kernel, no "the new files are in Rust" as a habit, and no
Rust in the board's build while that build is ESP-IDF's.

## Consequences

The rule of `AGENTS.md` ("C11, no C++, no GNU extensions in the portable
kernel") stands, with C23 as the version of 2.1. The memory safety of the
kernel is the work it already does: W^X, the guard page under a stack, SMAP and
the window of `copy_from_user`, ASan and TSan on the host, and a gate for every
behaviour. A person who wants Rust in this tree has a door - a component with a
hostile input, an ADR and a gate - and it is deliberately narrow.
