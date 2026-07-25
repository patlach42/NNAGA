# Repository Guidelines

## Project Overview

Guitar RackCraft is an Android real-time guitar-effects rack. It hosts LV2 effects through a native Oboe audio engine, renders plugin UIs through sliders, Modgui WebView, or a custom X11/EGL surface, and the `full` flavor optionally hosts x86/x64 Windows VST2/VST3 plug-ins through Wine/FEX.

## Architecture & Data Flow

- **Startup:** `app/.../MainActivity.kt` extracts assets, initializes native paths through `EngineInitHelper`, calls `NativeEngine.nativeInit()`, then renders Compose navigation.
- **Audio:** Kotlin facades (`NativeEngine`, `AudioEngine`, `RackManager`) call JNI in `app/src/main/cpp/jni/NativeBridge.cpp`. `AudioEngine` receives Oboe callbacks and sends stereo buffers through `PluginChain`.
- **Plugins:** `PluginRegistry` selects factories by `format:id`. `LV2PluginFactory` scans extracted LV2 bundles with Lilv; `LV2Plugin` runs instances and handles state, atom/patch, workers, and UI metadata.
- **UI:** `RackViewModel` owns `StateFlow` UI state. `ModguiScreen` bridges WebView controls to ports. `X11PluginUIActivity` runs native editors in the `:x11ui` process.
- **VST (full only):** `vsthost_lib` uses `VstFactory` → `WineVstPlugin` → `WineHostProcess`; lock-free shared-memory rings move audio/parameters between the Android process and Wine/FEX guest. The VST host uses the project X11 server.

**Real-time rule:** Do not allocate, block, or take contended exclusive locks in audio callbacks. `PluginChain` intentionally uses `try_lock` and passes audio through on contention; expensive teardown belongs off the RT path.

## Key Directories

- `app/src/main/java/com/varcain/guitarrackcraft/` — Compose UI, ViewModels, Android lifecycle, JNI facades.
- `app/src/main/cpp/` — Oboe engine, LV2 host, plugin chain/registry, JNI bridge, custom X11 server.
- `vsthost_lib/` — optional Wine/FEX VST host library, native IPC/X11/Wine launcher, VST executables/assets, build scripts.
- `cmake/` — standalone Android cross-build of X11, LV2, and plugin ecosystem; Ninja preset is `android-arm64`.
- `gxplugins_pack/`, `neural_pack/`, `brummer_pack/` — install-time Play Asset Delivery packs.
- `tests/x11/` — native X11 unit and wire-protocol tests.
- `scripts/`, `tools/` — build, VST triage/state capture, metadata and thumbnail utilities.
- `patches/`, `3rd_party/` — externally sourced code and patches; avoid casual edits.

## Development Commands

```bash
# Initialize external sources before a clean checkout build
git submodule update --init --recursive

# Build native LV2/X11/plugin payloads, then Android full flavor
./build.sh                 # full; BUILD_VST=0 skips expensive Wine/FEX build
./run.sh debug             # build/install/start full debug APK
./run.sh release           # full release AAB + APK flow
./run.sh playstore         # Play Store flavor/bundletool flow

# Android variants
./gradlew assembleFullDebug
./gradlew assembleFullRelease
./gradlew assemblePlaystoreDebug
./gradlew assemblePlaystoreRelease

# Native prebuild directly
cmake --preset android-arm64 -S cmake
cmake --build --preset android-arm64

# Native X11 suite
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

`build.sh clean` resets submodules and removes ignored build outputs; use only when explicitly intended. `vsthost_lib/scripts/build-all.sh` accepts phases such as `llvm`, `winedeps`, `wine`, `fex`, `dxvk`, `mesa`, `turnip`, `hosts`, and `pack`.

## Code Conventions & Common Patterns

- Kotlin: use existing `object` JNI facades, `data class` DTOs (`PluginInfo`, `PortInfo`), `StateFlow` in ViewModels, and Compose state/events. Run slow rack/VST/file work on `Dispatchers.IO`, not the main thread.
- C++: use RAII/`std::unique_ptr`, atomics for meters/RT flags, narrow lock scopes, and explicit JNI marshalling. Keep native factory routing in `PluginRegistry`; do not add format-specific conditionals to the engine.
- Plugin state and UI work must preserve format boundaries: LV2 uses Lilv features/ports; VST uses `vsthost_lib` IPC and Wine prefixes.
- X11 work is serialized through the existing executors/display manager. Do not introduce concurrent direct Xlib access.
- Shell scripts use Bash with `set -euo pipefail`, uppercase environment/configuration variables, functions, and named build-phase logging.

## Important Files

- `settings.gradle.kts`, `build.gradle.kts`, `gradle.properties` — modules, plugin versions, repository and Gradle defaults.
- `app/build.gradle.kts` — SDK levels, `full`/`playstore` flavors, asset packs, Android/CMake integration, release signing properties.
- `app/src/main/cpp/jni/NativeBridge.cpp` — native context and JNI registration; VST factory is conditional on `HAS_VST_HOST`.
- `app/src/main/cpp/engine/AudioEngine.cpp` — RT Oboe processing, meters, playback, recording.
- `app/src/main/cpp/plugin/PluginChain.cpp` and `PluginRegistry.cpp` — rack synchronization and plugin factory dispatch.
- `vsthost_lib/src/main/cpp/vst/WineVstPlugin.cpp` and `launcher/WineHostProcess.cpp` — VST audio IPC and Wine process lifecycle.
- `cmake/CMakePresets.json` — canonical native cross-build preset.
- `run.sh` and `build.sh` — supported local build/install workflows.

## Runtime/Tooling Preferences

- Use the Gradle wrapper (`./gradlew`), not a system Gradle.
- Java/Kotlin target is **17**; Gradle wrapper is 8.9; AGP is 8.7.3; Kotlin is 1.9.20.
- Android app code is `arm64-v8a` only, min SDK 26. The `full` flavor has target SDK 28 for Wine compatibility; `playstore` targets 35 and excludes VST hosting.
- Native prebuild needs Android NDK and CMake 3.22.1. `vsthost_lib` pins NDK `26.1.10909125`; its full Wine/FEX toolchain is disk- and time-intensive.
- Release signing values belong in `~/.gradle/gradle.properties`, never in committed `gradle.properties`.

## Testing & QA

- Native X11 tests use CMake, CTest, and GoogleTest 1.14.0 in `tests/`. They cover core stores, protocol handshake/replies, framebuffer/image operations, GLX, and optional XCB client behavior.
- The XCB test target is optional and requires `pkg-config` plus libxcb development files.
- `app/build.gradle.kts` declares JUnit/AndroidX/Espresso/UIAutomator/Compose test dependencies, but there are currently no `app/src/test` or `app/src/androidTest` test sources.
- `vsthost_lib/src/main/cpp/ahbspike/AhbChannelTest.cpp` is an on-device JNI diagnostic, not an automated test suite.
- For behavior changes, run the narrowest relevant native/Gradle command; for UI/device changes, install the relevant APK and exercise the changed path. Use `adb logcat | grep -E 'AudioEngine|NativeBridge|LV2Plugin|PluginBrowser'` for runtime diagnostics.
