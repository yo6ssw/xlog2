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

### Release APK / publishing

`.github/workflows/android.yml` builds a **universal signed APK** (all vendored
ABIs in one file) on every `v*` tag and attaches it to the GitHub release, next
to the desktop AppImage and syncd tarballs. Users install it by sideloading the
`xlog2-android-<ver>.apk` asset.

Signing keys live in **repo secrets**, never in git. One-time setup:

```sh
keytool -genkeypair -v -keystore xlog2-release.jks -alias xlog2 \
        -keyalg RSA -keysize 4096 -validity 10000       # keep this file SAFE + backed up
base64 -w0 xlog2-release.jks                            # -> ANDROID_KEYSTORE_BASE64
```

Add four secrets to the repo (Settings ▸ Secrets ▸ Actions):

| secret | value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64`   | base64 of `xlog2-release.jks` |
| `ANDROID_KEYSTORE_PASSWORD` | the store password |
| `ANDROID_KEY_ALIAS`         | `xlog2` |
| `ANDROID_KEY_PASSWORD`      | the key password |

Losing the keystore means you can never ship an *update* to an already-installed
app (Android rejects a differently-signed APK for the same `applicationId`), so
back it up. Locally, `./gradlew :app:assembleRelease` with no keystore in the
environment just produces an **unsigned** APK — handy for smoke tests.

A signed APK is the easy path; the natural next step for discoverability is
**F-Droid** (this app is fully FOSS — libsodium/sqlite/OkHttp, no Google libs),
which builds from source using the same Gradle recipe.

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

## USB HID paddle (`UsbPaddle.kt`)

The phone-side reader for the vendor-HID Morse paddle (USB VID `0x1EAF`), the
analogue of the desktop's `/dev/hidraw` `HidPaddleInput`. It reads the interrupt
report over the USB Host API and drives the native keyer (`onDit`/`onDah` →
`PaddleKeyer.setDit`/`setDah`); report byte 0 is bit0 = dit, bit1 = dah. Two
Android-specific gotchas, both fixed and commented in the file — mind them if you
touch this code:

- **Composite device — pick the HID interface, not the first interrupt-IN
  endpoint.** The paddle enumerates as CDC-ACM serial **+** HID; the CDC comms
  interface (class 2) also exposes an interrupt-IN endpoint (its modem-status
  notification channel) that never carries paddle reports. Match
  `interfaceClass == USB_CLASS_HID` (class 3).
- **Read via `UsbRequest`, and catch `TimeoutException`.** `bulkTransfer()` is
  unreliable on interrupt endpoints (usbfs builds a bulk pipe). Use the async
  `UsbRequest` (`initialize`/`queue`/`requestWait`). Critically,
  `requestWait(timeout)` **throws `TimeoutException`** on an idle timeout (it does
  not return null) — swallow it and keep the request queued, or the read loop
  tears down every 200 ms, flapping the connected state and dropping a held
  contact.

Debugging tip: the phone has a single USB-C port, so the paddle and USB adb can't
be attached at once. logcat's ring buffer survives the disconnect — clear it,
run the paddle session unplugged, then replug USB and `adb logcat -d`. The keyer
itself connects to cwsd over the **network**, so keyer/UI-only bugs reproduce
over adb with no paddle.

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
