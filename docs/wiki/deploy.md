# Building and deploying the wiki

The wiki is one MkDocs project built many times: once per release tag and once
for the branch. The result is a static site with a mike-compatible layout; it
is published to GitHub Pages from a branch, never from the tree that holds the
kernel sources.

The theme, the builder and the workflow live on the **`wiki` branch**, and
that branch is a leaf: it takes the kernel from `mainline` and the releases
from the tags, builds them, and is never merged back into `mainline`, a
release candidate or a feature branch. The kernel tree carries the pages
(`docs/wiki/`) and the link check; it does not carry the site.

The site is a **project page**, so its address and the prefix of every path is
fixed by the repository, not by a domain:

```
https://turbososiska304.github.io/Micro_OS/
```

All links the theme emits are built from one value, `BASE_PATH =
/Micro_OS/`, and the links inside a version are relative. A future domain
changes that value, `SITE_URL` and the CNAME together; nothing else in the
pages has to move. The landing page of the project is a separate site,
<https://turbososiska304.github.io/microos-site/>.

## What the build produces

`tools/wiki_build_versions.py` writes one directory:

```
site/
├── index.html            redirect to latest/ (the newest stable tag)
├── .nojekyll             GitHub Pages must not run Jekyll over the site
├── versions.json         the versions mike knows (version, title, aliases)
├── latest/               redirect alias of the newest stable tag
├── 0.4.0/                a tag: its own docs/wiki, or the note that it had none
├── 0.5.0-rc2/
├── 0.6.0-rc1/
├── dev/                  the current branch, built at its tip
├── versions/index.html   the human list of every version
├── assets/               the stylesheet and the icon of the list and the 404
└── 404.html
```

The rule for the versions is mechanical: every tag of `v0.4.0` and later is
built, plus `dev`. A tag whose tree has no `docs/wiki` (the wiki appeared with
`0.5.0-rc2`) gets a short page that says so and points at the newest
documentation instead of an empty page tree. The index of the version is in
[versions.md](versions.md).

## Requirements

The system Python is externally managed (PEP 668), so the tools live in a
venv outside the repository:

```bash
python3 -m venv ~/.local/share/opencode/wiki-venv
~/.local/share/opencode/wiki-venv/bin/pip install mkdocs mike pygments
```

`pygments` is what colours the code blocks; `mike` owns the version
directories, the aliases and `versions.json`. The script checks that `mkdocs`
and `mike` belong to the interpreter that runs it.

## Building all versions

```bash
~/.local/share/opencode/wiki-venv/bin/python tools/wiki_build_versions.py
# site/ is replaced as a whole; the repository is not modified
```

The script does not disturb the working tree: it makes a `git clone --shared`
in a temporary directory, extracts each tag's `docs/wiki` with `git archive`,
builds it with the theme of the current tree, and runs `mike deploy` in the
clone. That branch (`gh-pages`) exists only in the clone and is exported into
`site/` at the end; the clone is removed. Nothing is pushed.

Useful options:

```bash
# one version while editing the theme
... tools/wiki_build_versions.py --only 0.6.0-rc1,dev

# build somewhere else, keep the temporary clone for inspection
... tools/wiki_build_versions.py --site /tmp/wiki --keep-temp

# a custom domain (also changes the links only if BASE_PATH is changed)
... tools/wiki_build_versions.py --cname wiki.example.org
```

`latest` is the newest stable tag, and the root of the site redirects there.
That is why the root currently shows the note of `0.4.0`: it is the newest
stable release, and it shipped before the wiki existed. The newest pages are
the newest release candidate and `/Micro_OS/dev/`.

## Previewing locally

The site expects to live under `/Micro_OS/`. Serve a parent directory that
contains that name, either a real copy or a symlink:

```bash
mkdir -p /tmp/microos-wiki-root
ln -sfn "$PWD/site" /tmp/microos-wiki-root/Micro_OS
python3 -m http.server 8000 --bind 0.0.0.0 --directory /tmp/microos-wiki-root
```

Then open <http://127.0.0.1:8000/Micro_OS/>; the version switch, `/dev/` and
the search work exactly as on Pages, prefix included. Serving `site/` itself
at the root of a server is not the deployment: the absolute prefix
`/Micro_OS/` would point outside the served tree.

A single version without the prefix is still available for a quick edit:

```bash
~/.local/share/opencode/wiki-venv/bin/mkdocs serve
```

That build uses the `dev` entry of `extra` in `mkdocs.yml`; the version links
and the "All versions" link assume the Pages prefix and do not resolve on that
bare server. The full preview above is the one that matches production.

## Publishing

There are two ways to publish, with the same `site/` as the artifact. The
first is the one this repository uses today.

### A. The `gh-pages` branch of `Micro_OS` (default)

The built site is a branch of the same repository: `gh-pages`, at its root, no
Jekyll, served by GitHub Pages at `/Micro_OS/`. The kernel sources never carry
the generated HTML; the branch is an orphan of the `wiki` history.

The GitHub Actions workflow `.github/workflows/wiki.yml` lives on the `wiki`
branch, builds on every push to it, every night (a release is a tag on
`mainline`, which this branch does not see as a push) and by hand
(`gh workflow run wiki.yml --ref wiki`), then publishes `site/` to `gh-pages`
with `GITHUB_TOKEN` (`permissions: contents: write`). The checkout is the
`wiki` branch, which carries the builder and the theme; each release is read
from its tag with `git archive`. No secret is needed for this path; the
workflow uses the token GitHub gives the run.

To do the same by hand (and **only** when publishing is intended):

```bash
~/.local/share/opencode/wiki-venv/bin/python tools/wiki_build_versions.py
cd site
git init -b gh-pages
git add -A
git commit -m "docs: build the wiki"
git remote add origin git@github.com:TurboSosiska304/Micro_OS.git
git push --force origin gh-pages
```

The one setting outside the repository is in *Settings → Pages*: the source
is the branch `gh-pages` and the directory `/ (root)`. GitHub then serves
<https://turbososiska304.github.io/Micro_OS/>. The branch is fully rebuilt on
every publish, so a forced push is correct: the versions are derived from the
tags, not edited by hand.

### B. A separate repository `microos-wiki`

If the built HTML should not live next to the kernel at all, the same `site/`
goes to a repository of its own (for example `TurboSosiska304/microos-wiki`,
branch `gh-pages`). That history is nothing but site builds, and the Pages
address becomes `https://turbososiska304.github.io/microos-wiki/`; the
`BASE_PATH` has to become `/microos-wiki/` or a custom domain is needed, or
the repository has to be named `TurboSosiska304.github.io`. This is the
reason the branch in the same repository is the default: the prefix is stable
without a second name.

In the workflow, replace the publish step with an external repository and a
personal token that can write to it:

```yaml
- uses: peaceiris/actions-gh-pages@v4
  with:
    personal_token: ${{ secrets.WIKI_DEPLOY_TOKEN }}
    external_repository: TurboSosiska304/microos-wiki
    publish_branch: gh-pages
    publish_dir: ./site
```

`WIKI_DEPLOY_TOKEN` is a fine-grained PAT with *Contents: read and write* on
`microos-wiki` only; it is a repository secret, not a value in the tree.
Nothing in this tree pushes on its own.

## Moving to a domain

When a domain exists, four things change and nothing else:

1. `site_url` in `mkdocs.yml` and `SITE_URL` in `tools/wiki_build_versions.py`
   become `https://wiki.example.org/`;
2. `extra.base_path` in `mkdocs.yml` and `BASE_PATH` in the script become `/`;
3. the build writes a CNAME (`--cname wiki.example.org`), or the file is
   added to `site/` before publishing;
4. the DNS of the domain gets the `CNAME`/A records GitHub Pages asks for, and
   *Settings → Pages → Custom domain* names it.

The theme and the version switch need no edits: they build every link from
`base_path`. `mike` needs no edits either: its aliases and redirect pages are
relative, and the version directories do not move.

## Adding, changing and removing a version

A version is a tag; there is no list to maintain in the wiki:

```bash
git tag -a v0.6.0 -m "Micro OS 0.6.0"
git push origin v0.6.0           # only when the release is intended
```

The next build of the workflow picks the tag up. The alias `latest` follows
the newest stable tag on its own, so `latest` and the root move from `0.4.0`
to `0.6.0` in the same run, and the note of `0.4.0` starts pointing at the
pages of `0.6.0`.

`dev` is not a tag: it is built from the branch of the run (`wiki` in CI),
and its banner names the commit. A push to `wiki` is enough.

With mike, the same operations on the branch are:

```bash
mike list --branch gh-pages
mike delete --branch gh-pages 0.4.0-rc1
mike alias --branch gh-pages --update-aliases 0.6.0 latest
mike set-default --branch gh-pages latest
```

They are listed for completeness; in this repository the branch is written by
the build script as a whole, and editing it by hand would be undone by the
next publish.

## What is not part of the build

`make ci` and `make integrity` do not need MkDocs: the wiki is Markdown that
GitHub also renders in place, and the link checker
(`python3 tools/wiki_check.py`) is a separate, hand-run check because the wiki
links into the rest of `docs/` on purpose. `tools/wiki_hooks.py` rewrites a
link that leaves `docs_dir` to the same file on GitHub at the ref of the
build, so the canonical documents stay reachable from the site. The theme is
three small files (`docs/theme/main.html`,
`docs/theme/assets/wiki.css`, `docs/theme/assets/favicon.svg`); the site has
no JavaScript of its own, and the search index is the only script (MkDocs'
`search` plugin).
