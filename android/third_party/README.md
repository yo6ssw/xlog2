# Vendored native deps for the Android sync core

The Android build of `xlog_mobile` (the carved-out C++ sync core) needs three
native libraries that have no pkg-config on Android. `build-deps.sh` produces
them per ABI under this directory:

```
android/third_party/
  <abi>/lib/libsodium.a        # mesh transport crypto (required)
  <abi>/lib/libcurl.a + TLS    # qrz.com lookups (only when XLOG_MOBILE_QRZ=ON)
  <abi>/include/...
  sqlite/sqlite3.{c,h}         # storage engine (amalgamation, compiled in)
```

where `<abi>` ∈ `arm64-v8a`, `armeabi-v7a`, `x86_64`.

## Requirements

- **Android NDK** (r26+). Set `ANDROID_NDK_HOME` or pass `--ndk`.
- The SDK's CMake (3.22+) and a recent **Gradle wrapper** (8.7+). The system
  Gradle is irrelevant — the project ships `./gradlew`.

Install via Android Studio (SDK Manager → "NDK (Side by side)" + "CMake"), or
headless:

```sh
sdkmanager "ndk;26.3.11579264" "cmake;3.22.1" "platforms;android-35" "build-tools;35.0.0"
```

## Building the deps

```sh
ANDROID_NDK_HOME=~/Android/Sdk/ndk/26.3.11579264 ./build-deps.sh
# minimal (no qrz.com / no TLS):
ANDROID_NDK_HOME=... ./build-deps.sh --no-qrz
```

- **libsodium** builds cleanly with its bundled `dist-build/android-*.sh`.
- **sqlite** is just the amalgamation; it is fetched once and compiled into the
  lib by CMake.
- **libcurl** is the heavy one — it needs a TLS backend. The robust recipe:
  1. Build **BoringSSL** for each ABI against the NDK clang.
  2. `./configure --host=<triple> --with-ssl=<boringssl-prefix>
     --disable-shared --enable-static` with `CC`/`AR`/`RANLIB` from
     `$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin`, `--with-pic`,
     and a minimal protocol set (`--disable-ldap --without-libpsl …`).
  3. Drop `libcurl.a` (+ `libssl.a`/`libcrypto.a`) into `<abi>/lib/` and headers
     into `<abi>/include/`.

  CMake links every `lib*.a` it finds in `<abi>/lib/`, so the TLS static libs are
  picked up automatically.

If the curl/TLS cross-build is more trouble than it's worth, build the app with
`-DXLOG_MOBILE_QRZ=OFF` (the `app/build.gradle.kts` CMake argument). The core
then drops `Qrz.cpp`/`QrzPeer.cpp`/`QrzCache.cpp` and the bridge compiles the
`XLOG_NO_QRZ` path; the qrz.com tier can be reimplemented in Kotlin (OkHttp)
while the mesh + logbook keep working.

`.build/` (downloads + intermediate trees) and the generated `<abi>/` and
`sqlite/` outputs are git-ignored.
