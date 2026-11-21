#!/usr/bin/env python3
"""Micro OS — check the links of the wiki.

The pages of docs/wiki link to each other and into the canonical documents of
docs/. A path that does not exist or a heading that was renamed is a link that
leads nowhere, and nothing else notices: the wiki is Markdown that no compiler
reads. This script walks the pages, resolves every relative link and checks
the anchors of the links that name a heading.

Usage:
    python3 tools/wiki_check.py [path ...]

With no argument the whole docs/wiki is checked; a path may be a file or a
directory. Links to the web and absolute URLs are not fetched. A link that
begins with '/' is taken from the root of the repository, which is how the
wiki names a document of docs/ when a relative path would be unreadable.

The exit status is 0 when every link is good and 1 otherwise.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
WIKI = ROOT / "docs" / "wiki"

# A fence opens a block in which a line is not Markdown; the closing fence has
# to use the same character and be at least as long.
FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
LINK_RE = re.compile(r"!?\[([^\]]*)\]\(([^)]*)\)")
INLINE_CODE_RE = re.compile(r"(`+)(.+?)\1")
HTML_ANCHOR_RE = re.compile(r"""(?:id|name)\s*=\s*["']([^"']+)["']""")
SCHEME_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")

MARKDOWN_SUFFIXES = (".md", ".markdown")


def slugify(text: str) -> str:
    """The anchor GitHub makes out of a heading.

    Lower case, the spaces become hyphens, the letters, the digits, the
    hyphens and the underscores stay and everything else goes - which is what
    a browser does with the text of a heading of GitHub-flavoured Markdown.
    """
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()

    out = []
    for char in text:
        if char.isspace():
            out.append("-")
        elif char in "-_" or char.isalnum():
            out.append(char)

    return "".join(out)


def content_lines(text: str) -> list[tuple[int, str]]:
    """The lines of a page with the fenced blocks taken out.

    A heading inside a fence is code, not a heading, and a link inside one is
    an example that must not be followed. The number of the line is the one of
    the file, so that a problem names the place a person can open.
    """
    lines = []
    fence = ""

    for number, line in enumerate(text.splitlines(), start=1):
        match = FENCE_RE.match(line)

        if match is not None:
            token = match.group(1)
            if fence == "":
                fence = token[0] * len(token)
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = ""
            continue

        if fence == "":
            lines.append((number, line))

    return lines


def anchors_of(path: Path) -> set[str]:
    """Every anchor a browser can jump to in the document."""
    text = path.read_text(encoding="utf-8")
    anchors = set(HTML_ANCHOR_RE.findall(text))
    seen: dict[str, int] = {}

    for _, line in content_lines(text):
        match = HEADING_RE.match(line)
        if match is None:
            continue

        base = slugify(match.group(2))
        if base in seen:
            seen[base] += 1
            anchors.add(f"{base}-{seen[base]}")
        else:
            seen[base] = 0
            anchors.add(base)

    return anchors


def target_of(raw: str) -> str:
    """The destination of a link, without the optional title."""
    target = raw.strip()

    if target.startswith("<"):
        end = target.find(">")
        if end > 0:
            return target[1:end]

    return target.split(None, 1)[0] if target else ""


def check_page(path: Path, cache: dict[Path, set[str]]) -> tuple[int, list[str]]:
    """Check one page. Returns the number of links and the broken ones."""
    problems = []
    links = 0
    text = path.read_text(encoding="utf-8")

    for number, line in content_lines(text):
        # A span of inline code is not a link either.
        line = INLINE_CODE_RE.sub(lambda m: " " * len(m.group(0)), line)

        for match in LINK_RE.finditer(line):
            target = target_of(match.group(2))
            links += 1

            where = f"{path.relative_to(ROOT)}:{number}"

            # An empty destination is the page itself, which CommonMark allows.
            if target == "":
                continue

            if SCHEME_RE.match(target):
                continue

            if target.startswith("#"):
                anchor = unquote(target[1:])
                if path not in cache:
                    cache[path] = anchors_of(path)
                if anchor and anchor not in cache[path]:
                    problems.append(f"{where}: no anchor '{target}' in {path.name}")
                continue

            name, _, anchor = target.partition("#")
            name = unquote(name)
            anchor = unquote(anchor)

            if name.startswith("/"):
                resolved = (ROOT / name.lstrip("/")).resolve()
            else:
                resolved = (path.parent / name).resolve()

            if not resolved.exists():
                problems.append(f"{where}: no such file: {target}")
                continue

            if anchor and resolved.is_file() and resolved.suffix.lower() in MARKDOWN_SUFFIXES:
                if resolved not in cache:
                    cache[resolved] = anchors_of(resolved)
                anchors = cache[resolved]
                if anchor not in anchors:
                    close = difflib.get_close_matches(anchor, sorted(anchors), n=1)
                    hint = f" (did you mean '#{close[0]}'?)" if close else ""
                    problems.append(
                        f"{where}: no anchor '{target}' in "
                        f"{resolved.relative_to(ROOT)}{hint}"
                    )

    return links, problems


def pages_of(target: Path) -> list[Path]:
    if target.is_file():
        return [target] if target.suffix.lower() in MARKDOWN_SUFFIXES else []
    if target.is_dir():
        return sorted(p for p in target.rglob("*.md") if p.is_file())

    return []


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check the links of docs/wiki.")
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        help="a page or a directory of the wiki (default: docs/wiki)",
    )
    parser.add_argument("--verbose", action="store_true", help="name every page checked")
    args = parser.parse_args(argv)

    targets = args.paths if args.paths else [WIKI]
    pages = []
    for target in targets:
        found = pages_of(target if target.is_absolute() else (Path.cwd() / target))
        if not found:
            print(f"wiki_check: no Markdown page in {target}")
            return 1
        pages.extend(found)

    cache: dict[Path, set[str]] = {}
    broken = 0
    links = 0

    for page in pages:
        count, problems = check_page(page, cache)
        links += count
        broken += len(problems)

        if args.verbose:
            print(f"{page.relative_to(ROOT)}: {count} link(s), {len(problems)} problem(s)")

        for problem in problems:
            print(f"  {problem}")

    if broken:
        print(f"wiki_check: {broken} broken link(s) in {len(pages)} page(s)")
        return 1

    print(f"wiki_check: {links} link(s) in {len(pages)} page(s), OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
