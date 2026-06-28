# Publishing xlog2-android on Google Play

## Context

`android/` is a Jetpack-Compose client (`applicationId = ro.scripca.xlog2`,
versionName `0.1.0`, code `1`) that reuses the C++ sync core over JNI. The goal
is to publish it on Google Play as a **public production app**, treating the
closed-test gate as an explicit milestone. (As of writing, no developer account
exists yet.)

The codebase is already in good technical shape for Play — the hard parts are
done: native libs force **16 KB page alignment**
(`android/app/src/main/cpp/CMakeLists.txt` uses `-Wl,-z,max-page-size=16384` /
`common-page-size=16384`), `targetSdk = 35` / `compileSdk = 35` meet the current
Play bar, the foreground service is declared (`dataSync` + low-importance
notification + `MulticastLock` in `SyncService.kt`), and a full adaptive launcher
icon set exists. AGP 8.5.2 / Gradle 8.7 / Kotlin 2.0.20 build cleanly.

What's missing falls into four buckets: **release signing**, a handful of
**repo artifacts** (privacy policy, version hygiene, optional polish), the
**Play Console account + listing/policy forms**, and a **release-track sequence**
(internal → closed test → production). This document covers all four.

---

## A. Developer account (do first — has lead time)

- **Cost:** $25 one-time registration.
- **Recommendation: Personal account.** Org accounts need a free **D-U-N-S
  number** (1–2 week lead time) and are exempt from the new-account tester gate,
  but add paperwork and an organizational identity that may not be wanted. Since
  closed testing is accepted as a step, Personal is the simpler fit.
- **Personal-account requirements to budget for:**
  - **Identity verification** (government ID, address) at signup.
  - **Closed-testing gate:** accounts created after 2023-11-13 must run a closed
    test with **≥20 testers opted-in for ≥14 continuous days** before they can
    apply for production access. This is the long pole — line up ~20 ham contacts
    early. (Internal testing does **not** count toward this; it must be a
    *closed* track.)
- Enroll in **Play App Signing** (default for new apps): Google holds the app
  signing key; you upload with an **upload key** you generate (see §B1).

---

## B. Repository changes

### B1. Release signing config — **critical blocker**
No `signingConfigs` exists today; an unsigned release can't be uploaded.

- Generate an **upload keystore** (kept OUT of git):
  `keytool -genkeypair -v -keystore upload-keystore.jks -alias xlog2 -keyalg RSA -keysize 4096 -validity 10000`
- Add a gitignored **`android/keystore.properties`** (path, storePassword,
  keyAlias, keyPassword) and load it in `android/app/build.gradle.kts`:
  - a `signingConfigs { create("release") { ... } }` block reading the
    properties file (guarded so debug builds without it still work / CI can use
    env vars), wired into `buildTypes.release.signingConfig`.
- Add `android/keystore.properties` and `*.jks`/`*.keystore` to
  **`android/.gitignore`** (verify the file exists; create if not). Back the
  keystore up securely — losing the upload key requires a Play reset.

### B2. App Bundle output (already works; document + verify)
- Play requires an **`.aab`**, not an APK. `./gradlew :app:bundleRelease`
  produces `app/build/outputs/bundle/release/app-release.aab`. No code change
  needed beyond signing; add a short note to `android/README` (or the repo
  `README`) with the build/upload command.
- **Keep `isMinifyEnabled = false`.** R8 over a thin Kotlin layer buys little and
  risks stripping JNI entry points; the existing `proguard-rules.pro` already
  keeps `XlogCore` + native methods, but minify-off is the safe default for v1.

### B3. Version hygiene
- `versionCode` must **strictly increase** with every upload. Decide whether to
  ship as `0.1.0` or bump the marketing `versionName` (e.g. `1.0.0`) for a public
  debut. Default: keep `0.1.0` and only bump code.
- Optional: a tiny `packaging/android-release.sh` mirroring the desktop
  `release.sh` idea — bump `versionCode`/`versionName` in `build.gradle.kts`,
  build `bundleRelease`. Low priority; the gradle `BuildConfig` footer already
  surfaces the version in-app (Settings).

### B4. Privacy policy (required by Play, hosted at a public URL)
- Add **`android/PRIVACY.md`** (and host it — GitHub Pages / raw gist / project
  site). Content is genuinely simple: the developer operates **no servers and
  collects no data**; QSO records and the QRZ cache sync **peer-to-peer** among
  the user's own devices, encrypted (libsodium X25519 + ChaCha20-Poly1305) and
  gated by a user-set shared secret; credentials/secret stay **on-device** in
  app-private storage. This URL is pasted into the Console store listing AND the
  Data Safety form.

### B5. Optional polish (not blockers)
- **Themed (monochrome) icon:** add `<monochrome>` to
  `mipmap-anydpi-v26/ic_launcher.xml` + a monochrome drawable for Android 13+
  themed icons. Nice-to-have.
- **Credential at-rest hardening:** `Settings.kt` stores `qrzPassword` and
  `syncSecret` in plaintext `SharedPreferences`. QRZ creds are currently *unused*
  (`XLOG_MOBILE_QRZ=OFF`) — consider **removing the unused QRZ fields** to shrink
  the Data Safety surface, or move the secret to `EncryptedSharedPreferences`.
  Low priority; declare honestly either way.

---

## C. Play Console listing + policy forms

Store listing assets to produce:
- **App icon** 512×512 PNG (hi-res, from the existing foreground art).
- **Feature graphic** 1024×500 PNG.
- **Phone screenshots** ≥2 (up to 8) — capture the redesigned Log list, Entry
  form, Sync, Peers. Optional 7"/10" tablet shots.
- **Title** ≤30 chars (e.g. "XLOG2 — Ham Radio Log"), **short description**
  ≤80, **full description** ≤4000.

Console forms to complete:
- **Data safety** — declare no developer collection; on-device + P2P only.
  Reference the privacy URL.
- **Content rating** (IARC questionnaire) — utility app, expect "Everyone".
- **Target audience & content** — adults/general, **not** directed at children.
- **Ads** — declare none.
- **App access** — note the app needs no login; sync uses a user-supplied secret,
  no account/credentials to provide a reviewer.
- **Foreground service declaration** — Play requires justifying the `dataSync` FGS
  type. Justification: *continuous peer-to-peer logbook sync over the LAN mesh
  must keep running while the app is backgrounded so contacts logged on other
  devices arrive promptly; multicast discovery needs the held MulticastLock.* Be
  aware Android 15 imposes a **6-hour/day `dataSync` runtime cap** — acceptable
  for this use, but note it; if Google pushes back, the fallback is to make
  background sync opt-in (it already is, via the Sync toggle) and emphasize that
  in the declaration.

---

## D. Release-track sequence (the milestone path)

1. **Internal testing** — upload the signed AAB, smoke-test on real devices via
   the opt-in link (instant, up to 100 testers). Fix the **pre-launch report**
   findings (Console runs your AAB on real devices automatically).
2. **Closed testing** — promote to a closed track, recruit **≥20 testers**, keep
   them opted-in **≥14 days** (the personal-account production gate). Gather
   feedback, iterate with new `versionCode`s.
3. **Apply for production access** (personal accounts) once the gate is met.
4. **Production** — complete all §C forms, roll out (consider a **staged
   rollout** %).

---

## Critical files

- `android/app/build.gradle.kts` — add `signingConfigs`/`buildTypes.release`
  signing (reads `keystore.properties`); optional versionCode bump.
- `android/keystore.properties` *(new, gitignored)* + `android/.gitignore` —
  keystore secrets, never committed.
- `upload-keystore.jks` *(new, stored securely outside git)*.
- `android/PRIVACY.md` *(new)* + a public hosting location.
- *(optional)* `android/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml` +
  monochrome drawable; `Settings.kt` credential cleanup;
  `packaging/android-release.sh`.

## Verification

- `cd android && ./gradlew :app:bundleRelease` → confirm a **signed**
  `app-release.aab` is produced (check `apksigner verify` on an APK extracted via
  `bundletool build-apks`).
- Confirm 16 KB alignment survives release build:
  `unzip -p app-release.aab base/lib/arm64-v8a/libxlog2jni.so > /tmp/x.so && llvm-readelf -l /tmp/x.so | grep LOAD` → `Align` must be `0x4000`.
- Install on a physical device from the **Internal testing** opt-in link; verify
  logging, ADIF import/export, and mesh sync against a desktop peer.
- Review and clear the Console **pre-launch report** before promoting tracks.
