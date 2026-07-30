# Audio Stack Optimization Plan

## Goal

Reduce callback CPU, scheduler jitter, and safe USB queue depth without weakening real-time guarantees:

- no allocation, blocking control locks, or unbounded work in audio callbacks;
- preserve `PluginChain` dry pass-through on `try_lock` contention;
- preserve exact PCM, transport, loop, and plugin semantics;
- measure on physical ARM64 hardware at blocks 16, 32, 64, 128, 256, and 512;
- accept an optimization only with a measurable callback, xrun-margin, latency, or power/thermal benefit.

The current baseline already includes host `-O3`, ThinLTO, optional PGO, ARM64 NEON peak/mix kernels, exact division-free USB packet scheduling, chunked `memcpy` SPSC rings, and an equal-rate WAV bulk-copy path.

## Required measurement matrix

Exercise these rack profiles separately:

1. empty graph;
2. four dry tracks;
3. four and eight LV2 instances;
4. one neural LV2 instance;
5. one and two VST instances;
6. mixed-rate WAV tracks;
7. transport, loop, BPM, and parameter changes.

Record:

- stage time: capture wait/decode, graph, plugin wrappers, PCM encode, playback wait;
- callback `p50`, `p95`, `p99`, and maximum CPU work duration;
- deadline misses, capture/playback xruns, transfer and lifecycle failures;
- ring occupancy, eventfd writes/wakeups, and bytes copied;
- end-to-end impulse/analog latency where applicable;
- thermal headroom, core residency, power, and binary size;
- callback allocations: always zero.

## Priority 1 — Direct USB zero-copy PCM

### Playback

Current path:

```text
float L/R -> DirectUsbOutput::pcm_ -> playback ring -> USB transfer
```

Replace the intermediate PCM buffer with a bounded playback-ring producer API that exposes up to two writable spans across wrap-around. Pack 16/24/32-bit PCM, including 24-in-4, directly into those spans and publish the producer cursor once after all complete frames are written.

### Capture

Current path:

```text
USB transfer -> capture ring -> DirectUsbOutput::capturePcm_ -> float mono
```

Expose up to two readable capture-ring spans and convert the selected channel directly into the render float buffer. Publish the consumer cursor once. Preserve sign extension, left-justified subslots, incomplete-frame handling, underrun counters, and zero-fill of unavailable frames.

### Verification

- compare old/new isolated ARM64 kernels and full driver block cost;
- run USB scheduler, driver, realtime, and PCM format tests;
- run at least two 30-second live cycles at the calibrated profile;
- require zero deadline misses/xruns/transfer/lifecycle failures before recalibration.

### Verified implementation (2026-07-30)

- Playback and capture now convert directly against bounded two-span ring reservations; the staging PCM vectors are gone. Producer/consumer cursors publish only complete frames after conversion.
- Wrap-around, split-frame, 16/24/32-bit, 24-in-4, watermark, lifecycle, and pending-transfer contracts pass `41` realtime, `19` scheduling, and `25` USB driver tests.
- Isolated ARM64 measurements showed playback conversion about `8%` faster at 32 frames and `6–10%` faster at 256 frames; capture was neutral at 32 frames and up to about `6%` faster at 256 frames.
- Xiaomi live validation passed two 30-second cycles at 48 kHz, 24 valid bits in 4-byte subslots, four device channels, 48-frame blocks, and period multiplier `5`: zero actual xruns, capture overruns/underruns, deadline misses, transfer failures, or lifecycle failures. Estimated host queue latency was `8.4375 ms`; this is not analog end-to-end latency.
- Multiplier `4` produced one playback xrun in one of two 30-second cycles, so it is not accepted as the stable live profile under the observed device load. The earlier 32-frame/multiplier-2 profile also failed repeated current-load runs and must not be presented as calibrated stable.

## Priority 2 — USB transfer batching

`onIso()` currently performs ring cursor loads/stores, wrap checks, counters, and underrun bookkeeping per USB packet. Reserve/copy/publish once per transfer while retaining per-packet lengths, status, silence, and xrun accounting. Apply the same principle to capture where packet errors and implicit-feedback metadata allow it.

Replace event-thread single-writer `fetch_add`/CAS counters with thread-local counters plus release publication where ownership is provably single-writer.

## Priority 3 — Coalesced USB wakeups

Reduce `eventfd_write` and `poll` wakeups by signalling state transitions rather than every completion:

- capture insufficient -> enough frames;
- playback full -> enough writable frames.

Use a proven pending-notification protocol and explicit stop wakeup. Test disconnect, stop/restart, ring wrap, and contention for lost wakes before adoption.

## Priority 4 — LV2 mailbox and transport overhead

Fixed-size LV2 atom/worker/output messages contain roughly 8 KiB payloads. Replace whole-object zeroing and assignment with size-aware bounded payload storage:

- metadata SPSC ring plus bounded byte ring, or fixed slots copying only `size`;
- retain drop counters and allocation-free callbacks;
- do no payload work while queues are empty.

Compute beat, bar, bar-beat, speed, and time-position data once per graph block. Reuse a pre-forged payload for compatible LV2 atom inputs instead of repeating `floor`, `fmod`, and forge construction per plugin.

Direct LV2 audio-port binding is conditional: it can remove input/output copies only after stable buffer bindings, mono behavior, active-port reconnect semantics, and `lv2:inPlaceBroken` handling are proven.

## Priority 5 — RackGraph fast paths and publication cadence

- publish UI-facing transport status every 16–32 blocks, while keeping internal transport sample-accurate and publishing state transitions immediately;
- cache duration/rate-derived values in immutable snapshots;
- empty track chain: mix directly from source;
- empty master chain: write mix directly to final output;
- first active track: scaled copy instead of clear plus add;
- single dry unity-gain track: direct copy/pass-through where semantics permit.

A later architectural step may replace per-chain shared locks and graph hazard barriers with one immutable audio-graph snapshot and an audio-thread epoch acknowledgement. Do not weaken the current `seq_cst` hazard protocol without a formal reclamation proof.

## Priority 6 — VST/Wine IPC

Version the shared-memory ABI from interleaved audio rings to planar left/right rings. This removes host interleave scratch, guest deinterleave, guest reinterleave, and host deinterleave passes.

Replace guest `Sleep(1)` polling with a cross-process wake primitive such as a verified futex/WaitOnAddress-compatible sequence. The Android audio thread must never wait for Wine.

Add target occupancy to the 16,384-frame VST ring so its emergency capacity cannot become steady-state latency. Measure one and two VST instances with impulse latency and underrun telemetry.

Before performance changes, normalize guest `stop_flag`, `guest_frames_produced`, and related shared fields to atomic cross-process accesses.

## Priority 7 — Scheduling, ADPF, memory, and PGO

- choose performance CPUs from observed capacity/frequency data, not CPU number; retain cpuset/permission fallbacks and avoid hard pinning when too few CPUs are allowed;
- report actual CPU work rather than USB wait/backpressure time to ADPF;
- prefault bounded USB, graph, and startup buffers off the RT thread;
- optionally `mlock` only bounded buffers after checking `RLIMIT_MEMLOCK`; never use `mlockall`;
- collect representative production PGO profiles across the full rack matrix; generation instrumentation must never ship.

Reject generic `-Ofast`/`-ffast-math`, SoC-specific `-mcpu`, broad allocator replacement, and unmeasured linker flags because they risk plugin floating-point semantics, fleet compatibility, code-size/cache regressions, or provide no benefit to an allocation-free callback.

## External library policy

Large dependencies are acceptable only when they beat the current ARM64 implementation or unlock a required DSP algorithm. Gate every prototype on physical ARM64 with zero callback allocations and at least a 10–15% end-to-end or stage-specific win.

### Benchmark-gated SIMD abstractions

- [Google Highway](https://github.com/google/highway) — C++17, Apache-2/BSD-3; prefer static AArch64 dispatch for tiny blocks.
- [xsimd](https://github.com/xtensor-stack/xsimd) — C++17, BSD-3, NEON64 and Android CI.

Use these only if they outperform or materially simplify multiple copy/interleave/mix/FIR kernels. They are not expected to beat existing hand-NEON automatically.

### Conditional DSP prototypes

- [Ne10](https://github.com/projectNe10/Ne10) — BSD-style 3-clause, Android/NEON FFT and FIR; initialize plans off RT.
- [KFR](https://github.com/kfrlib/kfr) — GPLv2/v3 or commercial, C++20; consider for future FFT, convolution, filters, oversampling, and SRC if a C++20 target is acceptable.
- [SpeexDSP](https://github.com/xiph/speexdsp), [libsamplerate](https://github.com/libsndfile/libsamplerate), soxr, or r8brain — mixed-rate resampling only after CPU, quality, and algorithmic-delay comparison.
- PFFFT/KissFFT/FFTS — future convolution/EQ only, with all plans/workspaces created off RT.
- [ARM Compute Library](https://github.com/ARM-software/ComputeLibrary) or [XNNPACK](https://github.com/google/XNNPACK) — only for future in-host neural inference, not PCM transport or graph copies.

Allocator libraries and server-oriented oneDNN do not address the current host callback bottlenecks.

## Delivery order

1. Direct USB playback/capture zero-copy PCM.
2. Per-transfer USB batching and single-writer counter publication.
3. Coalesced eventfd wakeups.
4. LV2 size-aware mailboxes and shared time-position preparation.
5. RackGraph dry/empty-chain fast paths and reduced status publication.
6. Planar VST shared-memory ABI and guest wake primitive.
7. Capacity-aware scheduling, corrected ADPF accounting, bounded prefaulting, and representative PGO.
8. Recalibrate minimum stable watermark after every verified reduction in CPU/jitter; only a lower stable queue is considered an end-to-end latency win.
