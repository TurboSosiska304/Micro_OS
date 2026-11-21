#!/usr/bin/env python3
"""Micro OS — build every version of the wiki into one mike-compatible site.

The wiki is not one build: every release tag carries the pages of its own
commit, and a reader who opens an old page must see the old text. This script
checks out the wiki of each tag from the repository (a `git archive`, no
worktree is disturbed), builds it with the theme of the current tree, and lets
`mike` write the layout of https://turbososiska304.github.io/Micro_OS/:

    site/index.html          a redirect to the alias `latest`
    site/versions.json       the versions mike knows
    site/latest/             a redirect alias for the latest stable release
    site/<version>/          the pages of a tag, or the note that it had none
    site/dev/                the pages of the current branch
    site/versions/index.html the human list of every version, stubs included
    site/assets/             the stylesheet of the list and of the 404 page

`mike` is run inside a temporary clone. It commits to the branch `gh-pages`
there and never to the repository of the caller; the branch is exported into
`--site` and the clone is removed. Nothing is pushed: the deployment is a
separate step, described in docs/wiki/deploy.md.

Usage:

    python3 tools/wiki_build_versions.py
    python3 tools/wiki_build_versions.py --site /tmp/wiki --only 0.6.0-rc1,dev

The interpreter must be the venv that has mkdocs, mike and PyYAML; the
`mkdocs` command must be the one of the same venv (the script puts the venv's
bin first on PATH for mike).
"""

from __future__ import annotations

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THEME = ROOT / "docs" / "theme"
BASE_CONFIG = ROOT / "mkdocs.yml"

# GitHub Pages of the repository is a project page, not a domain: the site is
# served under /Micro_OS/. The root and the prefix are read from mkdocs.yml
# (site_url and extra.base_path); the constants below are the fallbacks for a
# tree whose configuration lost them. A later domain changes those two values
# and the CNAME together, and nothing else: the theme builds every link from
# extra.base_path, and the links inside a version are relative.
SITE_URL = "https://turbososiska304.github.io/Micro_OS/"
BASE_PATH = "/Micro_OS/"
REPO_URL = "https://github.com/TurboSosiska304/Micro_OS"
FIRST_VERSION = (0, 4, 0)
DEV_VERSION = "dev"
# The development pages follow the branch of the kernel, not the branch that
# builds the site: `wiki` is a leaf and is never merged with them. A release
# candidate is the newest documentation the project has; `mainline` is where
# it lands. The builder takes the first branch of this list that exists.
DEV_REFS = ("release_candidate", "mainline")
DEV_REF_DEFAULT = "auto"
BRANCH = "gh-pages"


def die(message: str) -> "NoReturn":
    """Stop with one line the person who ran the script can act on."""
    print(f"wiki_build_versions: {message}", file=sys.stderr)
    raise SystemExit(1)


def site_settings() -> tuple:
    """The root URL and the path prefix of the deployment, from mkdocs.yml."""
    try:
        import yaml
    except ImportError:
        return SITE_URL, BASE_PATH

    base = yaml.safe_load(BASE_CONFIG.read_text(encoding="utf-8")) or {}
    root = str(base.get("site_url") or SITE_URL)
    if not root.endswith("/"):
        root += "/"
    prefix = str((base.get("extra") or {}).get("base_path") or BASE_PATH)
    if not prefix.startswith("/"):
        prefix = "/" + prefix
    if not prefix.endswith("/"):
        prefix += "/"
    return root, prefix


def run(command: list, *, cwd: Path | None = None, env: dict | None = None,
        check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    if check and result.returncode != 0:
        shown = " ".join(command)
        die(
            f"command failed ({shown}):\n"
            f"{result.stdout.strip()}\n{result.stderr.strip()}".strip()
        )
    return result


def git(*arguments: str, cwd: Path | None = None) -> str:
    result = run(["git", "-C", str(cwd or ROOT), *arguments])
    return result.stdout.strip()


def exists_ref(name: str) -> str:
    """The ref as this clone can name it, or an empty string."""
    for candidate in (name, f"origin/{name}", f"refs/remotes/origin/{name}"):
        result = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--verify", "--quiet", candidate],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 and result.stdout.strip():
            return candidate
    return ""


def resolve_ref(name: str) -> str:
    """A branch of the kernel, whether the clone has it as a local ref or not.

    A CI checkout of `wiki` has its own branch and the tags; `mainline` is
    only there as `origin/mainline`, and a local name would not resolve.
    """
    for candidate in (name, f"origin/{name}", f"refs/remotes/origin/{name}"):
        result = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--verify", "--quiet", candidate],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 and result.stdout.strip():
            return candidate
    die(f"the branch {name!r} is not in the clone")
    raise AssertionError


# --- the versions of the tags ------------------------------------------------


def parse_tag(tag: str) -> tuple:
    """A tag as `v0.6.0-rc1` -> ((0, 6, 0), 'rc1'); a final tag has None."""
    match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:-(.+))?", tag)
    if match is None:
        die(f"the tag {tag!r} is not v<major>.<minor>.<patch>[-<pre>]")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3))), match.group(4)


def pre_rank(pre: str) -> tuple:
    """Order the suffixes of the release candidates the way people read them."""
    match = re.match(r"([a-zA-Z]+)(\d*)", pre)
    if match is None:
        return (1, 0, pre)
    name = match.group(1).lower()
    number = int(match.group(2) or 0)
    rank = {"pre": -1, "alpha": 0, "a": 0, "beta": 1, "b": 1, "rc": 2}.get(name, 1)
    return (rank, number, pre)


def version_key(tag: str) -> tuple:
    core, pre = parse_tag(tag)
    if pre is None:
        return (*core, 1, (9, 0, ""))
    return (*core, 0, pre_rank(pre))


def tag_has_wiki(tag: str) -> bool:
    """Whether the tag carries any Markdown page of the wiki at all."""
    names = git("ls-tree", "-r", "--name-only", tag, "--", "docs/wiki").splitlines()
    return any(name.endswith(".md") for name in names)


def collect_versions(dev_ref: str = DEV_REF_DEFAULT) -> list:
    tags = []
    for tag in git("tag", "--list", "v*").splitlines():
        core, _ = parse_tag(tag)
        if core >= FIRST_VERSION:
            tags.append(tag)
    tags.sort(key=version_key)

    versions = []
    for tag in tags:
        _, pre = parse_tag(tag)
        versions.append(
            {
                "version": tag[1:],
                "tag": tag,
                "kind": "stable" if pre is None else "prerelease",
                "has_wiki": tag_has_wiki(tag),
            }
        )

    if dev_ref == "auto":
        dev_ref = next(
            (name for name in DEV_REFS if exists_ref(name)), DEV_REFS[-1]
        )
    resolved = resolve_ref(dev_ref)
    versions.append(
        {
            "version": DEV_VERSION,
            "tag": None,
            "kind": "dev",
            "has_wiki": True,
            "docs_ref": resolved,
            "ref": dev_ref,
            "commit": git("rev-parse", "--short", resolved),
        }
    )
    return versions


def entry_key(entry: dict) -> tuple:
    """Sort the development entry after every release, whatever it is."""
    if entry["kind"] == "dev":
        return (9999,)
    return version_key("v" + entry["version"])


def newest(versions: list, predicate) -> dict | None:
    found = [v for v in versions if predicate(v)]
    if not found:
        return None
    return max(found, key=entry_key)


def version_groups(versions: list) -> list:
    groups = []
    for label, kind in (
        ("Stable", "stable"),
        ("Pre-release", "prerelease"),
        ("Development", "dev"),
    ):
        items = [v for v in versions if v["kind"] == kind]
        items.sort(key=entry_key, reverse=True)
        if items:
            groups.append(
                {
                    "label": label,
                    "versions": [
                        {
                            "version": v["version"],
                            "kind": v["kind"],
                            "has_wiki": v["has_wiki"],
                        }
                        for v in items
                    ],
                }
            )
    return groups


# --- one build ----------------------------------------------------------------


def archive_wiki(tag: str, destination: Path) -> Path:
    """The `docs/wiki` of a tag, extracted outside the tree of the caller."""
    destination.mkdir(parents=True, exist_ok=True)
    git_archive = subprocess.Popen(
        ["git", "-C", str(ROOT), "archive", "--format=tar", tag, "docs/wiki"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    tar = subprocess.Popen(
        ["tar", "-x", "-C", str(destination)],
        stdin=git_archive.stdout,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    assert git_archive.stdout is not None
    git_archive.stdout.close()
    tar.wait()
    error = git_archive.stderr.read().decode("utf-8", "replace").strip()
    git_archive.wait()
    if git_archive.returncode != 0 or tar.returncode != 0:
        die(f"cannot extract docs/wiki of {tag}: {error}")
    return destination / "docs" / "wiki"


def stub_page(version: str, newest_with_wiki: str | None, stable: str | None,
              site_root: str) -> str:
    """The page of a release that shipped before the wiki existed."""
    newest = (
        f"[{newest_with_wiki}]({site_root}{newest_with_wiki}/)"
        if newest_with_wiki
        else f"[the development documentation]({site_root}dev/)"
    )
    stable_line = ""
    if stable and stable != version:
        stable_line = (
            f"The stable address of the wiki points at the newest stable tag, "
            f"`{stable}`; until a stable release carries a wiki, this note is "
            f"what it leads to.\n\n"
        )
    elif stable and stable == version:
        stable_line = (
            "The root of the wiki points at this version because it is the "
            "newest stable tag.\n\n"
        )
    return (
        f"# This version shipped without a wiki\n\n"
        f"Micro OS {version} was released before the wiki existed. The pages "
        f"appeared with 0.5.0-rc2; there is no versioned copy of them to show "
        f"for this tag, and inventing one would describe a tree nobody ran.\n\n"
        f"{stable_line}"
        f"The release itself is still on GitHub: "
        f"[the tree of {version}]({REPO_URL}/tree/v{version}/docs) and "
        f"[the source of the tag]({REPO_URL}/tree/v{version}).\n\n"
        f"| If you want to... | Read |\n"
        f"|---|---|\n"
        f"| read the newest documentation | {newest} |\n"
        f"| read the development branch | [dev]({site_root}dev/) |\n"
        f"| see every version | [all versions]({site_root}versions/) |\n\n"
        f"The canonical documents of the project are in [`docs/`]"
        f"({REPO_URL}/tree/v{version}/docs); the wiki explains how to use the "
        f"system, not why it is shaped the way it is.\n"
    )


def filtered_nav(nav: list, docs_dir: Path) -> list:
    """Keep the entries of the current nav whose page exists in this tag."""
    kept = []
    for entry in nav or []:
        for title, target in entry.items():
            if isinstance(target, str) and (docs_dir / target).is_file():
                kept.append({title: target})
    return kept


def build_config(entry: dict, docs_dir: Path, site_dir: Path, extra: dict) -> dict:
    """The mkdocs.yml of one version, with absolute paths."""
    if not BASE_CONFIG.is_file():
        die(f"there is no {BASE_CONFIG}")

    try:
        import yaml
    except ImportError:
        die(
            "PyYAML is not available; run the script with the wiki venv, for "
            "example /home/nik/.local/share/opencode/wiki-venv/bin/python"
        )

    base = yaml.safe_load(BASE_CONFIG.read_text(encoding="utf-8"))
    config = dict(base)
    config["docs_dir"] = str(docs_dir)
    config["site_dir"] = str(site_dir)
    root = str(base.get("site_url") or SITE_URL)
    if not root.endswith("/"):
        root += "/"
    config["site_url"] = root + entry["version"] + "/"
    config["theme"] = {
        "name": None,
        "custom_dir": str(THEME),
        "locale": "en",
    }
    config["plugins"] = [
        "search",
        {"mike": {"canonical_version": ""}},
    ]
    config["hooks"] = [str(ROOT / "tools" / "wiki_hooks.py")]
    # The landing page, the prefix and the rest of the site live in the extra
    # of mkdocs.yml; a version only replaces the values that name it.
    merged = dict(base.get("extra") or {})
    merged.update(extra)
    config["extra"] = merged

    if entry["has_wiki"]:
        config["nav"] = filtered_nav(base.get("nav"), docs_dir)
    else:
        config["nav"] = [{"Home": "README.md"}]
    return config


def build_one(mike: str, env: dict, entry: dict, versions: list, latest: dict,
              work: Path, clone: Path) -> None:
    version = entry["version"]
    print(f"  {version:<12} {'wiki' if entry['has_wiki'] else 'no wiki'}", flush=True)

    site_root, _ = site_settings()
    newest_with_wiki = newest(
        versions, lambda v: v["has_wiki"] and v["tag"] is not None
    )
    if entry["has_wiki"]:
        if entry["tag"] is not None:
            docs_dir = archive_wiki(entry["tag"], work / "docs" / version)
        else:
            docs_dir = archive_wiki(
                entry.get("docs_ref", DEV_REF_DEFAULT), work / "docs" / version
            )
    else:
        docs_dir = work / "docs" / version / "docs" / "wiki"
        docs_dir.mkdir(parents=True, exist_ok=True)
        stable = newest(versions, lambda v: v["kind"] == "stable")
        (docs_dir / "README.md").write_text(
            stub_page(
                version,
                newest_with_wiki["version"] if newest_with_wiki else None,
                stable["version"] if stable else None,
                site_root,
            ),
            encoding="utf-8",
        )

    latest_stable = newest(versions, lambda v: v["kind"] == "stable")
    extra = {
        "version": version,
        "version_kind": entry["kind"],
        "has_wiki": entry["has_wiki"],
        "latest": latest["version"] if latest else "",
        "latest_stable": latest_stable["version"] if latest_stable else "",
        "latest_stable_has_wiki": bool(latest_stable and latest_stable["has_wiki"]),
        "newest": newest_with_wiki["version"] if newest_with_wiki else "",
        "dev_ref": entry.get("ref", ""),
        "dev_commit": entry.get("commit", ""),
        "wiki_ref": entry["tag"] or entry.get("ref", "main"),
        "wiki_docs_prefix": "docs/wiki",
        "edit_base": f"{REPO_URL}/blob/{entry['tag'] or entry.get('ref', 'main')}/docs/wiki/",
        "version_groups": version_groups(versions),
    }
    site_dir = work / "build" / version
    config_path = work / f"mkdocs-{version}.yml"

    try:
        import yaml
    except ImportError:  # the message is produced by build_config as well
        die("PyYAML is not available")

    config = build_config(entry, docs_dir, site_dir, extra)
    config_path.write_text(
        yaml.safe_dump(config, sort_keys=False, allow_unicode=True),
        encoding="utf-8",
    )

    title = version
    if entry["kind"] == "prerelease":
        title += " (pre-release)"
    elif entry["kind"] == "dev":
        title += " (development)"

    command = [
        mike,
        "deploy",
        "--config-file",
        str(config_path),
        "--branch",
        BRANCH,
        "--ignore-remote-status",
        "--alias-type",
        "redirect",
        "--allow-empty",
        "--title",
        title,
        "--prop-set-string",
        f"kind={entry['kind']}",
        "--prop-set",
        f"has_wiki={'true' if entry['has_wiki'] else 'false'}",
        "--prop-set-string",
        f"ref={entry['tag'] or entry.get('ref', 'main')}",
    ]
    if entry.get("commit"):
        command += ["--prop-set-string", f"commit={entry['commit']}"]
    command.append(version)
    run(command, cwd=clone, env=env)

    if site_dir.is_dir():
        shutil.rmtree(site_dir)


# --- the pages outside the versions ------------------------------------------


def write_root_pages(site: Path, versions: list, cname: str) -> None:
    """The human list of versions, the 404 and the optional CNAME."""
    _, base = site_settings()
    assets = site / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name in ("wiki.css", "favicon.svg"):
        shutil.copy2(THEME / "assets" / name, assets / name)

    rows = []
    for entry in sorted(
        versions, key=entry_key, reverse=True
    ):
        link = f'<a href="{base}{entry["version"]}/">{entry["version"]}</a>'
        channel = {
            "stable": "stable",
            "prerelease": "pre-release",
            "dev": "development",
        }[entry["kind"]]
        state = "" if entry["has_wiki"] else "no wiki; the tag predates the pages"
        if entry["kind"] == "dev":
            state = f'built from <code>{html.escape(entry.get("ref", "main"))}</code>'
        rows.append(
            f"<tr><td>{link}</td><td>{channel}</td><td>{state}</td></tr>"
        )

    built = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    page = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <link rel="icon" href="{base}assets/favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="{base}assets/wiki.css">
  <title>All versions — Micro OS wiki</title>
</head>
<body>
<header class="top">
  <div class="top-inner">
    <a class="brand" href="{base}">Micro&nbsp;OS <span>wiki</span></a>
    <span class="repo">All versions</span>
  </div>
</header>
<main class="all-pages">
  <h1>All versions</h1>
  <p class="lead">One page tree per release tag and one for the branch. The
  root of the site follows the newest stable tag; a release older than
  0.5.0-rc2 carries a note instead of pages, because the wiki did not exist
  when it shipped.</p>
  <table>
    <thead><tr><th>Version</th><th>Channel</th><th>Note</th></tr></thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>
  <p class="built">Built {built}; the development documentation follows the
  branch at every push.</p>
</main>
<footer><div class="footer-inner"><div>Micro OS wiki</div>
<div class="footer-links"><a href="{base}">the current version</a>
<a href="{REPO_URL}">the source</a></div></div></footer>
</body>
</html>
"""
    versions_dir = site / "versions"
    versions_dir.mkdir(parents=True, exist_ok=True)
    (versions_dir / "index.html").write_text(page, encoding="utf-8")

    not_found = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <link rel="icon" href="{base}assets/favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="{base}assets/wiki.css">
  <title>Page not found — Micro OS wiki</title>
</head>
<body>
<header class="top">
  <div class="top-inner">
    <a class="brand" href="{base}">Micro&nbsp;OS <span>wiki</span></a>
  </div>
</header>
<main class="all-pages">
  <h1>Page not found</h1>
  <p>The address does not belong to a page of the wiki. Try
  <a href="{base}">the current version</a> or
  <a href="{base}versions/">the list of every version</a>.</p>
</main>
</body>
</html>
"""
    (site / "404.html").write_text(not_found, encoding="utf-8")

    if cname:
        (site / "CNAME").write_text(cname + "\n", encoding="utf-8")


# --- the entry point ----------------------------------------------------------


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="Build every version of the wiki.")
    parser.add_argument(
        "--site",
        type=Path,
        default=ROOT / "site",
        help="the directory of the built site (default: site/)",
    )
    parser.add_argument(
        "--cname",
        default="",
        help="the CNAME of a custom domain (default: none; the Pages project "
        "page needs no file)",
    )
    parser.add_argument(
        "--only",
        default="",
        help="a comma-separated list of versions to build, for a quick look",
    )
    parser.add_argument(
        "--dev-ref",
        default=DEV_REF_DEFAULT,
        help="the branch the development pages are read from (default: auto: "
        "release_candidate when it exists, else mainline)",
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="keep the temporary clone and the builds for debugging",
    )
    args = parser.parse_args(argv)

    missing = [
        name
        for name in ("mkdocs", "mike")
        if shutil.which(name, path=str(Path(sys.executable).parent) + os.pathsep
                        + os.environ.get("PATH", "")) is None
    ]
    if missing:
        die(
            f"{', '.join(missing)} not on PATH; run the script with the wiki "
            "venv, for example\n"
            "  /home/nik/.local/share/opencode/wiki-venv/bin/python "
            "tools/wiki_build_versions.py"
        )
    try:
        import yaml  # noqa: F401
    except ImportError:
        die("PyYAML is not installed; use the wiki venv")

    versions = collect_versions(args.dev_ref)
    latest = newest(versions, lambda v: v["kind"] == "stable")
    if latest is None:
        latest = newest(versions, lambda v: v["has_wiki"] and v["tag"] is not None)

    chosen = versions
    if args.only:
        names = {name.strip() for name in args.only.split(",") if name.strip()}
        chosen = [v for v in versions if v["version"] in names]
        if not chosen:
            die(f"--only names no version: {args.only}")

    site = args.site.resolve()
    print(f"wiki: {len(chosen)} version(s) -> {site}", flush=True)
    print(
        f"  latest stable: {latest['version'] if latest else '(none)'}; "
        f"root redirects there",
        flush=True,
    )

    work = Path(tempfile.mkdtemp(prefix="microos-wiki-"))
    clone = work / "repo"
    env = os.environ.copy()
    env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env.get("PATH", "")
    env.setdefault("GIT_COMMITTER_NAME", "Micro OS wiki builder")
    env.setdefault("GIT_COMMITTER_EMAIL", "wiki@microos.org")
    mike = shutil.which("mike", path=env["PATH"])
    if mike is None:
        die("mike is not on PATH of the venv")

    try:
        run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(ROOT), str(clone)])
        run(["git", "-C", str(clone), "config", "user.name", "Micro OS wiki builder"])
        run(["git", "-C", str(clone), "config", "user.email", "wiki@microos.org"])

        for entry in chosen:
            build_one(mike, env, entry, versions, latest, work, clone)

        # Everything is deployed; the alias and the root come last because
        # they point at a version that has to exist already.
        target = latest or chosen[-1]
        configs = sorted(work.glob("mkdocs-*.yml"))
        config_path = next(
            (path for path in configs if path.name == f"mkdocs-{target['version']}.yml"),
            configs[-1],
        )
        run(
            [
                mike,
                "alias",
                "--config-file",
                str(config_path),
                "--branch",
                BRANCH,
                "--ignore-remote-status",
                "--alias-type",
                "redirect",
                "--update-aliases",
                "--allow-empty",
                target["version"],
                "latest",
            ],
            cwd=clone,
            env=env,
        )
        run(
            [
                mike,
                "set-default",
                "--config-file",
                str(config_path),
                "--branch",
                BRANCH,
                "--ignore-remote-status",
                "--allow-empty",
                "latest",
            ],
            cwd=clone,
            env=env,
        )

        if site.exists():
            shutil.rmtree(site)
        site.mkdir(parents=True)
        archive = subprocess.Popen(
            ["git", "-C", str(clone), "archive", "--format=tar", BRANCH],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        tar = subprocess.Popen(
            ["tar", "-x", "-C", str(site)],
            stdin=archive.stdout,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        assert archive.stdout is not None
        archive.stdout.close()
        tar.wait()
        archive.wait()
        if archive.returncode != 0 or tar.returncode != 0:
            die("cannot export the built branch into " + str(site))

        write_root_pages(site, versions, args.cname)
    finally:
        if args.keep_temp:
            print(f"  kept: {work}", flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)

    total = sum(1 for _ in site.rglob("*") if _.is_file())
    print(f"wiki: {total} file(s) in {site}", flush=True)
    print(f"  open {site / 'index.html'} or serve {site} over HTTP", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
