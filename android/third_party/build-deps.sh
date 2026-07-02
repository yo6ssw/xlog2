#!/usr/bin/env bash
# Build the native third-party deps the Android sync core needs, per ABI:
#
#   * libsodium  — the mesh transport crypto (required)
#   * libcurl    — qrz.com lookups (only when QRZ is enabled; pulls a TLS lib)
#   * sqlite3    — the storage engine (fetched as the amalgamation, compiled in)
#
# Outputs:
#   android/third_party/<ABI>/lib/{libsodium.a,libcurl.a,...}
#   android/third_party/<ABI>/include/...
#   android/third_party/sqlite/{sqlite3.c,sqlite3.h}
#
# Requires the Android NDK (set ANDROID_NDK_HOME or pass --ndk). libsodium ships
# an Android build script; libcurl is configured against the NDK toolchain with
# a vendored TLS backend. This script is best-effort and idempotent — re-run it
# after `git submodule update` or an NDK bump.
#
# Usage:
#   ANDROID_NDK_HOME=~/Android/Sdk/ndk/<ver> ./build-deps.sh [--abis "arm64-v8a x86_64"] [--no-qrz]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ABIS="arm64-v8a armeabi-v7a x86_64"
WANT_QRZ=1
API=26
SODIUM_VER="1.0.20"
SQLITE_YEAR="2024"
SQLITE_AMALG="sqlite-amalgamation-3460100"

while [ $# -gt 0 ]; do
    case "$1" in
        --ndk)   ANDROID_NDK_HOME="$2"; shift 2;;
        --abis)  ABIS="$2"; shift 2;;
        --no-qrz) WANT_QRZ=0; shift;;
        --api)   API="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME (or pass --ndk /path/to/ndk)}"
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
[ -d "$TOOLCHAIN" ] || { echo "NDK toolchain not found at $TOOLCHAIN" >&2; exit 1; }

work="$HERE/.build"
mkdir -p "$work"

# Disable automake dependency tracking for every ./configure we run below
# (libsodium's android-*.sh, and libcurl). We do clean one-shot cross-builds and
# never need incremental .deps, and some hosts (e.g. Debian trixie, as used by
# the F-Droid buildserver) otherwise fail configure with "Something went wrong
# bootstrapping makefile fragments for automatic dependency tracking". A
# config.site is honoured by configure regardless of how it's invoked, so we
# don't have to patch the vendored build scripts.
cat > "$work/config.site" <<'EOF'
enable_dependency_tracking=no
EOF
export CONFIG_SITE="$work/config.site"

# --- sqlite amalgamation (arch-independent C; compiled into xlog_mobile) ----
if [ ! -f "$HERE/sqlite/sqlite3.c" ]; then
    echo ">> fetching sqlite amalgamation"
    mkdir -p "$HERE/sqlite"
    ( cd "$work"
      curl -fsSLO "https://www.sqlite.org/$SQLITE_YEAR/$SQLITE_AMALG.zip"
      unzip -oq "$SQLITE_AMALG.zip" )
    cp "$work/$SQLITE_AMALG/sqlite3.c" "$work/$SQLITE_AMALG/sqlite3.h" "$HERE/sqlite/"
fi

# --- libsodium (its own Android build script emits per-ABI static libs) ------
if [ ! -d "$work/libsodium" ]; then
    echo ">> fetching libsodium $SODIUM_VER"
    ( cd "$work"
      curl -fsSLO "https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM_VER-stable.tar.gz" \
        || curl -fsSLO "https://github.com/jedisct1/libsodium/releases/download/$SODIUM_VER-RELEASE/libsodium-$SODIUM_VER.tar.gz"
      tar xzf libsodium-*.tar.gz
      # The -stable tarball unpacks to libsodium-stable; a versioned one to
      # libsodium-<ver>. Normalise to "libsodium".
      d=$(find . -maxdepth 1 -type d -name 'libsodium-*' | head -1)
      mv "$d" libsodium )
fi
echo ">> building libsodium (dist-build/android-*.sh)"
( cd "$work/libsodium"
  export ANDROID_NDK_HOME
  export NDK_PLATFORM="android-$API"   # libsodium's android-*.sh requires this
  # libsodium's android-build.sh only sets CC, leaving configure to discover the
  # binutils. NDK r26 ships ONLY the llvm-prefixed tools (no ar/nm/ranlib and no
  # aarch64-linux-android-* wrappers), so configure otherwise falls back to the
  # host nm — which fails libtool's "parse nm output from a clang object" test on
  # some hosts (e.g. the F-Droid buildserver / Debian trixie). Point the archive
  # tools at the NDK's llvm-* explicitly so the build never touches host binutils.
  export AR="$TOOLCHAIN/bin/llvm-ar"
  export NM="$TOOLCHAIN/bin/llvm-nm"
  export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
  export STRIP="$TOOLCHAIN/bin/llvm-strip"
  for abi in $ABIS; do
      case "$abi" in
        arm64-v8a)   script=android-armv8-a.sh;;
        armeabi-v7a) script=android-armv7-a.sh;;
        x86_64)      script=android-x86_64.sh;;
        *) echo "unhandled abi $abi" >&2; exit 1;;
      esac
      # The per-ABI output dir name varies across libsodium versions
      # (e.g. libsodium-android-armv8-a / -westmere). Detect it: it's the
      # libsodium-android-* dir that gains a fresh build.
      before=$(ls -d libsodium-android-* 2>/dev/null || true)
      "./dist-build/$script"
      after=$(ls -d libsodium-android-* 2>/dev/null || true)
      # `grep .` exits 1 when nothing is new (a rebuild, dirs already present);
      # tolerate that under `set -o pipefail` and fall back to the newest dir below.
      src=$(comm -13 <(echo "$before" | tr ' ' '\n' | sort) \
                     <(echo "$after"  | tr ' ' '\n' | sort) | { grep . || true; } | head -1)
      [ -n "$src" ] || src=$(ls -dt libsodium-android-* | head -1)
      [ -f "$src/lib/libsodium.a" ] || { echo "libsodium build for $abi failed (no $src/lib/libsodium.a)" >&2; exit 1; }
      out="$HERE/$abi"; mkdir -p "$out/lib" "$out/include"
      cp "$src/lib/libsodium.a" "$out/lib/"
      cp -r "$src/include/." "$out/include/"
      echo "   $abi <- $src"
  done )

# --- libopus (rig-audio Opus decode on the client) --------------------------
# Cross-compiled with the NDK clang via opus's autotools. Decode-only use, but we
# build the full static lib (small) and let the linker drop unused encoder code.
OPUS_VER="1.5.2"
if [ ! -d "$work/libopus" ]; then
    echo ">> fetching libopus $OPUS_VER"
    ( cd "$work"
      curl -fsSLO "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
      tar xzf "opus-$OPUS_VER.tar.gz"
      mv "opus-$OPUS_VER" libopus )
fi
echo ">> building libopus"
for abi in $ABIS; do
    case "$abi" in
      arm64-v8a)   triple=aarch64-linux-android; host=aarch64-linux-android;;
      armeabi-v7a) triple=armv7a-linux-androideabi; host=arm-linux-androideabi;;
      x86_64)      triple=x86_64-linux-android; host=x86_64-linux-android;;
      *) echo "unhandled abi $abi" >&2; exit 1;;
    esac
    bdir="$work/libopus/build-$abi"
    rm -rf "$bdir"; mkdir -p "$bdir"
    ( cd "$bdir"
      export CC="$TOOLCHAIN/bin/${triple}${API}-clang"
      export AR="$TOOLCHAIN/bin/llvm-ar"
      export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
      export CFLAGS="-fPIC -O2"
      ../configure --host="$host" --prefix="$bdir/out" \
          --disable-shared --enable-static --disable-doc --disable-extra-programs >/dev/null
      make -j"$(nproc)" >/dev/null
      make install >/dev/null )
    [ -f "$bdir/out/lib/libopus.a" ] || { echo "libopus build for $abi failed" >&2; exit 1; }
    out="$HERE/$abi"; mkdir -p "$out/lib" "$out/include"
    cp "$bdir/out/lib/libopus.a" "$out/lib/"
    cp -r "$bdir/out/include/opus" "$out/include/"
    echo "   $abi opus <- $bdir"
done

# --- libcurl (optional; needs a TLS backend) --------------------------------
if [ "$WANT_QRZ" = "1" ]; then
    echo ">> NOTE: libcurl cross-build is the heaviest dep."
    echo "   Recommended: build BoringSSL for each ABI, then configure curl with"
    echo "   --with-ssl=<boringssl-prefix> --host=<triple> against the NDK clang."
    echo "   See README in this dir. If this proves painful, configure the app"
    echo "   with -DXLOG_MOBILE_QRZ=OFF and do qrz.com in Kotlin (OkHttp)."
    # Placeholder: a full curl+BoringSSL recipe is environment-specific and is
    # documented in android/third_party/README.md rather than hard-coded here.
fi

echo ">> done. Vendored deps under $HERE/<abi>/ and $HERE/sqlite/"
