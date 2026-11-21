#!/usr/bin/env python3
"""Micro OS — keep the links into the repository alive on the built wiki.

A wiki page may link to a canonical document outside `docs_dir`, for example
`../../CONTRIBUTING.md` or `../roadmap.md`. MkDocs cannot copy those files
into the site, and a relative link that leaves the site would be a 404 on
GitHub Pages. This hook rewrites such a link to the file on GitHub at the ref
the site is built from (the tag, or the branch for `dev`), so the page keeps
pointing at the canonical text instead of at a missing URL.

Links inside `docs_dir` are left alone: MkDocs already turns them into the
pages of the site. The hook is listed in `mkdocs.yml`; the build script also
passes its absolute path, because the generated configuration of a version
lives outside the tree.

The ref and the prefix of the pages come from `extra.wiki_ref` and
`extra.wiki_docs_prefix`. With no hook support (an old MkDocs) the file is
inert; nothing else depends on it.
"""

from __future__ import annotations

import posixpath
import re
from urllib.parse import urlsplit

REPO = "https://github.com/TurboSosiska304/Micro_OS"
ANCHOR_RE = re.compile(r'(<a\s[^>]*?href=")([^"]+)(")', re.IGNORECASE)


def _repo_url(path: str, ref: str) -> str:
    """A blob for a file, a tree for a directory: the source on GitHub."""
    kind = "blob" if posixpath.splitext(path)[1] else "tree"
    return f"{REPO}/{kind}/{ref}/{path}"


def on_page_content(html, page, config, files):  # noqa: ARG001
    extra = config.extra or {}
    ref = extra.get("wiki_ref") or "main"
    prefix = (extra.get("wiki_docs_prefix") or "docs/wiki").strip("/")
    source = page.file.src_path

    def rewrite(match):
        href = match.group(2)
        parts = urlsplit(href)

        if (
            not href
            or href.startswith(("#", "/", "//"))
            or parts.scheme
            or parts.netloc
        ):
            return match.group(0)

        target = posixpath.normpath(
            posixpath.join(prefix, posixpath.dirname(source), parts.path)
        )
        if target == prefix or target.startswith(prefix + "/"):
            return match.group(0)
        if target.startswith("..") or not target:
            return match.group(0)

        url = _repo_url(target, ref)
        if parts.fragment:
            url += "#" + parts.fragment
        return match.group(1) + url + match.group(3)

    return ANCHOR_RE.sub(rewrite, html)
