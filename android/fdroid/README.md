# Publishing xlog2 to F-Droid

F-Droid builds the app from source on its own build servers and hosts the result
in the main repository — the natural, no-cost distribution channel for a fully
free-software ham app. Everything here is already prepared; what remains needs
*your* GitLab account.

## What is already done (in this repo)

- **Build recipe:** [`ro.scripca.xlog2.yml`](ro.scripca.xlog2.yml) — the exact
  file F-Droid needs, kept in-tree so it tracks the source. It carries the
  listing **Summary/Description inline**, so the text renders on the very first
  build regardless of which commit is built.
- **Store listing metadata (Fastlane layout):**
  `android/app/fastlane/metadata/android/en-US/` — title, short/full description,
  and a per-versionCode changelog (placed under the build `subdir` so F-Droid
  discovers it). F-Droid imports these from the *built tag's* checkout; they
  mirror the inline text and take over (plus screenshots) once a built tag
  contains them. Add screenshots by dropping PNGs in
  `.../en-US/images/phoneScreenshots/` (`1.png`, `2.png`, …).
- The app icon is pulled from the APK automatically.
- **License / freedom:** GPL-3.0-or-later, `SPDX` headers throughout; deps are
  AndroidX/Compose/OkHttp (all FOSS) + native libsodium (ISC) and sqlite (public
  domain). No Google Play Services, no proprietary libraries.

## Submit it (one-time, from your GitLab account)

1. Sign in at https://gitlab.com and **fork** https://gitlab.com/fdroid/fdroiddata
2. In your fork, add the recipe at the path F-Droid expects:
   ```sh
   git clone https://gitlab.com/<you>/fdroiddata
   cp /path/to/xlog2/android/fdroid/ro.scripca.xlog2.yml \
      fdroiddata/metadata/ro.scripca.xlog2.yml
   ```
3. (Recommended) Dry-run the build locally with F-Droid's tooling before opening
   the MR — this is exactly what their CI runs:
   ```sh
   pip install fdroidserver          # or: apt install fdroidserver
   cd fdroiddata
   fdroid readmeta
   fdroid lint ro.scripca.xlog2
   fdroid build -v -l ro.scripca.xlog2   # needs Android SDK + NDK r26d locally
   ```
4. Commit and push to your fork, then open a **Merge Request** against
   `fdroid/fdroiddata`. F-Droid maintainers review, their buildserver builds it,
   and once merged the app appears in the F-Droid client (usually within a day of
   the next index build).

Alternatively, the lowest-effort path is to file a **Request For Packaging (RFP)**
issue at https://gitlab.com/fdroid/rfp/-/issues and attach this recipe — a
maintainer then does the MR. Submitting the MR yourself is faster.

## Two things a reviewer may raise

1. **Native deps are fetched at build time.** `third_party/build-deps.sh` downloads
   the libsodium and sqlite *source* from their official sites and compiles them in
   `prebuild`. F-Droid build servers have network access, so this works, but
   reviewers prefer pinned/offline sources. If asked, the fix is to vendor the
   sqlite amalgamation and libsodium tarball into the repo (or pin their SHA-256 in
   the script). Ping me and I'll wire that up.
2. **App version vs. git tags.** The app's `versionCode`/`versionName` (2 / 0.2.0)
   is independent of the shared ecosystem tags (`v0.6.x`). The recipe uses
   `UpdateCheckMode: Tags`, which reads the versionCode from `build.gradle.kts` at
   the newest tag. **To ship a new F-Droid build, bump `versionCode` in
   `android/app/build.gradle.kts`** on that release and add a matching `Builds:`
   entry to the recipe. A release that does not touch the Android app should leave
   the versionCode unchanged so F-Droid does not republish an identical APK.

## Signing note

F-Droid signs the APK with **its own key**, not the release keystore in
`~/Dropbox/keys/xlog2/`. That keystore signs the APK attached to GitHub releases.
The two signatures differ, so a user cannot switch between the GitHub APK and the
F-Droid build by updating in place — they must pick one source and stay on it.
That is normal and expected for dual-channel distribution.
