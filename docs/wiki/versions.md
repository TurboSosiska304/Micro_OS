# Versions of this wiki

The wiki is built once per release and served from GitHub Pages, with one page
tree for each version. Nothing here is a promise that an old page still
describes the tree: a version page is a photograph of the documentation at
that tag, and the source of the same version is a checkout away.

## The addresses

The site is a project page of the repository, so every path begins with
`/Micro_OS/`; the links of the site already carry that prefix.

| Address | What it holds |
|---|---|
| [turbososiska304.github.io/Micro_OS](https://turbososiska304.github.io/Micro_OS/) | the latest stable release; the root redirects to it |
| `https://turbososiska304.github.io/Micro_OS/<version>/` | the wiki of a tag, `0.4.0`, `0.6.0-rc1` and so on |
| [Micro_OS/dev](https://turbososiska304.github.io/Micro_OS/dev/) | the wiki of the main branch, built on every push |
| [Micro_OS/versions](https://turbososiska304.github.io/Micro_OS/versions/) | every version that was ever built, including the old ones |
| [microos-site](https://turbososiska304.github.io/microos-site/) | the landing page of the project |

The version menu in the header lists the release tags and `dev`; the link
*All versions* at its foot is the full list, with the note of each version.

## Which pages a version has

The wiki appeared in `0.5.0-rc2`. A release older than that - `0.4.0` and the
release candidates before it - shipped without these pages, and its directory
holds a short note that says so rather than an empty page tree. The root of
the site still points at the newest stable tag, because that is what a root
address means; until a stable release carries a wiki, the note leads to the
newest documentation that does exist. The pages of [`docs/`](../README.md)
are the canonical documents for every version.

## The banners

| Banner | Meaning |
|---|---|
| none | the stable release, the documentation of the tag at the root |
| *pre-release* | a release candidate: the pages may change before the tag becomes final |
| *development* | the branch, built at a commit that is not a tag; the banner names the commit |

The banner is a plain page element, not a script: a version switch is a set of
links, and the site works with JavaScript while search is the only feature
that wants it (the search index itself is static).

## How a version is built

`tools/wiki_build_versions.py` checks out every tag from `0.4.0` on into a
temporary clone, builds the tag's own `docs/wiki` with the theme of this tree,
and lets `mike` write the mike-compatible layout (`<version>/`,
`versions.json`, the `latest` alias and the root redirect). The production
deployment is described in [deploy.md](deploy.md); a version is added by
tagging a release, not by editing this page.
