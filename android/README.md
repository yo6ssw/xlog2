# xlog2-android

A mobile field-logging client that joins the same peer-to-peer **sync mesh** as
the xlog2 desktop app. A QSO logged on the phone gossips to the desktop (and
`xlog2-syncd`) and vice-versa, over the identical bespoke-crypto mesh — because
the app is a **third frontend** to xlog2's C++ core, not a reimplementation.

## How it works

```
Kotlin + Jetpack Compose UI  (fast field-logging)
      │ StateFlow / repository
SyncService (foreground, holds MulticastLock)
      │ JNI  (XlogCore.kt  ⇄  xlog_jni.cpp)
libxlog2jni.so
      │
xlog_mobile  =  the sync subset of ../src/core, cross-compiled
      │                    │
   multimaster (mesh)   libsodium + sqlite (vendored per ABI)
```

`xlog_jni.cpp` wires the core exactly like `../src/tools/SyncDaemon.cpp`: a
single core thread drains an `IUiDispatcher` queue (so sync callbacks never run
on the mesh IO thread), a JNI-backed `ILogPageView` signals the UI to re-query.
See `../docs/logbook-sync.md` for the protocol and the repo `CLAUDE.md` for the
core architecture.

## Build

Prerequisites (installed into `~/Android/Sdk`):

```sh
sdkmanager "ndk;26.3.11579264" "cmake;3.22.1" \
           "platforms;android-35" "build-tools;35.0.0" "platform-tools"
```

Vendor the native deps (libsodium + sqlite; libcurl is **not** needed — the
qrz.com tier is done in Kotlin/OkHttp, so the core builds with
`-DXLOG_MOBILE_QRZ=OFF`):

```sh
ANDROID_NDK_HOME=~/Android/Sdk/ndk/26.3.11579264 ./third_party/build-deps.sh --no-qrz
```

Then:

```sh
./gradlew :app:assembleDebug          # -> app/build/outputs/apk/debug/app-debug.apk
./gradlew :app:installDebug           # to a connected device/emulator
```

`local.properties` must point `sdk.dir` at the SDK (already set on the dev box).

### Host sanity build (no device)

The carve-out doubles as an off-device target so the sync core is validated
before cross-compiling:

```sh
cmake -S native -B ../build-android-host && cmake --build ../build-android-host -j
../build-android-host/xlog_mobile_smoke nodeA AA1AA 10   # log + run the mesh 10 s
# run a second instance with the same secret to watch a QSO sync
```

## v1 features

- **Log**: fast entry form (big Call field, live DXCC/dupe/band-from-freq),
  recent-QSO list, tap to edit/delete.
- **Sync**: foreground service + MulticastLock for LAN auto-discovery; status
  screen with this node's id + identity key; Sync-now.
- **Trusted peers**: admit/revoke peers (an identity mesh won't sync the phone
  until a peer trusts it; paste the identity key into the desktop's
  Sync ▸ Trusted peers, or trust the desktop here).
- **ADIF**: import/export via the Storage Access Framework (reuses `Adif.cpp`).
- **Settings**: station call/locator, sync secret/peers/node-name, QRZ creds.

### DXCC data

The AD1C `cty.dat` is bundled at `app/src/main/assets/cty.dat`; on first run it
is copied to the app's files dir and loaded, so the DXCC chip (country · CQ ·
ITU · continent) resolves offline. Replace that asset to update the prefix
table. If it's ever absent the feature degrades gracefully (blank chip).

## Notes / known gaps

- **QRZ qrz.com** lookups are not yet wired in Kotlin (OkHttp); the native
  mesh-peer QRZ tier is a later iteration that needs the `XLOG_MOBILE_QRZ=ON`
  core build (libcurl per ABI — see `third_party/README.md`).
- The **emulator can't bridge LAN multicast** to the host network; test
  auto-discovery on a real device. For the emulator, point the phone at the
  desktop via a **static peer** (Settings ▸ Static WAN peers =
  `<desktop-ip>:7388`).
- Doze may pause TCP in deep sleep; static peers + multimaster's reconnect
  cover wake-up resync.
