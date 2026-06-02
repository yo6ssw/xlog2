#!/usr/bin/env bash
#
# Cut a release: bump the version in CMakeLists.txt and debian/changelog, commit,
# and create the annotated tag vX.Y.Z. Keeps the build version, the package
# version and the tag from drifting apart (see VERSIONING.md).
#
# Usage:
#   packaging/release.sh X.Y.Z
# then:
#   git push origin <branch> --follow-tags
#   packaging/publish-ppa.sh
#
set -euo pipefail

ver="${1:-}"
if [[ ! "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: $0 MAJOR.MINOR.PATCH" >&2
    exit 2
fi

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$REPO"

DEBFULLNAME="${DEBFULLNAME:-Adrian Scripca}"
DEBEMAIL="${DEBEMAIL:-benishor@gmail.com}"
tag="v$ver"

# Refuse to run on a dirty tree or an existing tag — releases come from a clean,
# committed state.
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "working tree has uncommitted changes; commit or stash first." >&2
    exit 1
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "tag $tag already exists." >&2
    exit 1
fi

cur="$(grep -oP '^[[:space:]]*VERSION[[:space:]]+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
echo "Releasing $ver (was $cur)"

# 1) Bump the canonical version in CMakeLists.txt (the project() VERSION line,
#    not cmake_minimum_required's).
sed -i -E "s/^([[:space:]]*VERSION[[:space:]]+)[0-9]+\.[0-9]+\.[0-9]+/\1${ver}/" CMakeLists.txt
got="$(grep -oP '^[[:space:]]*VERSION[[:space:]]+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
[[ "$got" == "$ver" ]] || { echo "failed to bump VERSION in CMakeLists.txt" >&2; exit 1; }

# 2) Prepend a new debian/changelog stanza (native version, no ~series suffix;
#    publish-ppa.sh adds the per-series suffix at upload time).
tmp="$(mktemp)"
{
    printf 'xlog2 (%s) noble; urgency=medium\n\n  * Release %s.\n\n -- %s <%s>  %s\n\n' \
        "$ver" "$ver" "$DEBFULLNAME" "$DEBEMAIL" "$(date -R)"
    cat debian/changelog
} > "$tmp"
mv "$tmp" debian/changelog

# 3) Commit and tag.
git add CMakeLists.txt debian/changelog
git commit -q -m "Release $ver"
git tag -a "$tag" -m "xlog2 $ver"

echo "Committed and tagged $tag."
echo "Next: git push origin \"\$(git branch --show-current)\" --follow-tags"
echo "      packaging/publish-ppa.sh"
