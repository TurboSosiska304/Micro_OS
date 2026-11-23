#!/usr/bin/env python3
"""Micro OS — the compact magic database of the port of file.

The release of `file` ships a magic database of about ten megabytes: every
format of every system of the last forty years, compiled into one file. The
image of a program of this system is a file of a few hundred kilobytes and the
RAM of the machine is what the boot leaves; the database is therefore a
**subset of the Magdir files**, chosen by `third_party/file/magic/subset.json`
and written as one text file, which the build then compiles with the library
itself into `magic.mgc` (the two levels of the port: the compiled file first,
the text as the fallback).

    tools/magic_subset.py --config third_party/file/magic/subset.json \
                          --out build-x86/magic

The subset keeps a rule and its continuations up to a `depth` of `>`, and a
rule whose description `desc` does not name is dropped with its subtree (the
condition lines without a description stay: they are the tree that reaches a
description). A `name` block is kept only when a kept rule `use`s it, and the
`use` closure is followed. A rule of a float type is dropped: a program of
this system has no floating point (`-mgeneral-regs-only`), so a database that
matched one would ask the library for a number it cannot hold.

The output is deterministic: the blocks keep the order of their file and the
files the order of the configuration. The script warns on stderr when a `use`
names a block that no selected file defines; such a database still loads.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

# The types of a rule that need a floating point register. A program of this
# system is built with -mgeneral-regs-only and the library is compiled with
# MICRO_OS_NO_FLOAT, so a rule of one of them is not in the database.
FLOAT_TYPES = {"float", "befloat", "lefloat", "double", "bedouble", "ledouble"}

# A line at column zero that is neither a continuation, a comment nor an
# attribute begins a rule (or a `name` block).
TOP_LEVEL = re.compile(r"^[^>#!\s]")
NAME = re.compile(r"^\S+\s+name\s+(\S+)")
USE = re.compile(r"\buse\s+([^\s#]+)")


def level(line: str) -> int:
    """The depth of a rule: the number of leading '>' characters."""
    count = 0
    while count < len(line) and line[count] == ">":
        count += 1
    return count


def rule_type(line: str) -> str:
    """The type field of a rule line, without its flags (`string/b`)."""
    fields = re.split(r"[ \t]+", line.strip(), maxsplit=3)
    if len(fields) < 2:
        return ""
    return fields[1].split("/")[0]


def rule_desc(line: str) -> str:
    """The description of a rule, or the empty string."""
    fields = re.split(r"[ \t]+", line.strip(), maxsplit=3)
    return fields[3].strip() if len(fields) >= 4 else ""


def parse(path: str):
    """The blocks of one magic file: (name or None, lines)."""
    blocks = []
    current = None
    with open(path, "r", encoding="utf-8", errors="replace") as source:
        for line in source:
            if TOP_LEVEL.match(line):
                current = [line]
                blocks.append(current)
            elif current is None:
                # The header comment of the file: it is not a block and the
                # build does not need it.
                continue
            else:
                current.append(line)
    result = []
    for lines in blocks:
        match = NAME.match(lines[0])
        result.append([match.group(1) if match else None, lines])
    return result


def uses(lines) -> set:
    """The names a block refers to with `use`."""
    names = set()
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith("#") or stripped.startswith("!:"):
            continue
        for match in USE.finditer(line):
            # `use \^elf-le` names the same block as `use elf-le`; the caret
            # asks for the other byte order of the entries and is not part of
            # the name.
            names.add(match.group(1).lstrip("\\^"))
    return names


def prune(lines, depth: int, desc):
    """Keep the rules of a block up to `depth`, with a filter on the text."""
    kept = []
    skip = None
    last_rule = ""
    for line in lines:
        if line.startswith("!:"):
            if skip is not None:
                continue
            # An attribute belongs to the rule before it. A rule without a
            # description cannot carry a MIME type or an extension: the
            # compiler refuses the pair.
            if rule_desc(last_rule):
                kept.append(line)
            continue
        if not line.strip() or line.startswith("#"):
            kept.append(line)
            continue
        depth_here = level(line)
        if skip is not None and depth_here > skip:
            continue
        if skip is not None and depth_here <= skip:
            skip = None
        if depth_here > depth:
            skip = depth_here
            continue
        if rule_type(line) in FLOAT_TYPES:
            skip = depth_here
            continue
        if depth_here > 0:
            description = rule_desc(line)
            if description and desc and not any(re.search(p, description) for p in desc):
                skip = depth_here
                continue
        kept.append(line)
        last_rule = line
    return kept


def build(config_path: str, out_path: str) -> int:
    with open(config_path, "r", encoding="utf-8") as source:
        config = json.load(source)
    base = os.path.dirname(os.path.abspath(config_path))

    files = {}
    order = []
    defined = {}
    for spec in config["files"]:
        path = os.path.join(base, spec["path"])
        if path in files:
            continue
        blocks = parse(path)
        blocks = [[name, prune(lines, spec.get("depth", 99), spec.get("desc", []))]
                  for name, lines in blocks]
        files[path] = blocks
        order.append((path, spec))
        for index, (name, _) in enumerate(blocks):
            if name:
                defined.setdefault(name, []).append((path, index))

    chosen = {path: set() for path, _ in order}
    needed = set()
    for path, spec in order:
        for index, (name, lines) in enumerate(files[path]):
            if name:
                continue
            if spec.get("keep") and any(re.search(p, lines[0]) for p in spec["keep"]):
                chosen[path].add(index)
                needed |= uses(lines)

    included = set()
    while needed:
        name = needed.pop()
        if name in included:
            continue
        included.add(name)
        found = defined.get(name)
        if not found:
            print(f"magic_subset: no definition of `{name}`", file=sys.stderr)
            continue
        path, index = found[0]
        chosen[path].add(index)
        needed |= uses(files[path][index][1])

    rules = 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Micro OS: the subset of the magic files of the port.\n")
        out.write("# Generated by tools/magic_subset.py from\n")
        out.write("# third_party/file/magic/*.json and Magdir; do not edit.\n")
        for path, _ in order:
            if not chosen[path]:
                continue
            out.write(f"\n# --- {os.path.basename(path)}\n")
            for index, (_, lines) in enumerate(files[path]):
                if index in chosen[path]:
                    out.writelines(lines)
                    rules += len([line for line in lines if line.strip()
                                  and not line.startswith("#")])
    print(f"magic_subset: {rules} rules in {out_path}", file=sys.stderr)
    return 0


def main(argv) -> int:
    parser = argparse.ArgumentParser(description="the compact magic database of the port")
    parser.add_argument("--config", required=True, help="the selection (subset.json)")
    parser.add_argument("--out", required=True, help="the text magic to write")
    args = parser.parse_args(argv)
    return build(args.config, args.out)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
