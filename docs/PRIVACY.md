# Privacy notice

_Last reviewed: 2026-08-10_

NNAGA is an experimental, vibe-coded fork. This notice describes the behavior visible in the repository at the time of review; it is not a legal promise, certification, or guarantee about every device, build, plugin, or service.

## What Android access NNAGA requests

The application manifest declares:

- `INTERNET`, which permits network requests. The in-app Tone3000 integration uses this for requests to `https://www.tone3000.com/api/v1`.
- `READ_MEDIA_AUDIO` (and `READ_EXTERNAL_STORAGE` on Android 12 and earlier), which can be used when the user selects audio or other files through Android storage/file pickers. Whether Android asks for a runtime permission depends on the picker and OS version.
- USB host support is optional. NNAGA can use a USB audio device after the user selects/authorizes it; the manifest does not declare broad device or location access.

The app also declares Android backup enabled (`android:allowBackup="true"`). The contents and scope of backup are controlled by Android and the selected backup provider; treat app-local data as potentially included in device backups.

## Data NNAGA keeps locally

When used, NNAGA stores or processes data on the device, including:

- audio-engine and device settings, including USB calibration profiles and measured diagnostic values;
- rack/plugin configuration, UI preferences, favorites, and imported plugin metadata;
- bundled and imported plugin files, VST/VST3 files, model/impulse-response files, and Tone3000 downloads under the app's internal files area (for example `lv2`, `plugins`, `vst_plugins`, `neural_models`, and model directories);
- temporary imported audio files in the app cache while an audio file is decoded or loaded; and
- Android/application logs. The code logs operational events, paths/identifiers, request URLs, and—on Tone3000 API errors or search responses—may log response text. Android system logging controls retention and access to those logs.

These are local app/device data categories. NNAGA's rack, audio processing, plugin loading, calibration, favorites, and file-management code operates on the device. Local processing does not by itself mean that data is sent to NNAGA maintainers or to Tone3000.

Files selected through a picker are copied into app-private storage for use by the relevant plugin. Deleting an item in NNAGA's file UI deletes the selected local file; Android backups, caches, system logs, and other copies may have separate lifecycles.

## Optional Tone3000 connection

Tone3000 integration is optional. If you browse Tone3000, sign in, follow a Tone3000 link, or download a Tone3000 model, NNAGA makes network requests to the Tone3000 service (and to the model URL returned by that service). If you do not use those features, the Tone3000 request flow described here is not started by NNAGA's Kotlin code; the manifest still grants the general Android `INTERNET` capability.

Depending on the action, the external service receives information needed for the request, such as:

- the search text and selected filters (gear, size, format, architecture, calibration, sort, and paging);
- a Tone3000 tone/model identifier or URL when fetching details or downloading a model;
- an API key supplied through the Tone3000 sign-in callback when NNAGA exchanges it for a session;
- an access token in an `Authorization: Bearer` header for authenticated requests, and access/refresh tokens in the refresh request; and
- the request's `User-Agent` (`GuitarRackCraft/<version>`) and `X-App-Id: GuitarRackCraft` headers.

Tone3000's own server, authentication page, model hosts, and network providers may also process normal connection data such as IP address, timestamps, and request metadata. This notice does not describe or control their practices; consult their terms and privacy information. NNAGA does not have enough repository evidence to state what Tone3000 retains or how it uses account data.

Tone3000 responses can include account/profile information displayed by NNAGA (for example, the authenticated username), tone/model metadata, and downloadable files. Those responses and files are handled locally after receipt. NNAGA does not upload the user's microphone/audio stream, rack state, VST binaries, or arbitrary local files as part of the Tone3000 API methods shown in this repository. This statement is limited to those methods: a separately installed or imported plugin may have its own behavior that NNAGA has not audited.

### Authentication and tokens

The browser-based sign-in uses a `guitarrackcraft://tone3000auth` callback. NNAGA receives the callback API key and exchanges it with Tone3000 for an access token and refresh token. It stores both values in ordinary Android `SharedPreferences` named `tone3000_prefs` under `access_token` and `refresh_token`; the repository does not show encryption or Android Keystore wrapping for these preferences. Tokens are sent to Tone3000 as described above, not to NNAGA maintainers. A failed authenticated request can trigger token refresh. Choosing **Log out** removes both stored token values and clears the in-memory authenticated user state.

A Tone3000 deep link can also provide a tone URL. NNAGA uses it to retrieve Tone3000 metadata and download the selected model to local storage; it does not send the user's local model file back as part of that flow.

## VST/plugin files and logs

NNAGA imports or extracts plugin binaries and associated resources into app-private directories and may read user-selected files to install or configure a plugin. VST hosting can involve Wine, native libraries, X11 UI components, and third-party plugin code. NNAGA cannot make a repository-grounded statement that every third-party plugin is offline or that its logs never leave the device. Review a plugin's own documentation and permissions before installing or loading it.

Operational failures and diagnostics can appear in Android logs. Error logging may include exception messages, paths, plugin names/IDs, request URLs, HTTP status codes, and response bodies. Avoid sharing logs publicly without reviewing them for tokens, URLs, usernames, local paths, or other sensitive content.

## User controls

- Decline or revoke Android media permission when the platform permits; do not use the file/audio import or picker features that require it.
- Do not open Tone3000 browsing/sign-in/download features if you do not want those requests. You control which local files are selected or imported.
- Use **Log out** in the Tone3000 UI to clear the stored access and refresh tokens. You can also clear NNAGA app storage or uninstall the app through Android settings; that removes app-private data subject to Android backup, filesystem, and system-log behavior.
- Remove downloaded models, imported plugins, favorites, and other files using NNAGA's controls where available, or Android app-storage/file-management controls. Clear the app cache to remove temporary cached imports; cache cleanup is separate from app data and backups.
- Review Android backup settings and your backup provider if app-local data should not be backed up.
- To stop an external service's processing or request account/data deletion, contact that service through its own support channels; NNAGA cannot perform those server-side actions.

## Questions and issue reports

For a repository-grounded privacy question or suspected privacy bug, open an issue at [github.com/patlach42/NNAGA/issues](https://github.com/patlach42/NNAGA/issues). Do not include access tokens, API keys, private audio, unredacted logs, or other sensitive material in an issue.
