# NNAGA (NNAGA Not Android Guitar App)

[![NNAGA logo](logo.png)](https://github.com/patlach42)

NNAGA means **NNAGA Not Android Guitar App**. It is an open-source Android guitar effects host with a chainable rack for LV2 effects, neural amp models, audio-file playback, recording, presets, and plugin editors rendered through the Android UI. The project is maintained by [patlach42 on GitHub](https://github.com/patlach42).

## Features

- Chain LV2 effects and reorder them in the rack
- Neural amp modeling (NAM and AIDA-X)
- WAV playback through the effects chain
- Raw-input and processed-output recording
- Preset save and restore
- Native plugin editors displayed through the Android X11/EGL compatibility layer
- Optional Windows VST2/VST3 hosting in the `full` flavor through Wine and FEX emulation

## Direct USB audio

NNAGA includes a direct USB Audio Class (UAC) path for supported, user-authorized USB audio interfaces. The Android layer discovers the device and obtains permission; the native C++ driver then uses the borrowed `UsbDeviceConnection` file descriptor and the bundled USBFS/libusb backend. It claims the required UAC interfaces and alternate settings, negotiates a supported PCM format, and transfers audio with preallocated isochronous buffers.

For devices with implicit feedback, capture starts before playback and capture-derived packet metadata is paired with output transfers. The transport uses bounded rings and explicit stop/reap/join ordering. Buffer size, period multiplier, format, channels, and output pair are configurable, and the app reports transport state and xrun diagnostics. Scheduling, CPU affinity, and Android performance hints are best effort; device behavior and measured results depend on the interface, phone, format, and configuration. This documentation makes no universal latency or performance claim.

The direct path owns the USB interfaces for the session; it must not be combined with Android `AudioTrack`/`AudioRecord` routing on the same interface. Keep the Java USB connection alive until native stop and close have completed.

## Requirements

- Android 8.0 (API 26) or newer, on `arm64-v8a`
- JDK 17
- Android SDK Platform 35
- Android NDK `27.2.12479018`
- CMake `3.22.1`
- Host packages: `ninja-build meson python3 python3-mako pkg-config autoconf automake libtool gettext patch cmake flex bison ocaml ocamlbuild ocaml-findlib libnum-ocaml-dev`

Initialize the repository and its pinned dependencies before building:

```sh
git submodule update --init --recursive
```

## Build with Make

Run `make help` for the same list from the command line. Native builds invoke `build.sh`; Android packaging uses the repository's Gradle wrapper. `build.sh` and `run.sh` remain the authoritative build scripts.

| Target | Command delegated to | Purpose |
| --- | --- | --- |
| `native` | `./build.sh` | Build native libraries using the default (`full`) setup |
| `full` | `./build.sh full` | Build native libraries for the full flavor, including the optional VST host stack |
| `playstore` | `./build.sh playstore` | Build native libraries and asset-pack layout for Play Store packaging |
| `debug` | `./run.sh debug` | Build and (when a device is available) install/start the full debug app |
| `release` | `./run.sh release` | Build the full release APK and AAB, then optionally install/start the APK |
| `assemble-full-debug` | `./gradlew assembleFullDebug` | Assemble the full debug APK only |
| `assemble-full-release` | `./gradlew assembleFullRelease` | Assemble the signed full release APK |
| `assemble-playstore-debug` | `./gradlew assemblePlaystoreDebug` | Assemble the Play Store debug APK |
| `assemble-playstore-release` | `./gradlew assemblePlaystoreRelease` | Assemble the signed Play Store release APK |

Before any Android packaging target, configure all four signing properties privately in `~/.gradle/gradle.properties` (not in this repository):

```properties
RELEASE_STORE_FILE=/private/path/to/upload.keystore
RELEASE_STORE_PASSWORD=...
RELEASE_KEY_ALIAS=...
RELEASE_KEY_PASSWORD=...
```

Keep the keystore and passwords private. Every Android build variant uses this one signing key, and packaging cannot proceed until all four `RELEASE_*` values are configured. The `playstore` run path additionally uses the upload credentials supplied through its private environment configuration when it installs split APKs locally.

## Architecture

- **Kotlin and Jetpack Compose:** Android UI, settings, USB permission/device management, and lifecycle
- **C++17 audio engine:** rack processing, LV2 host integration, recording/playback, and JNI bridge
- **Direct USB transport:** the standalone `liblowlatencyaudio` C++17 component (`LibusbUacDriver`, `UsbScheduling`, and `DirectUsbOutput`)
- **LV2 stack:** lilv and bundled LV2 plugin content
- **Plugin UI compatibility:** Cairo/Mesa and the project's minimal X11/EGL bridge
- **Optional Windows VST host:** Wine, FEX, and related components in the `full` flavor; omitted from `playstore`

The application owns the Android engine, rack, plugins, JNI, Kotlin/UI, and VST integration. The standalone direct-USB component owns its transport, scheduling, rings, lifecycle, and diagnostics. See [`liblowlatencyaudio/README.md`](liblowlatencyaudio/README.md) and [`liblowlatencyaudio/AGENTS.md`](liblowlatencyaudio/AGENTS.md) for its detailed contract. Third-party dependency inventory and notices are in [`3rd_party/README.md`](3rd_party/README.md).

## Licensing and attribution

The application and project-owned code are distributed under GPL-3.0-or-later; see [`LICENSE`](LICENSE). `liblowlatencyaudio/LICENSE` is the component's GPL-3.0 license text and remains applicable to that component. Third-party code is not relicensed by NNAGA: retain each dependency's original copyright notices and license terms, including the bundled libusb LGPL-2.1-or-later notices and the licenses in `3rd_party/`. Refer to the corresponding source directory and notice files when redistributing binaries.
