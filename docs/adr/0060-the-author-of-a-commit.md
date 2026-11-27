# ADR 0060 - The author of a commit, and the tools that help

* Status: accepted
* Date: 2026-11-27

## Context

The contributors of a repository are computed by the hosting service from the
`author` and `Co-authored-by:` fields of a commit. Two commits of the 2.1
history carry `Co-authored-by: Claude Opus 4.8 <noreply@anthropic.com>`, added by
an agent that wrote part of the C23 change, and the model appeared among the
contributors of the project.

That is a question of authorship, not of preference. An AI assistant is a tool:
the work it helps to produce belongs to the person who directed it, the
assistant holds no copyright in it and cannot hold one, and its name is not a
party to the license of the tree. A trailer that names a model in a commit is a
convention of a tool, and a reader of the history is entitled to know who wrote
the change.

## Decision

* **A commit is signed by the person who made it.** The `author` field is a
  person; the email is theirs.
* **`Co-authored-by:` names a person who wrote part of the change** - a
  colleague of the team, a contributor of a patch, a pair of hands. It never
  names a model, an editor, a generator or a script.
* **The history describes the change and not the tools.** A commit that an
  assistant helped to write says what changed and why; the tool is named in the
  message only when the change is about the tool (a port of a driver, a
  generated table with its provenance).
* **The license is the grant of the authors.** `LICENSE` is MIT, the holders are
  the owner and the people who contributed; nothing in a trailer changes that,
  and a change of the license is a decision of theirs.
* `NOTICE.md` at the root says all of this for a reader who does not read ADRs.

The two commits of 2.1 are left as they are: rewriting a history that a release
has published is a cost that buys nothing, because the trailer never was a
claim in the first place.

## Consequences

An agent of this tree never adds a `Co-authored-by:` with a model (`AGENTS.md`,
"Commits"). A contributor who receives a patch from an assistant signs the
commit themselves. The contributors list of the hosting service names the
people, which is what it is for.
