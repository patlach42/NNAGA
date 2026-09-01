# NNAGA release operator guide

This guide describes the manual **Build & Deploy** workflow in
`.github/workflows/build-deploy.yml`. The checked-out `version.properties` file
is the sole source of the Android application version. The workflow does not
accept version overrides and does not patch source files.

## Version contract

At the repository root, `version.properties` contains exactly one assignment
for each of these keys:

```properties
VERSION_NAME=0.1.0
VERSION_CODE=100
```

`VERSION_NAME` is a complete SemVer 2.0.0 value without a leading `v`.
Use compatibility-oriented increments:

- **MAJOR**: an incompatible public/API change.
- **MINOR**: backward-compatible functionality.
- **PATCH**: backward-compatible bug or security fixes.

The optional `-prerelease` component uses dot-separated ASCII
alphanumeric/hyphen identifiers (numeric identifiers have no leading zeroes).
The optional `+build` component uses the same identifier syntax and does not
affect SemVer precedence; build metadata alone is not a prerelease.

`VERSION_CODE` is independent of SemVer and is the Android/Google Play
ordering integer. Every published prerelease or release MUST use a previously
unused `VERSION_CODE` larger than every code already published for
`com.vibes.dsp`, including versions containing `+build` metadata. Before a
Play upload, confirm the next code is unused in Play Console.

### Dashboard display suffix

The dashboard derives its clean suffix from Git rather than a version-file
counter. It counts commits since the latest commit that introduced the current
exact `VERSION_NAME` line:

```sh
git log --format=%H -S"VERSION_NAME=<version>" -- version.properties
git rev-list --count <baseline>..HEAD
```

The count is displayed as no suffix for `0`, otherwise spreadsheet-style
lowercase base-26 (`1=a`, ..., `26=z`, `27=aa`, `28=ab`). Changing
`VERSION_NAME` establishes a new baseline; if history is rewritten or a commit
is cherry-picked, the baseline is recomputed from the resulting history.

For a dirty worktree, status is exactly:

```sh
git status --porcelain=v1 --untracked-files=all --ignore-submodules=dirty
```

When that output is non-empty, the dashboard appends `-dirty-<letter>`, using
the same spreadsheet-style mapping for the index (`1=a`, ..., `26=z`,
`27=aa`). The root ignored file `dirty.version` stores the next local
dirty-build index;
when absent, the index starts at `1`. The index is incremented exactly once,
atomically, after a successful local Gradle graph containing at least one app
assemble/bundle task. It is never incremented during configuration, a failed
graph, a clean graph, or GitHub Actions. These display suffixes do not alter
SemVer, Android package metadata, canonical artifact names, or release tags.

Git tags use the `v<version>` convention: for `VERSION_NAME=1.2.0`, the tag is
`v1.2.0` and the GitHub Release display name is `NNAGA v1.2.0`. Artifacts and
tags are SemVer-only; dashboard display suffixes are never embedded in them.


## Ordered release checklist

1. **Choose and record the identity.** Update `VERSION_NAME` and
   `VERSION_CODE` in root `version.properties`. Do not edit
   `app/build.gradle.kts`, pass version inputs, or use environment overrides.
   The dashboard suffix is derived automatically from Git history and local
   dirty state.
2. **Prepare the changelog.** Move the entries under `[Unreleased]` in
   `CHANGELOG.md` under a heading `## [<version>] - YYYY-MM-DD`, using the
   exact `VERSION_NAME`, and recreate an empty `[Unreleased]` section.
3. **Commit the source.** Commit `version.properties`, the changelog update,
   and any release changes. Record the immutable commit/ref to dispatch; do
   not dispatch a worktree state that has not been committed.
4. **Dispatch the exact ref.** In **Actions → Build & Deploy → Run workflow**,
   select that commit (or an exact ref resolving to it). Set either or both
   of the independent booleans:

   | Input | Default | Effect |
   | --- | --- | --- |
   | `create_github_release` | `true` | Build and publish the full/VST APK and GitHub Release. |
   | `upload_to_play_store` | `false` | Build and upload the Play Store AAB as an internal draft. |

   A Play-Store-only dispatch does not run the full/VST path; selecting both
   paths does not merge their publication boundaries.
5. **Verify the preflight.** Confirm the initial `version` job accepts the
   committed two-key properties, exports the expected name/code, and reports
   `prerelease=true` only when `VERSION_NAME` has a `-prerelease` component.
   A `+build` component alone must report `prerelease=false`.
6. **Verify exact build outputs.** For a full release, inspect
   `app/build/outputs/versioned/apk/fullRelease/nnaga-<version>-full-release.apk`.
   For Play Store, inspect
   `app/build/outputs/versioned/bundle/playstoreRelease/nnaga-<version>-playstore-release.aab`.
   Confirm the embedded manifest `versionName` and `versionCode` equal the
   two committed properties. These versioned paths are the supported
   distributables; do not search AGP output directories with a glob or use
   legacy artifact names.
7. **Verify the full publication.** The full APK and its checksum sidecar are
   published as workflow artifact `nnaga-<version>-full-release-apk`. Confirm
   the GitHub Release is tagged `v<version>`, displayed as `NNAGA v<version>`,
   and has the pre-release state reported by preflight. After downloading the
   APK and sidecar together, run:

   ```sh
   VERSION_NAME=1.2.0   # use the committed value
   sha256sum -c "nnaga-${VERSION_NAME}-full-release.apk.sha256"
   ```

   The sidecar must contain the hash for the exact basename
   `nnaga-<version>-full-release.apk`.
8. **Verify the Play publication, when selected.** Confirm the exact AAB is
   published as workflow artifact `nnaga-<version>-playstore-release-aab` and
   that the upload action consumed that file for package `com.vibes.dsp` on
   the `internal` track with status `draft`. Store review, promotion, and
   production rollout remain operator actions.
9. **Handle failures safely.** A failure before any GitHub Release/tag or Play
   artifact publication may be fixed and rerun with the same version values.
   Once a release/tag or Play artifact has been published, do not replace it:
   choose the next appropriate SemVer and always increment `VERSION_CODE`.

## Secrets

Configure these as repository or environment Actions secrets before dispatching
a path that builds a signed artifact:

- `KEYSTORE_BASE64`: base64-encoded Android upload keystore. The workflow
  decodes it into a temporary runner file and removes that file in cleanup.
- `KEYSTORE_PASSWORD`: keystore password.
- `KEY_ALIAS`: signing-key alias.
- `KEY_PASSWORD`: signing-key password.

The Play Store path additionally requires `PLAY_STORE_KEY`, the Google Play
service-account JSON consumed by the upload action. It uploads package
`com.vibes.dsp` to the `internal` track with status `draft`.

Do not paste secret values into inputs, logs, issue comments, or release notes.
The workflow passes signing values to Gradle and does not intentionally print
their contents. Full/VST and Play Store publication remain independent, so
only provide the secrets required by the selected path.
