# NNAGA release operator guide

This guide describes the manual `Build & Deploy` workflow in `.github/workflows/build-deploy.yml`. The workflow is an experimental release path for this fork; operators should review the build logs and downloaded files before publishing them.

## Dispatch inputs

Open **Actions → Build & Deploy → Run workflow** and provide:

| Input | Required | Workflow behavior |
| --- | --- | --- |
| `version_name` | Yes | String patched into `app/build.gradle.kts` as `versionName`. The workflow description gives `1.0.0` and `1.0.0-beta.1` as examples. |
| `version_code` | Yes | String patched as `versionCode`; the workflow description requires an integer higher than the previous release. |
| `create_github_release` | Yes (default `true`) | Runs the full/VST build and GitHub Release path when true. |
| `upload_to_play_store` | Yes (default `false`) | Runs the separate Play Store AAB path when true. |

The two booleans are independent. A dispatch can run either path or both. A Play-Store-only dispatch does not run the full/VST build.

## Secrets

Configure these as repository or environment Actions secrets before dispatching a path that builds a signed artifact:

- `KEYSTORE_BASE64`: base64-encoded Android upload keystore. The workflow decodes it into a temporary runner file and removes that file in cleanup.
- `KEYSTORE_PASSWORD`: keystore password.
- `KEY_ALIAS`: signing-key alias.
- `KEY_PASSWORD`: signing-key password.

The Play Store path additionally requires `PLAY_STORE_KEY`, the Google Play service-account JSON consumed by the upload action. It uploads package `com.vibes.dsp` to the `internal` track with status `draft`.

Do not paste secret values into inputs, logs, issue comments, or release notes. The workflow passes signing values to Gradle and does not intentionally print their contents.

## Full GitHub Release output

When `create_github_release` is true, the workflow runs `assembleFullRelease` after staging the VST host components. It discovers the first `*.apk` under:

```text
app/build/outputs/apk/full/release
```

After discovery it writes a SHA-256 manifest beside that APK (`<apk-path>.sha256`, containing the `sha256sum` output). Both files are uploaded as the workflow artifact named:

```text
nnaga-full-<version_name>-apk
```

The same APK and `.sha256` sidecar are attached to the GitHub Release. The release tag requested by the workflow is `v<version_name>` and its display name is `NNAGA v<version_name>`. Names containing `beta`, `alpha`, or `rc` are marked prerelease by the workflow.

Verify the checksum after downloading both files, for example:

```sh
sha256sum -c NNAGA-full-release.apk.sha256
```

Use the actual downloaded APK filename in place of the example. The checksum proves the downloaded file matches the sidecar; it does not independently establish provenance or security of the build.

## Version and tag constraints

The workflow does not perform semantic-version or numeric validation. Operators must supply a valid `version_name` for the resulting GitHub tag `v<version_name>` and a numeric, monotonically increasing `version_code` accepted by Android/Google Play. Avoid tag-invalid or shell-sensitive characters in `version_name`; use the documented forms such as `1.0.0` or `1.0.0-beta.1`.

The workflow passes `tag_name` to `softprops/action-gh-release`; it does not contain an explicit `git tag` or `git push` command. Do not assume that running the workflow creates or updates a source tag independently of the GitHub Release action. Check the resulting repository state and release page.

## Play Store boundary

The Play Store path runs `bundlePlaystoreRelease`, uploads an AAB (not the full APK), and targets the internal draft track. It is intentionally separate from the GitHub Release path and does not produce the VST/full APK or its checksum sidecar. Store submission, review, promotion, and production rollout remain operator actions outside this workflow.
