#!/bin/sh
#
# Build a Qt-only xlog2 AppImage (no GTK, so the GTK 4.10 floor never applies).
#
# IMPORTANT: run this on the OLDEST glibc you want to support — an AppImage runs
# only where glibc >= the build host's. Build on Ubuntu 22.04 (glibc 2.35) so the
# result runs on Raspberry Pi OS bookworm (2.36) and everything newer.
#
#   VER=0.6.0 ARCH=x86_64 packaging/build-appimage.sh     # or ARCH=aarch64
#
# Produces xlog2-qt-<VER>-<ARCH>.AppImage in the current directory.
#
set -eux

: "${VER:=dev}"
ARCH="${ARCH:-$(uname -m)}"
[ "$ARCH" = "arm64" ] && ARCH=aarch64      # linuxdeploy/AppImage use aarch64

export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE in CI/containers
export QMAKE=qmake6
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so   # so the image is headless-testable

work="$PWD/appimage-work"
tools="$work/tools"
mkdir -p "$tools"
base="https://github.com/linuxdeploy"
LD="$tools/linuxdeploy-$ARCH.AppImage"
LDQT="$tools/linuxdeploy-plugin-qt-$ARCH.AppImage"
[ -f "$LD" ]   || curl -fL "$base/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" -o "$LD"
[ -f "$LDQT" ] || curl -fL "$base/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" -o "$LDQT"
chmod +x "$LD" "$LDQT"
export PATH="$tools:$PATH"

# Qt frontend only.
cmake -S . -B build-appimage -DCMAKE_BUILD_TYPE=Release \
    -DXLOG_BUILD_GTK=OFF -DXLOG_BUILD_QT=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-appimage -j"$(nproc)"

APPDIR="$work/AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install build-appimage

# Keep only the Qt logger in the image (drop the syncd/paddle tools that the
# install also drops in), but keep the shared coastline data.
rm -f "$APPDIR/usr/bin/xlog2-syncd" "$APPDIR/usr/bin/xlog2-paddle"
rm -f "$APPDIR/usr/share/applications/xlog2-paddle.desktop"
rm -f "$APPDIR/usr/share/icons/hicolor/scalable/apps/xlog2-paddle.svg"
rm -rf "$APPDIR/usr/lib/systemd" "$APPDIR/usr/lib/udev"

OUTPUT="xlog2-qt-$VER-$ARCH.AppImage" \
    "$LD" --appdir "$APPDIR" \
        -e "$APPDIR/usr/bin/xlog2-qt" \
        -d "$APPDIR/usr/share/applications/xlog2-qt.desktop" \
        -i "$APPDIR/usr/share/icons/hicolor/scalable/apps/xlog2-qt.svg" \
        --plugin qt --output appimage

ls -lh xlog2-qt-*.AppImage
