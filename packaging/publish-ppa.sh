#!/usr/bin/env bash
#
# Build signed Debian *source* packages of xlog2 and upload them to a Launchpad
# PPA, which then builds the xlog2-gtk and xlog2-qt binaries for each series.
#
# Usage:
#   packaging/publish-ppa.sh [--no-upload] [--series "noble jammy"] [--rev N]
#
#   --no-upload   build + sign the source packages but don't dput them
#   --series ...  space-separated Ubuntu series (overrides SERIES below)
#   --rev N       per-series revision suffix (bump to re-upload same version)
#
# Prerequisites (install once):
#   sudo apt install devscripts debhelper dput
# and a GPG key whose UID matches DEBEMAIL, registered on your Launchpad
# account (https://launchpad.net/~benishor/+editpgpkeys).
#
set -euo pipefail

# --- configuration -----------------------------------------------------------
PPA="ppa:benishor/hamtools"
DEBFULLNAME="Adrian Scripca"
DEBEMAIL="benishor@gmail.com"
KEYID="18B97354B106F3841ADEC0CF85BED1A01D653065"  # sign with this key by fingerprint
                              # (the key's UID is "Adrian Scripca (benishor) <…>",
                              #  which debsign can't match from the bare changelog
                              #  identity, so select it explicitly)
SERIES=(noble plucky resolute)  # Ubuntu series to publish for (24.04, 25.04, 26.04)
REV=1                         # per-series revision; bump to re-upload a version
export DEBFULLNAME DEBEMAIL

# --- args --------------------------------------------------------------------
UPLOAD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-upload) UPLOAD=0; shift ;;
        --series)    read -r -a SERIES <<< "$2"; shift 2 ;;
        --rev)       REV="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- locate repo root --------------------------------------------------------
REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$REPO"

# --- tool check --------------------------------------------------------------
need=()
for t in dpkg-buildpackage dpkg-source debsign git; do
    command -v "$t" >/dev/null || need+=("$t")
done
command -v dh >/dev/null || need+=("debhelper (dh)")
(( UPLOAD )) && { command -v dput >/dev/null || need+=("dput"); }
if (( ${#need[@]} )); then
    echo "Missing tools: ${need[*]}" >&2
    echo "Install with: sudo apt install devscripts debhelper dput" >&2
    exit 1
fi

# --- version -----------------------------------------------------------------
BASEVER="$(grep -oP 'VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
[[ -n "$BASEVER" ]] || { echo "could not read project VERSION from CMakeLists.txt" >&2; exit 1; }

# debian/ must be committed: we publish exactly what's in HEAD.
if ! git diff --quiet HEAD -- debian packaging CMakeLists.txt; then
    echo "WARNING: uncommitted changes to debian/, packaging/ or CMakeLists.txt;" >&2
    echo "         the upload reflects HEAD (committed state), not your working tree." >&2
fi

OUT="$REPO/dist"
mkdir -p "$OUT"
echo "Publishing xlog2 $BASEVER to $PPA for: ${SERIES[*]}"

for series in "${SERIES[@]}"; do
    ver="${BASEVER}~${series}${REV}"
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT

    # Clean, deterministic export of the committed tree (no build/, .git, …).
    git archive --format=tar HEAD | tar -x -C "$work"

    # Per-series changelog (version + target distribution).
    cat > "$work/debian/changelog" <<EOF
xlog2 ($ver) $series; urgency=medium

  * Build for $series.

 -- $DEBFULLNAME <$DEBEMAIL>  $(date -R)
EOF

    echo "==> $series: building source package xlog2_${ver}"
    ( cd "$work"
      # -S source only, -d skip build-dep check (not building binaries here),
      # -us -uc unsigned (we debsign explicitly below).
      dpkg-buildpackage -S -d -us -uc
      debsign ${KEYID:+-k"$KEYID"} "../xlog2_${ver}_source.changes"
    )

    cp "$work"/../xlog2_"${ver}"* "$OUT"/ 2>/dev/null || true
    changes="$OUT/xlog2_${ver}_source.changes"

    if (( UPLOAD )); then
        echo "==> $series: dput -> $PPA"
        dput "$PPA" "$changes"
    else
        echo "==> $series: built $changes (upload skipped)"
    fi

    rm -rf "$work"; trap - EXIT
done

echo "Done. Artifacts in $OUT/"
(( UPLOAD )) && echo "Watch the builds at https://launchpad.net/~benishor/+archive/ubuntu/hamtools"
