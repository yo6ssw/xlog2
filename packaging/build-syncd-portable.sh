#!/bin/sh
#
# Build a portable xlog2-syncd tarball (dynamic; depends only on the ubiquitous
# sqlite3/curl/sodium + libc). Build on the OLDEST glibc you want to support
# (Ubuntu 22.04, glibc 2.35) so it runs on Raspberry Pi OS bookworm and newer.
#
#   VER=0.6.0 ARCH=x86_64 packaging/build-syncd-portable.sh   # or ARCH=aarch64
#
# Produces xlog2-syncd-<VER>-linux-<ARCH>.tar.gz (binary + user unit + install.sh).
#
set -eux

: "${VER:=dev}"
ARCH="${ARCH:-$(uname -m)}"
[ "$ARCH" = "arm64" ] && ARCH=aarch64

# Frontends off: builds only xlog_core + xlog2-syncd (no Qt/GTK needed). The core
# still needs its dev headers to compile, but the linker pulls only the objects
# syncd references, so the binary depends on just sqlite3/curl/sodium.
cmake -S . -B build-syncd -DCMAKE_BUILD_TYPE=Release \
    -DXLOG_BUILD_GTK=OFF -DXLOG_BUILD_QT=OFF
cmake --build build-syncd --target xlog2-syncd -j"$(nproc)"

stage="xlog2-syncd-${VER}-linux-${ARCH}"
rm -rf "$stage"; mkdir -p "$stage"
cp build-syncd/xlog2-syncd "$stage/"
cp packaging/xlog2-syncd.service "$stage/"
cp packaging/xlog2-syncd-install.sh "$stage/install.sh"
chmod +x "$stage/install.sh"
cp docs/logbook-sync.md "$stage/" 2>/dev/null || true

cat > "$stage/README.txt" <<'EOF'
xlog2-syncd — headless logbook sync / backup peer for xlog2.

Great for a spare/always-on box (e.g. a Raspberry Pi): it keeps a full replica
of your synced logbook and serves the distributed QRZ cache.

Install (per-user, no root needed for the service itself):

    ./install.sh

Then set the sync secret to match your other nodes and restart:

    edit ~/.config/xlog2/layout.ini   ([sync] secret = ...)
    systemctl --user restart xlog2-syncd

Logs:   journalctl --user -u xlog2-syncd -f

Runtime deps: libsqlite3, libcurl, libsodium (present on any modern distro).
EOF

tar czf "${stage}.tar.gz" "$stage"
echo "== syncd deps =="; ldd "$stage/xlog2-syncd" | grep -iE 'sqlite|curl|sodium' || true
ls -lh "${stage}.tar.gz"
