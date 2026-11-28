#!/usr/bin/env bash
#
# The checks a release needs *after* the tag, which is where 2.3.0 was caught
# with a site that still said "plan" and a hero that still showed the painting
# of 2.1.
#
# Usage: tools/release_check.sh v2.4.0 [path-to-site]
#
# It reads, it does not write: every "no" is a thing a person has to fix before
# the release is called done. Exit code is the number of failed checks.

set -u

TAG="${1:-}"
SITE="${2:-/home/nik/microos/microos-site}"
FAILED=0

fail() {
    printf '  NO   %s\n' "$1"
    FAILED=$((FAILED + 1))
}

pass() {
    printf '  ok   %s\n' "$1"
}

if [ -z "$TAG" ]; then
    echo "usage: $0 vX.Y.Z [path-to-site]" >&2
    exit 2
fi

VERSION="${TAG#v}"

echo "the tag"
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    pass "the tag $TAG exists at $(git rev-parse --short "$TAG^{commit}")"
else
    fail "the tag $TAG does not exist"
fi

echo "the release page"
if ! command -v gh >/dev/null; then
    fail "gh is not installed, the release page cannot be checked"
else
    ASSETS="$(gh release view "$TAG" --json assets -q '.assets | length' 2>/dev/null || echo 0)"
    if [ "${ASSETS:-0}" -ge 16 ]; then
        pass "the release page carries $ASSETS assets"
    else
        fail "the release page carries ${ASSETS:-0} assets, expected at least 16"
    fi

    for name in "MicroOS-$VERSION-uefi.img" "MicroOS-$VERSION.iso" "checksums.txt"; do
        if gh release view "$TAG" --json assets -q '.assets[].name' 2>/dev/null |
            grep -qx "$name"; then
            pass "the asset $name is uploaded"
        else
            fail "the asset $name is missing"
        fi
    done
fi

echo "the site"
if [ ! -d "$SITE" ]; then
    fail "the site is not at $SITE"
else
    # The entry of a release is a block, not a line: read from its `version:`
    # to the closing brace and look for the status inside it.
    STATUS="$(awk -v v="$VERSION" '
        $0 ~ "version: \""v"\"," { inside = 1 }
        inside && /status: "/ { print; inside = 0 }
    ' "$SITE/src/gallery.ts" 2>/dev/null)"
    if echo "$STATUS" | grep -q 'status: "stable"'; then
        pass "the gallery carries $VERSION as stable"
    else
        fail "the gallery does not carry $VERSION as stable (${STATUS:-no entry})"
    fi

    if grep -qE "version: \"[0-9]+\.[0-9]+\"" "$SITE/src/gallery.ts" >/dev/null 2>&1; then
        :
    fi

    # The page must not name a version older than the release as "latest".
    OLD="$(grep -rhoE 'v[0-9]+\.[0-9]+\.[0-9]+' "$SITE/src" "$SITE/index.html" 2>/dev/null |
        sort -u | grep -v "v$VERSION" | head -5)"
    if [ -z "$OLD" ]; then
        pass "no other version is named in the site sources"
    else
        fail "the site still names other versions: $(echo "$OLD" | tr '\n' ' ')"
    fi

    if grep -q 'name: "plan"' "$SITE/src/gallery.ts" 2>/dev/null; then
        fail "a release is still marked as a plan in the gallery"
    else
        pass "no release is left as a plan"
    fi

    SHORT="${VERSION%.*}"
    if [ -f "$SITE/CREDITS.md" ] &&
        grep -qiE "$VERSION|v$VERSION|$SHORT" "$SITE/CREDITS.md"; then
        pass "CREDITS.md names $VERSION"
    else
        fail "CREDITS.md does not name $VERSION"
    fi
fi

echo "the tree"
if grep -q "KERNEL_VERSION_MAJOR\|KERNEL_VERSION_MINOR\|KERNEL_VERSION_PATCH" \
    components/kernel/include/kernel/config.h 2>/dev/null; then
    M="$(grep -oE 'KERNEL_VERSION_MAJOR +[0-9]+' components/kernel/include/kernel/config.h |
        grep -oE '[0-9]+')"
    N="$(grep -oE 'KERNEL_VERSION_MINOR +[0-9]+' components/kernel/include/kernel/config.h |
        grep -oE '[0-9]+')"
    P="$(grep -oE 'KERNEL_VERSION_PATCH +[0-9]+' components/kernel/include/kernel/config.h |
        grep -oE '[0-9]+')"
    if [ "$M.$N.$P" = "$VERSION" ]; then
        pass "the kernel version is $M.$N.$P"
    else
        fail "the kernel version is $M.$N.$P, the tag is $VERSION"
    fi
else
    fail "the kernel version file is not readable"
fi

if [ -f CHANGELOG.md ] && grep -q "\[$VERSION\]" CHANGELOG.md; then
    pass "CHANGELOG.md carries [$VERSION]"
else
    fail "CHANGELOG.md does not carry [$VERSION]"
fi

if [ -f "docs/releases/${M:-0}.${N:-0}.md" ]; then
    pass "the release notes are there"
else
    fail "docs/releases/${M:-0}.${N:-0}.md is missing"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "every check passed: the release of $TAG is whole"
else
    echo "$FAILED check(s) did not pass: the release of $TAG is not whole yet"
fi

exit "$FAILED"
