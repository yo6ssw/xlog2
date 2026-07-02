#!/bin/sh
#
# Build a FULLY-STATIC (musl) xlog2-syncd — a single dependency-free binary that
# runs on any Linux (glibc or musl, incl. Raspberry Pi OS). Run inside Alpine:
#
#   docker run -i --rm -e VER=0.6.0 -e ARCH=x86_64 \
#       -v "$PWD":/src -w /src alpine:3.20 sh packaging/build-syncd-musl.sh
#
# The whole core compiles on musl; the final binary is linked by hand over the
# archives so only the objects syncd references are pulled in — so it needs just
# static sqlite/curl/sodium, NOT the (statically-unavailable) pipewire/dbus that
# xlog_core links for the GUI's audio/rig paths.
#
set -eux

: "${VER:=dev}"
ARCH="${ARCH:-$(uname -m)}"
[ "$ARCH" = "arm64" ] && ARCH=aarch64

apk add --no-cache build-base cmake pkgconf git linux-headers \
    hamlib-dev pipewire-dev dbus-dev alsa-lib-dev opus-dev \
    sqlite-dev curl-dev libsodium-dev >/dev/null
# Static libs for the final link (curl pulls a long transitive chain). Best-effort:
# a missing one only errors if the static link actually references it.
for p in sqlite-static curl-static libsodium-static openssl-libs-static zlib-static \
         brotli-static zstd-static nghttp2-static c-ares-static libpsl-static \
         libidn2-static libunistring-static; do
    apk add --no-cache "$p" >/dev/null 2>&1 || echo "note: no static package $p"
done

git config --global --add safe.directory "$PWD" 2>/dev/null || true

cmake -S . -B build-syncd-musl -DCMAKE_BUILD_TYPE=Release \
    -DXLOG_BUILD_GTK=OFF -DXLOG_BUILD_QT=OFF
cmake --build build-syncd-musl --target xlog2-syncd -j"$(nproc)"

# Fully-static final link: --start-group resolves the two archives; pkg-config
# --static adds curl's transitive static deps. Only syncd's referenced objects
# are pulled, so hamlib/opus/pipewire/dbus are never needed.
obj=build-syncd-musl/CMakeFiles/xlog2-syncd.dir/src/tools/SyncDaemon.cpp.o
g++ -static -o xlog2-syncd "$obj" \
    -Wl,--start-group \
        build-syncd-musl/libxlog_core.a \
        build-syncd-musl/third_party/multimaster/libmultimaster.a \
    -Wl,--end-group \
    $(pkg-config --static --libs sqlite3 libcurl libsodium) -pthread
strip xlog2-syncd

file xlog2-syncd
file xlog2-syncd | grep -q 'statically linked' || { echo "ERROR: not static" >&2; exit 1; }
./xlog2-syncd --help >/dev/null 2>&1 || { echo "ERROR: --help failed" >&2; exit 1; }

stage="xlog2-syncd-${VER}-linux-${ARCH}-static"
rm -rf "$stage"; mkdir -p "$stage"
cp xlog2-syncd "$stage/"
cp packaging/xlog2-syncd.service "$stage/"
cp packaging/xlog2-syncd-install.sh "$stage/install.sh"
chmod +x "$stage/install.sh"
cp docs/logbook-sync.md "$stage/" 2>/dev/null || true
cat > "$stage/README.txt" <<'EOF'
xlog2-syncd — headless logbook sync / backup peer (fully-static musl build).

A single dependency-free binary: runs on any Linux, including Raspberry Pi OS.
Install as a per-user service:  ./install.sh
Then set [sync] secret in ~/.config/xlog2/layout.ini and:
  systemctl --user restart xlog2-syncd
Logs:  journalctl --user -u xlog2-syncd -f
EOF
tar czf "${stage}.tar.gz" "$stage"
ls -lh "${stage}.tar.gz"
