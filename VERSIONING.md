# Versioning

xlog2 uses [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

While in the **0.x** series (pre-1.0) the project is still stabilising:
- **MINOR** (`0.X.0`) — new features, and changes that may alter behaviour or
  on-disk/settings formats.
- **PATCH** (`0.x.Y`) — bug fixes and small tweaks, no intended behaviour change.

After `1.0.0`, normal SemVer applies: MAJOR for breaking changes, MINOR for
backwards-compatible features, PATCH for fixes.

## Single source of truth

The version lives in **one** place: the `project()` call in `CMakeLists.txt`:

```cmake
project(xlog2 VERSION 0.1.0 ...)
```

Everything else derives from it — the PPA publish script reads it with `grep`,
and `debian/changelog` is kept in step. We deliberately do **not** derive the
version from `git describe`: the PPA flow publishes via `git archive HEAD`,
which strips `.git`, so a git-describe version would be unavailable there.

## Releasing

A release is: bump the version, commit, and tag.

```sh
packaging/release.sh 0.2.0      # bumps CMakeLists + debian/changelog, commits,
                                # and creates annotated tag v0.2.0
git push origin master --follow-tags
```

`release.sh` keeps `CMakeLists.txt`, `debian/changelog` **and the Android app
version** (`android/app/build.gradle.kts`) in sync, GPG-signs the commit and tag,
and creates the annotated tag `vX.Y.Z`, so the tag, the build version, the
package version and the Android version can never drift apart. Do the work and
commit it first; run `release.sh` when you're ready to cut the release.

### Android version / F-Droid autoupdate

The Android app must move in lockstep with the tag: its `versionName` equals the
release version and its `versionCode` is `major*1000000 + minor*10000 +
patch*100` (e.g. `0.6.7` → `60700`). F-Droid autoupdate (`UpdateCheckMode: Tags`
+ `AutoUpdateMode: Version` in the fdroiddata recipe) maps a new tag back to its
`versionName`, so a release whose Android `versionName` didn't match the tag
would never publish on F-Droid. `release.sh` sets both, so this stays correct as
long as releases go through it.

## Git tags

Each release is marked with an **annotated** tag `vMAJOR.MINOR.PATCH`
(e.g. `v0.1.0`). Tags are the canonical record of what was released; push them
with `--follow-tags`.

## How a version reaches the PPA

```
tag v0.2.0  →  CMakeLists VERSION 0.2.0  →  upload 0.2.0~<series><rev>
```

`packaging/publish-ppa.sh` appends a per-series suffix (`~noble1`, `~jammy1`, …)
because Launchpad needs a distinct version per Ubuntu series. The trailing
number is the `--rev`: bump it (`--rev 2`) to re-upload the *same* upstream
version to the *same* series (e.g. after fixing a packaging-only problem).
A new upstream version resets `--rev` to 1.
