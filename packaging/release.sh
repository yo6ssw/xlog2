#!/usr/bin/env bash
#
# Cut a release: bump the version in CMakeLists.txt, debian/changelog and the
# Android app (android/app/build.gradle.kts), commit, and create the signed
# annotated tag vX.Y.Z. Keeps the build version, the package version, the Android
# app version and the tag from drifting apart (see VERSIONING.md). The Android
# versionName must match the tag so F-Droid autoupdate tracks new releases.
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

# 1b) Bump the Android app version in lockstep. versionName must equal the tag's
#     version (F-Droid autoupdate maps it back to the tag); versionCode uses the
#     same scheme as build.gradle.kts: major*1000000 + minor*10000 + patch*100.
gradle="android/app/build.gradle.kts"
if [[ -f "$gradle" ]]; then
    IFS=. read -r maj min pat <<<"$ver"
    code=$(( 10#$maj * 1000000 + 10#$min * 10000 + 10#$pat * 100 ))
    sed -i -E "s/^([[:space:]]*versionCode = )[0-9]+/\1${code}/" "$gradle"
    sed -i -E "s/^([[:space:]]*versionName = )\"[0-9]+\.[0-9]+\.[0-9]+\"/\1\"${ver}\"/" "$gradle"
    gotname="$(grep -oP '^\s*versionName = "\K[0-9]+\.[0-9]+\.[0-9]+' "$gradle" | head -1)"
    gotcode="$(grep -oP '^\s*versionCode = \K[0-9]+' "$gradle" | head -1)"
    [[ "$gotname" == "$ver" && "$gotcode" == "$code" ]] \
        || { echo "failed to bump Android version in $gradle" >&2; exit 1; }
    echo "Android app: versionName $ver, versionCode $code"
fi

# 2) Prepend a new debian/changelog stanza (native version, no ~series suffix;
#    publish-ppa.sh adds the per-series suffix at upload time).
tmp="$(mktemp)"
{
    printf 'xlog2 (%s) noble; urgency=medium\n\n  * Release %s.\n\n -- %s <%s>  %s\n\n' \
        "$ver" "$ver" "$DEBFULLNAME" "$DEBEMAIL" "$(date -R)"
    cat debian/changelog
} > "$tmp"
mv "$tmp" debian/changelog

# 3) Commit and tag (GPG-signed, to match the signed release history).
git add CMakeLists.txt debian/changelog
[[ -f "$gradle" ]] && git add "$gradle"
git commit -qS -m "Release $ver"
git tag -s "$tag" -m "xlog2 $ver"

echo "Committed and tagged $tag."
echo "Next: git push origin \"\$(git branch --show-current)\" --follow-tags"
echo "      packaging/publish-ppa.sh"
