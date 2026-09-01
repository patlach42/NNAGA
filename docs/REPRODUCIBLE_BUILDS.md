# Reproducible builds

This guide describes how to make NNAGA release builds comparable. It does **not** promise byte-for-byte reproducibility: the Android build embeds the current date, time, and host name in `BuildConfig`, and signed Android packages can also differ in signing metadata.

## 1. Pin the source tree and application version

Build from an immutable commit (preferably release tag) rather than a moving branch. Record commit, tag, and submodule state in build log.

```sh
git fetch --tags --prune
git checkout --detach v<version>
git submodule update --init --recursive
git rev-parse HEAD
git describe --tags --always --dirty
git submodule status --recursive
```

NNAGA's application identity is authoritative from root `version.properties`:

```sh
VERSION_NAME=$(awk -F= '$1=="VERSION_NAME"{print $2}' version.properties)
VERSION_CODE=$(awk -F= '$1=="VERSION_CODE"{print $2}' version.properties)
printf 'VERSION_NAME=%s\nVERSION_CODE=%s\n' "$VERSION_NAME" "$VERSION_CODE"
```

The dashboard's clean display suffix is derived from Git: count commits since
the latest commit that introduced the current exact `VERSION_NAME` line
(`git log -S"VERSION_NAME=<version>" -- version.properties`), then map the
count using spreadsheet-style lowercase base-26 (`0` is empty, `1=a`, ...,
`26=z`, `27=aa`). A changed `VERSION_NAME` starts a new baseline; after a
history rewrite or cherry-pick, recompute it from the resulting history.

Dirty status uses:

```sh
git status --porcelain=v1 --untracked-files=all --ignore-submodules=dirty
```

For non-empty status, the dashboard appends `-dirty-<letter>`. The root ignored
`dirty.version` stores the next local dirty-build index (missing means `1`).
It increments exactly once atomically after a successful local Gradle graph
containing an app assemble/bundle task, never during configuration, failed or
clean graphs, or GitHub Actions. These display suffixes never enter SemVer,
Android metadata, canonical artifact names, or tags.

Keep a release tag equal to `v${VERSION_NAME}`. Artifacts and tags are
SemVer-only. A dirty tree must not be used for reproducibility comparison.

`build.sh` also runs `git submodule update --init --recursive`. Run it after the checkout even when submodules appear present, and retain the `git submodule status --recursive` output as provenance. Submodule commits are part of source compared.

The repository pins the Gradle wrapper to Gradle 8.9 (`gradle/wrapper/gradle-wrapper.properties`), Android Gradle Plugin to 8.7.3, Kotlin to 1.9.20, Java source/target to 17, and CMake to 3.22.1. The CI release workflow additionally selects Android NDK `26.1.10909125` for the VST host and `27.2.12479018` for native builds. Record the actual tools used, not only the requested versions:

```sh
./gradlew --version
java -version
cmake --version
ninja --version
git --version
sha256sum --version | head -1
```

For an Android SDK/NDK installation, record the SDK root and installed package list (the command may vary with SDK layout):

```sh
printf 'ANDROID_SDK_ROOT=%s\n' "${ANDROID_SDK_ROOT:-}"
"${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" --list
```

Also record relevant environment values that can affect output, especially `HOSTNAME`, `JAVA_HOME`, `ANDROID_SDK_ROOT`, `ANDROID_NDK`, `BUILD_VST`, and `CI`. Do not include passwords, keystore contents, or other secrets in the log.

## 3. Provide private signing properties

Release signing is required by `app/build.gradle.kts`. The four Gradle properties are:

- `RELEASE_STORE_FILE` — path to the private keystore;
- `RELEASE_STORE_PASSWORD` — keystore password;
- `RELEASE_KEY_ALIAS` — signing key alias;
- `RELEASE_KEY_PASSWORD` — key password.

Obtain these values through the project's approved private secret-handling process. Never commit them or place them in this document. For a local build, pass them through a private Gradle properties file or an equivalent secret-aware mechanism. For example, with values already present in a local, access-controlled shell environment:

```sh
./gradlew assembleFullRelease \
  -PRELEASE_STORE_FILE="$RELEASE_STORE_FILE" \
  -PRELEASE_STORE_PASSWORD="$RELEASE_STORE_PASSWORD" \
  -PRELEASE_KEY_ALIAS="$RELEASE_KEY_ALIAS" \
  -PRELEASE_KEY_PASSWORD="$RELEASE_KEY_PASSWORD"
```

The CI workflow decodes its private `KEYSTORE_BASE64` secret to a temporary keystore and passes the corresponding `RELEASE_*` properties to Gradle; it removes that temporary file during cleanup. A reproducer should follow the same secret-handling rules, without copying secret values into command transcripts or artifacts.

## 4. Select one build variant

`build.sh` accepts a flavor argument (default `full`) and initializes submodules, applies patches, generates or restores FFTW codelets, and stages native/plugin assets. Use the same flavor and staging conditions for both builds being compared:

```sh
# Full/sideload APK, including the VST host stack when BUILD_VST=1.
BUILD_VST=1 ./build.sh full
./gradlew assembleFullRelease \
  -PRELEASE_STORE_FILE="$RELEASE_STORE_FILE" \
  -PRELEASE_STORE_PASSWORD="$RELEASE_STORE_PASSWORD" \
  -PRELEASE_KEY_ALIAS="$RELEASE_KEY_ALIAS" \
  -PRELEASE_KEY_PASSWORD="$RELEASE_KEY_PASSWORD"

# Play Store APK/AAB path; this flavor does not bundle the VST host.
./build.sh playstore
./gradlew bundlePlaystoreRelease \
  -PRELEASE_STORE_FILE="$RELEASE_STORE_FILE" \
  -PRELEASE_STORE_PASSWORD="$RELEASE_STORE_PASSWORD" \
  -PRELEASE_KEY_ALIAS="$RELEASE_KEY_ALIAS" \
  -PRELEASE_KEY_PASSWORD="$RELEASE_KEY_PASSWORD"
```

In CI, `CI` causes `build.sh` to default `BUILD_VST` to `0`; the full-release workflow stages the cached VST components before running the Gradle `assembleFullRelease` task. A local full build should therefore state whether it used `BUILD_VST=1` and whether the same staged inputs were available. Do not compare a full APK with a playstore APK, or a debug output with a release output.

## 5. Capture artifact hashes and provenance

After a build, use canonical versioned outputs and record SHA-256 with source/toolchain records:

```sh
VERSION_NAME=$(awk -F= '$1=="VERSION_NAME"{print $2; exit}' version.properties)

APK=app/build/outputs/versioned/apk/fullRelease/nnaga-${VERSION_NAME}-full-release.apk
test -n "$VERSION_NAME" && test -f "$APK"
printf '%s  %s\n' "$(sha256sum "$APK" | cut -d' ' -f1)" "$APK"

AAB=app/build/outputs/versioned/bundle/playstoreRelease/nnaga-${VERSION_NAME}-playstore-release.aab
test -n "$VERSION_NAME" && test -f "$AAB"
printf '%s  %s\n' "$(sha256sum "$AAB" | cut -d' ' -f1)" "$AAB"
```

Run only the command relevant to the selected variant. Keep checksum line, variant/task name, commit, recursive submodule status, tool versions, signing certificate identity, and build environment together. The CI full-release path writes an adjacent `.sha256` file for `nnaga-<version>-full-release.apk` and uploads it with the APK. The Play Store path uploads the versioned AAB; generate and retain analogous local checksum when comparing it.

To identify signing certificate without exposing private key material:

```sh
apksigner verify --verbose --print-certs "$APK"
```

## 6. Compare cautiously

First compare the SHA-256 values. Equal hashes are strong evidence that files are identical; unequal hashes do not by themselves identify a source, toolchain, or security problem. Signed APK/AAB bytes may differ because signing adds or updates signing-block metadata, and ZIP metadata/order/compression can vary. NNAGA additionally writes `BUILD_DATE`, `BUILD_TIME`, and `BUILD_HOST` from the current clock and machine name, so two otherwise equivalent builds can legitimately have different bytes.

When hashes differ, compare observable contents and provenance instead of claiming reproducibility:

```sh
# APK structure, entries, and signing identity.
zipinfo -1 "$APK" > apk.entries
apksigner verify --verbose --print-certs "$APK"

# AAB structure (an AAB is a ZIP container).
zipinfo -1 "$AAB" > aab.entries
unzip -t "$AAB"

# If bundletool is installed, inspect the bundle manifest.
if command -v bundletool >/dev/null 2>&1; then
  bundletool dump manifest --bundle="$AAB" > aab.manifest.xml
fi

# Compare entry lists, then inspect same selected entries in both outputs.
diff -u first/apk.entries second/apk.entries || true
unzip -l "$APK"
```

Use the same commands and selected paths for both outputs. Also compare package/application ID, version code/name, ABI set, requested permissions, asset/plugin lists, native library names, and signer certificate fingerprint. For an AAB, compare its manifest and module/asset-pack contents with a bundle-aware Android tool such as `bundletool` when it is available; do not treat a generated universal APK as the same artifact as the original AAB.

If byte identity is required for a particular release, investigate the first differing entries and control the clock, host-derived values, dependency/toolchain inputs, signing configuration, ZIP normalization, and build environment as a separate engineering effort. This guide intentionally reports differences and comparable properties rather than claiming signed outputs are byte-identical.
