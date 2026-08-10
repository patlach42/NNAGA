# NNAGA support matrix

## Status and scope

NNAGA is a fully vibe-coded experimental fork. This page describes the repository's declared build baseline and the evidence currently recorded in repository documentation; it is not a certification list. A device or interface is not fully supported merely because it meets a row below. Validate the exact phone, Android build, USB interface, format, and settings before relying on NNAGA, especially for live or production use.

## Supported baseline (build and runtime target)

| Area | Repository baseline | Interpretation |
| --- | --- | --- |
| Android API | API 26 (Android 8.0) minimum; target SDK varies by flavor | The app is configured to build against this minimum. It does not establish compatibility with every API-26 device or vendor image. |
| CPU ABI | `arm64-v8a` only | The Gradle configuration filters native packaging to `arm64-v8a`; other ABIs are not a supported distribution target in this repository. |
| Native build | JDK 17, Android SDK Platform 35, Android NDK `27.2.12479018`, CMake `3.22.1` | These are the documented local-build prerequisites, not runtime hardware requirements. |
| USB host | Android USB host feature is declared `required=false` | Direct USB is optional at package-install time. A phone must still provide a usable USB host path for direct USB operation. |

The baseline is a compatibility floor and packaging statement, not a promise of audio stability, timing, or latency.

## Direct USB prerequisites

Direct USB audio requires all of the following for a meaningful run:

1. A phone/tablet and Android build with a working USB host connection, plus a physical OTG path, power arrangement, and cable/hub suitable for the interface.
2. An Android USB Audio Class device that is discoverable through `UsbManager` and authorized by the user. The instrumentation tests skip when no authorized USB-audio device is present.
3. A selected device and an advertised output format that NNAGA can open: sample rate, valid bits, PCM subslot bytes, and an even channel count sufficient for the selected output pair. The app probes the device and starts only an exact matching format.
4. Exclusive ownership of the selected UAC interfaces for the session. Do not use Android `AudioTrack`/`AudioRecord` on the same interfaces; keep the Java `UsbDeviceConnection` alive until native stop and close complete.
5. A tested buffer size and direct-USB period multiplier. The repository exposes buffer choices from 4 to 1024 frames, but labels 4, 6, 8, 12, 24, 48, 72, and 96 as experimental. Multipliers are constrained by the app to 1–8; a value being selectable is not evidence of stability.

Permission revocation, device detach, file-descriptor errors, and interface-claim failures are lifecycle failures, not compatibility evidence that can be generalized to another device.

## Full vs Play Store boundary

| Flavor | Target SDK | Included capability | Distribution boundary |
| --- | ---: | --- | --- |
| `full` | 28 | Includes the Windows VST host dependency and optional Wine/FEX stack | Sideloaded builds (including direct APK/F-Droid-style distribution) |
| `playstore` | 35 | Omits the VST host dependency; uses Play Asset Delivery packaging | Play Store packaging |

Both flavors share the Android baseline and `arm64-v8a` native filter. The flavor boundary is intentional: installing the Play Store flavor does not provide the `full` flavor's VST host. This table describes repository packaging; it does not imply that either distribution is available for every device or that Play Store policy acceptance is guaranteed.

## Evidence tiers

Use the highest tier actually demonstrated, and do not promote a lower tier into a support promise:

- **Declared baseline:** build files, manifests, and source configuration only. Establishes intended API/ABI/package behavior, not a working audio result.
- **Probe/permission evidence:** the device enumerates, the user grants USB permission, and the advertised format can be selected. Establishes availability of a path, not sustained stability.
- **Lifecycle/stress evidence:** the hardware-gated Android diagnostics complete their requested cycles without reported transfer, lifecycle, xrun, or deadline failures. This remains configuration-specific and does not prove analog or end-to-end latency.
- **Measured profile:** a report includes the exact device identity, format, buffer, multiplier, cycles, logs, and measurement method. It may justify a device/configuration-scoped observed profile only; it does not generalize to other devices, firmware, hubs, or settings.

The repository's direct-USB notes record an **observed Audient iD4 MKII profile** (VID `0x2708`, PID `0x0009`, UAC2, 48 kHz, 24-bit in 32-bit subslots, four channels) and a **separate observed Xiaomi live profile**. These are evidence-backed observations, not fully supported-device designations or universal latency claims. The Audient host-queue estimate in the notes excludes conversion, device FIFO, analog loopback, and acoustic latency; the Xiaomi profile is likewise not a promise.

## Reporting fields

When reporting a result or failure, include enough detail to reproduce the exact configuration:

- NNAGA version/commit, flavor (`full` or `playstore`), build type, and installation source.
- Phone/tablet manufacturer and model, Android version/API level, vendor/firmware build, and whether the device is rooted or modified.
- USB interface manufacturer/model, USB Audio Class version if known, VID/PID, connection topology (OTG adapter, hub, external power), and whether Android permission was granted.
- Selected format as `sample rate / valid bits / subslot bytes / channels`; output pair and capture routing if relevant.
- Buffer frames and direct-USB period multiplier; include startup/queue/watermark settings when a calibration or stress run changed them.
- Test type, duration, cycle count, start/stop/reconnect actions, and whether the result was probe-only, lifecycle/stress, or an actual audio/analog check.
- Full relevant logcat output, including direct-USB telemetry, skip/failure reason, xrun/transfer/lifecycle counters, estimated host-queue field, and timestamps. Attach the exact command/instrumentation arguments when tests were run.
- For latency claims, state whether the number is host-queue accounting, measured digital round trip, or analog/acoustic latency; include the measurement method and hardware.

## Known non-guarantees

- No universal device, Android-vendor, USB-hub, format, buffer, scheduling, performance, xrun, or latency guarantee is made.
- A passing repository test or a stable run proves only the exercised counters and lifecycle for that run. It does not prove microframe timing, USB clock/PLL continuity, packet payload correctness, device FIFO behavior, analog output continuity, or absence of a short pending-transfer gap.
- Android/vendor policy may deny urgent scheduling, CPU affinity, or performance hints; correctness does not depend on those hints.
- The documented Audient and Xiaomi observations do not establish support for another interface, phone, firmware, cable, hub, format, or buffer/multiplier combination.
- Direct USB may fail because of permission, detach, power, interface ownership, unsupported advertised format, or vendor-specific USB behavior even when the app installs and launches.
- VST hosting is a `full`-flavor-only capability. Play Store packaging intentionally omits it; this matrix does not promise that arbitrary Windows VST2/VST3 plugins will run.
- Report failures and observed profiles as issue reports using the fields above rather than inferring compatibility from a similar-looking device.
