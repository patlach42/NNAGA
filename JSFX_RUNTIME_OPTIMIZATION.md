# JSFX Runtime Optimization — Findings

Investigation of the JSFX (ysfx/EEL2) runtime on ARM64. All numbers are measured
on the connected reference device (`25113PN0EG`, platform `canoe`), big cluster
(`taskset f0`), 48 kHz, `ysfx` built from `3rd_party/ysfx` with the app's
`YSFX_PORTABLE=OFF` / `ysfx_android_compat.h` configuration.

Benchmark harness and scripts: `tools/jsfx_bench/` (see "Reproducing" below).

**Status.** Findings 1, 2, 4, 6, 7 and 8 are implemented in `app/src/main/cpp/jsfx/JsfxPlugin.{h,cpp}`
and covered by `JsfxPluginContractTest.SliderChangesApplyAcrossEveryChangeMaskWord`
(11/11 in `ysfx_smoke_contract_tests`). Codegen was verified by disassembly:
`mrs`/`msr FPCR` now appear once per block including on the exception-unwind
path, the 256 `ldaxrb`/`stlxrb` pairs are gone, and the mask fast path is four
plain loads. **The on-device end-to-end re-measurement has not been done** — the
reference device dropped its wireless `adb` link and has not come back. Finding 3 is not implemented; findings 4 and 6 are.

## Summary

The JSFX runtime pays **41–54 ns per sample, per plugin, on floating-point
control-register traffic that buys nothing**. It is pure overhead independent of
what the script does, so it dominates light scripts and is still ~29% of a
mid-weight one. Removing it needs no change to `3rd_party/ysfx`.

Two further defects break the allocation-free callback contract and are the
structural candidates for the reported reverb/synth dropouts: `@init` runs on
the audio thread at every transport start, and EEL's RAM allocator `calloc`s
512 KiB blocks from inside `@sample`.

The EEL2 AArch64 JIT itself is **not** a bottleneck — marginal cost measures at
~0.93 ns per JSFX statement (≈1 cycle/op at 3 GHz). Do not spend effort there.

| # | Finding | Cost today | After fix | Where |
| --- | --- | --- | --- | --- |
| 1 | FPCR saved/restored once **per sample** | 41–54 ns/sample | ~0 | `JsfxPlugin::process` |
| 2 | 256 atomic RMW per block for slider scan | 0.57–0.65 µs/block | 0.002 µs/block | `JsfxPlugin::process` |
| 3 | `@init` executes on the audio thread | +70 µs per transport start | 0 | `ysfx_process_generic` |
| 4 | Lazy 512 KiB `calloc` inside `@sample` | max 35.0 → 23.5 µs cold | — | `__NSEEL_RAMAlloc` |
| 5 | Residual per-sample call overhead | 6.4 ns/sample | needs JIT work | `eel_callcode64` |
| 6 | **Audio thread yields the VM to the UI** | **0.23% / 8.90% of blocks lost** | 0.00% | `JsfxPlugin::process` |
| 7 | **Per-block PDC renegotiation silences the rack** | **39/40 blocks silent** | 0/40 | `JsfxPlugin::process` |
| 8 | **`@slider` runs on the audio thread** | **10580 µs vs 667 µs budget** | 12.9 µs | `JsfxPlugin::process` |

## Finding 1 — FPCR save/restore on every sample (dominant)

`GLUE_CALL_CODE` in `3rd_party/ysfx/thirdparty/WDL/source/WDL/eel2/glue_aarch64.h:218`
wraps every JIT entry:

```c
if (!(h->compile_flags&NSEEL_CODE_COMPILE_FLAG_NOFPSTATE) &&
    !((f=glue_getscr())&(1<<24))) {     /* FPCR.FZ clear? */
  glue_setscr(f|(1<<24));               /* msr fpcr — set flush-to-zero */
  eel_callcode64(bp, cp, rt, consttab);
  glue_setscr(f);                       /* msr fpcr — restore */
} else eel_callcode64(bp, cp, rt, consttab);
```

Three facts make this the hot spot:

1. `ysfx` compiles every section with `NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS` only
   (`sources/ysfx.cpp:479`) — never `NOFPSTATE`, so the check is always live.
2. The Android process default is `FPCR = 0x0`, so **FZ is clear and the slow
   branch is always taken**. Verified on device, not assumed.
3. `ysfx_process_generic` calls `NSEEL_code_execute(fx->code.sample)` **once per
   frame** (`sources/ysfx.cpp:1550`). At 48 kHz that is 48 000 `mrs`+`msr`+`msr`
   sequences per second, per JSFX instance.

`msr fpcr` is a pipeline-serializing write. Isolated microbenchmark:

```
body only          :    5.00 ns/call
+ mrs fpcr         :    4.21 ns/call  (+0.00)   <- reads are free
+ mrs/msr/msr      :   46.37 ns/call  (+41.37)  <- writes are not
```

The ysfx-level measurement agrees independently. Sweeping @sample body size and
fitting cost per sample:

| `@sample` body | current (FZ=0) | FZ pre-set | delta |
| --- | ---: | ---: | ---: |
| 1 statement | 48.8 ns | 7.3 ns | 41.5 |
| 16 statements | 65.2 ns | 21.9 ns | 43.3 |
| 64 statements | 109.1 ns | 65.9 ns | 43.2 |

Fixed overhead is ~48 ns/sample, of which **~42 ns (87%) is FPCR churn**. The
slope is 0.93 ns per statement either way — the JIT is fine, the entry sequence
is not. For a typical 16-statement script, 64% of total runtime is waste.

### Fix

Set `FPCR.FZ` once around the block call, so EEL takes its `else` branch.
This is **semantics-neutral for JSFX**: EEL already forces FZ=1 for the duration
of every JIT call today; we are only hoisting it out of the per-sample loop.
Scoping it to the ysfx call (rather than the whole audio callback) keeps the
blast radius at zero for LV2/VST/native plugins, which is why this is preferable
to a callback-wide `ScopedNoDenormals`.

```c++
// JsfxPlugin.cpp, around ysfx_process_float
namespace {
class ScopedFlushToZero {   // AArch64 FPCR.FZ, bit 24
public:
    ScopedFlushToZero() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
        if (!(saved_ & (1ull << 24))) {
            const uint64_t fz = saved_ | (1ull << 24);
            __asm__ __volatile__("msr fpcr, %0" :: "r"(fz));
        }
    }
    ~ScopedFlushToZero() {
        if (!(saved_ & (1ull << 24)))
            __asm__ __volatile__("msr fpcr, %0" :: "r"(saved_));
    }
private:
    uint64_t saved_;
};
} // namespace
```

Cost becomes one `mrs`+`msr`+`msr` per **block** instead of per **sample** —
a 48–64× reduction of the same traffic.

### Measured result

The saving is a **fixed per-sample cost removed**, so express it that way — the
ratio depends entirely on how heavy the script is.

Scalar-only scripts, measured back to back with an isolated `mrs/msr/msr`
microbenchmark reading 41.4 ns in the same device state:

| `@sample` body | ns/sample saved |
| --- | ---: |
| 1 statement | 41.4 |
| 16 statements | 43.1 |
| 64 statements | 43.1 |
| trivial gain | 43.1 |

A second session on a warmer, lower-clocked device measured 52.0–52.1 ns/sample
saved against a microbenchmark reading 53.7 ns. **The two methods agree in both
states**, which is the load-bearing evidence for this finding.

Scripts that emit denormal output save *more* than the FPCR cost, because
`ysfx_process_generic`'s own sample conversion (`*spl[ch] = ins[ch][i] +
denorm_value` and `outs[ch][i] = (Real)*spl[ch]`) runs **outside** the JIT call
and therefore with FZ=0 today:

| script | ns/sample saved | note |
| --- | ---: | --- |
| 8-voice synth | 62.3 / 62.7 / 62.9 | consistent across 128 / 64 / 48-frame blocks |
| FDN reverb | 78.1 | one session |
| FDN reverb | 52.0 | a later session — no denormal bonus |

The synth's +20 ns reproduced across three block sizes; the reverb's +35 ns did
not reproduce in a second session. Treat the denormal bonus as **real but
state-dependent** — it appears when delay lines or release envelopes have
decayed into the denormal range, which is exactly the condition a reverb tail
reaches. Do not quote it as a guaranteed number.

Resulting speedup, as (real work + 42 ns) / real work:

| script | real work ns/sample | speedup |
| --- | ---: | ---: |
| 1 statement | 7.3 | ~6.7× |
| trivial gain | 8.9 | ~5.8× |
| 16 statements | 22.0 | ~2.9× |
| 8-voice synth | 42.3 | ~2.5× |
| 64 statements | 65.9 | ~1.6× |
| FDN reverb | 105.8 | ~1.5× |

**Light and mid-weight scripts gain 3–6×; heavy ones 1.5×.**

A caution for anyone re-measuring: a single cross-process A-then-B run on this
device produced 2.35× for the reverb, which did not reproduce. Ratios taken from
separate processes absorb whatever DVFS drift happened between them. Quote the
per-sample delta, or use experiment `P`.

## Finding 2 — 256 atomic RMW per block for the slider scan

`JsfxPlugin::process` scans all `ysfx_max_sliders` (256) entries every block:

```c++
for (uint32_t i = 0; i < kMaxSliders; ++i)
    if (dirty_[i].exchange(false, std::memory_order_acq_rel))
        ysfx_slider_set_value(fx_, i, pending_[i].load(...), true);
```

That is 256 `ldaxrb`/`stlxrb` pairs per block per instance, whether or not any
slider moved. Measured **0.57 µs/block**, and 0.65 µs/block in a second session on a lower-clocked device. For a light JSFX this alone doubles
the plugin's cost (0.57 µs of actual DSP after Finding 1 is fixed).

### Fix

Replace the `std::array<std::atomic<bool>, 256>` with four `std::atomic<uint64_t>`
words and bit-scan only what changed — the same shape `ysfx` already uses
internally for `slider.change_mask` (`sources/ysfx.cpp:1453`). `setParameter`
sets its bit with `fetch_or(release)`; `process` does four acquire loads and
only exchanges a word that is non-zero.

Measured: **0.57 µs → 0.002 µs per block (381–388× across two sessions)**. Uncontended cost becomes
four relaxed loads that almost always find zero.

## Finding 3 — `@init` executes on the audio thread at every transport start

`ysfx_set_time_info` (`sources/ysfx.cpp:1378`) sets `must_compute_init` on every
stopped→playing edge, and `ysfx_process_generic` then runs it inline:

```c++
if (fx->must_compute_init)
    ysfx_init(fx);          // on the audio thread
```

`ysfx_init` runs every `@init` section, calls `ysfx_clear_files` (takes a mutex
and destroys file objects), and triggers Finding 4's allocations. `ysfx.cpp:1534`
carries an upstream `// TODO: slider must never run concurrently with @sample or
@block`, and `ysfx.cpp:1307` a matching TODO for `@init` — these are known-unsafe
paths upstream, not something this fork introduced.

Measured with a reverb whose `@init` clears its delay lines (what real reverbs
do), toggling transport every 200 blocks at 48 frames:

```
A steady                p50=5.16  p99.9=16.51  max=311.72 us
E transport toggling    p50=5.05  p99.9=68.39  max= 72.19 us
```

**~70 µs added to the block that starts transport — 7% of a 48-frame budget for
one plugin**, and it multiplies by the number of JSFX instances in the rack,
since they all re-init on the same block. This is the strongest structural
candidate for glitching at loop/transport start.

### Fix

Do not let `@init` run from `process()`. Detect the pending-init edge on the
control thread (the app already owns `controlMutex_` and `UiPauseGuard`), run
`ysfx_init` there, and have the audio thread pass through for that block. If the
edge must be honoured sample-accurately, the alternative is to stop feeding
`ysfx_set_time_info` transitions from the RT thread and drive them from the
transport control path instead.

## Finding 4 — 512 KiB `calloc` from inside `@sample`

`__NSEEL_RAMAlloc` (`eel2/nseel-ram.c:139`) allocates lazily, one block at a
time, on first touch:

```c
p = pblocks[whichblock] = (EEL_F *)calloc(sizeof(EEL_F), NSEEL_RAM_ITEMSPERBLOCK);
```

`NSEEL_RAM_ITEMSPERBLOCK` is `1<<16`, so each miss is a **512 KiB `calloc`
plus first-touch page faults, executed on the audio thread** from the
`_asm_megabuf` slow path. `ysfx` only preallocates when the script itself
declares `options:prealloc=` (`sources/ysfx.cpp:460`), which most scripts do not.

The fast path is fine — `_asm_megabuf` resolves a resident block inline with no
call (`asm-nseel-aarch64-gcc.c:985`). Only the first touch is dangerous.

Measured on cold start, 3000 blocks from a fresh instance at 48 frames:

```
C lazy EEL RAM          p99.9=18.28  max=35.00 us
D RAM preallocated      p99.9=15.94  max=23.49 us
```

Smaller than Finding 3 for these scripts because the test reverb only spans a
few blocks; it scales with how much RAM the script actually touches and how
sparsely it touches it.

### Fix

`jsfxPreallocRam()` claims a bounded budget from `JsfxPlugin::activate`, which
already holds `controlMutex_` and runs off the RT thread. Budget is 256 Ki items
(2 MiB, four EEL blocks) — enough for a typical reverb's delay lines, against
`ysfx`'s default `maxmem` of 8 M items = 64 MiB, which would be far too much to
reserve per instance. Anything past the budget still falls back to EEL's lazy
path.

**`NSEEL_VM_preallocram` alone is not enough**, which the first version of this
document got wrong. `calloc` of a 512 KiB block returns fresh zero pages that
are not resident, so the allocation moves only the `malloc` off the audio
thread and leaves every first-touch page fault behind. Measured across
`activate()`:

```
preallocram only            RSS +28 KiB
preallocram + prefault      RSS +2052 KiB
```

So `jsfxPreallocRam` also writes one word per 4 KiB page to fault the RAM in.
The RAM is already zeroed and `@init` has not run yet, so no script state
changes. Asserted by `JsfxPluginContractTest.ActivateCommitsScriptRamUpFront`.

The VM handle is private to `ysfx_s`, so this needs ysfx's internal headers.
Rather than patch the submodule (it tracks upstream `JoepVanlier/ysfx`), the
dependency is isolated in `app/src/main/cpp/jsfx/JsfxRamPrealloc.cpp` — the only
TU that includes `ysfx.hpp`, with the extra include paths and `-fsigned-char`
scoped to it in CMake, so a submodule bump breaks one small file.

## Finding 6 — the audio thread gave up its block to the UI (the dropout)

This is the one that actually made noise, and it was not a CPU cost at all.

`JsfxPlugin::process` opened with:

```c++
if (uiHost_ && !uiHost_->tryAcquireEffect()) return passthrough();
```

`uiHost_` exists for **any** script with a `@gfx` section, whether or not an
editor is open, and its thread runs a frame loop that takes the same
`effectGate` around `ysfx_gfx_run`. So whenever the gfx thread was mid-frame,
the audio thread abandoned the block and emitted pass-through. The design
comment — "UI never waits for DSP ownership" — is the right instinct applied to
the wrong side: the UI was made non-blocking by making *audio* lossy.

**A lost block is not a glitch-free fallback.** For a synth, pass-through is its
(silent) input, so the voice cuts out. For a reverb it is a dry jump. That is
exactly the reported "разрывы", and it explains the reported split between
editor-open and editor-closed.

Measured with `JsfxGfxGateContractTest.AudioLosesNoBlocksToUiContention`
(32-frame blocks paced to real time, a `@gfx` section doing ~4000 EEL ops per
frame, counting blocks whose output was the input rather than the DSP result):

| | blocks lost | rate |
| --- | ---: | ---: |
| editor closed | 7 / 3000 | 0.23% |
| editor visible @30fps | 267 / 3000 | 8.90% |

At 48-frame blocks that is roughly two dropouts per second with the editor
closed and ninety per second with it open — a 39× difference that matches
"с включённым нативным интерфейсом сильнее, но и без него разрывы постоянные".

### Fix

The audio thread no longer consults the gate. This restores upstream ysfx
behaviour rather than inventing one: `fx->gfx.mutex` guards gfx-against-gfx
only, ysfx's thread-id guards (`ysfx_thread_id_dsp` / `ysfx_thread_id_gfx`)
exist precisely so the two sections can run on separate threads, and ysfx's own
JUCE plugin takes no such lock in `processBlock`. REAPER runs them concurrently
too — it is the JSFX model.

`pauseEffect()`/`resumeEffect()` are unchanged and still bracket `@init`, state
save and state restore from the control thread, where blocking is fine.

After the change both rates are **0.00%**. The test asserts this, and fails
(0.33% / 9.67%) when the gate is put back.

One consequence had to be handled: with `@gfx` and `@sample` genuinely
concurrent, both can now enter `__NSEEL_RAMAlloc`, whose `NSEEL_HOSTSTUB` mutex
ysfx leaves empty — two threads could race on the same `pblocks[whichblock]`.
Finding 4 removes that race as well as the RT allocation, which is why it ships
together with this change rather than after it.

## Finding 7 — per-block PDC renegotiation silenced the whole rack

Found with a second pair of eyes (codex, via `acpx`) after Finding 6 was fixed
and dropouts persisted. This one is not JSFX CPU either — it is a control-rate
value being renegotiated at audio rate.

`pdc_delay`, `pdc_bot_ch` and `pdc_top_ch` are ordinary EEL script globals. A
script may write them from `@init`, `@slider`, `@block`, `@sample` — or `@gfx`.
`JsfxPlugin::process` read them and republished `latencyFrames_` **every audio
block**. Downstream:

- `PluginChain::process` re-sums `getLatencyFrames()` every block
  (`PluginChain.cpp:387`).
- `RackGraph` compares exact integer latency every block and, on any change,
  zeroes `latencyHistoryWrite`/`latencyHistoryValid` for the changed node
  (`RackGraph.cpp:1670-1675`) and, when the global maximum moves, for **every
  track in the snapshot** (`RackGraph.cpp:1681-1688`).
- The delayed read path emits hard `0.0f` while `valid < delay`
  (`RackGraph.cpp:2156-2163`).

There is no debounce anywhere on that path. So a one-sample PDC change from a
single JSFX plugin restarts delay compensation for the entire rack, and each
restart costs `delay` frames of silence on every compensated track.

It gets worse when the value keeps moving. With `delay >= frames` the history
can never refill between changes, so the affected tracks stay silent
indefinitely. Reproduced at the graph level with a plugin whose reported latency
oscillated by one frame per block (delay 128, blocks of 64):

```
39 / 40 blocks fully silent while reported latency oscillated
```

This matches the report — dropouts with no relation to transport, worse with the
editor open (`@gfx` runs 30x/s and can write the same script globals).

### Fix

Latency is a control-rate property, so `JsfxPlugin` now publishes a new value
only once the script has held it for `kLatencyStableBlocks` (16 blocks, ~16 ms
at 48-frame blocks). Real changes still propagate, imperceptibly later; per-block
jitter never reaches the graph. Measured over 200 oscillating blocks: **200
reported-latency changes before, 0 after**
(`JsfxPluginContractTest.OscillatingScriptPdcDoesNotJitterReportedLatency`).

### What was deliberately not changed

The obvious alternative was to stop `RackGraph` discarding history on a latency
change — the ring holds the track's own past output, which a latency change does
not invalidate. That was tried and reverted: `RackGraphPdcTest.LatencyChange`
`ResetsHistoryBeforeCompensatedBlockWithoutAudioAllocation` asserts that after a
transition "no stale pre-transition sample or duplicated read-head value may
precede the aligned signal". Restarting clean is a deliberate choice — for a
*one-off* change a short zero ramp beats time-scrambled audio. The defect was
never that one reset happens; it is that nothing stopped them happening forever.
Fixing the source keeps that contract intact.

**Still worth reviewing separately:** `RackGraph` has no debounce of its own, and
`LV2Plugin` also republishes latency every block (`LV2Plugin.cpp:602`). Any LV2
plugin with a jittering `lv2:latency` port can still silence the rack the same
way. This fix covers JSFX only.

## Finding 8 — `@slider` ran on the audio thread (the slider dropouts)

Found after Findings 6 and 7 shipped and the user confirmed dropouts persisted
on that build, "в основном на jsfx". Independently flagged by codex in the same
pass.

`@slider` is script-defined and unbounded. ysfx executes it inside
`ysfx_process_generic` whenever a slider changed (`ysfx.cpp:1533`), i.e. **on
the audio thread**. Reverbs and synths routinely use it to rebuild coefficient
tables and clear delay lines.

`RackScreen.kt:3124` calls `setParameter` from `onValueChange`, which fires on
every touch-move event, and there is no throttle anywhere in the UI path. So a
drag produces 60-120 parameter updates per second, each landing on a block that
then runs the whole of `@slider`.

Measured with a fixture whose `@slider` clears a 64 Ki buffer and fills a table
(what a real reverb does), 32-frame blocks:

```
steady block                     18.9 us
block carrying a slider change   10580.3 us      (budget 667 us)
```

**16x over budget on every block that carries a slider change.** That is a
guaranteed underrun per update, 60-120 times a second while dragging — exactly
"со слайдерами тоже разрывы постоянные".

### Fix

`JsfxPlugin` now owns a worker thread. `setParameter` records the value and
signals it; the worker applies the pending values with `notify=false` (so ysfx
does not schedule `@slider` onto the audio thread) and then runs `@slider`
itself through `jsfxRunSliderCode()`. The audio thread keeps processing with the
previous coefficients meanwhile. `activate()` runs it inline too, so the first
block after `@init` does not pay for it either.

The worker coalesces by construction: updates arriving while `@slider` runs are
picked up on the next iteration, so a drag collapses to as many runs as the
worker can complete rather than one per touch event.

```
steady block                     12.5 us
block carrying a slider change   12.9 us
```

Asserted by `JsfxPluginContractTest.HeavySliderCostStaysOffTheAudioThread`,
which fails at 8245 us if the work is moved back.

### Two consequences worth knowing

**Parameter application is now asynchronous.** A value set via `setParameter` is
observable only after the worker has run — normally a thread wakeup, but delayed
by a pending `@slider` if one is in flight. `restoreState` inherits this. The
JSFX contract tests wait for application rather than assuming it is immediate.

**`@slider` and `@sample` can now touch the VM concurrently.** This was the
explicit trade accepted when choosing this fix over throttling: a script can in
principle read a half-updated coefficient for one block. It is the same class of
race ysfx already tolerates between `@gfx` and `@sample`, and the same one
Finding 6 relies on. If a specific script misbehaves audibly on parameter
moves, this is the first thing to suspect.

## Finding 5 — residual per-sample call overhead

After Finding 1 the fixed cost is **6.4 ns/sample**: `eel_callcode64`
(`glue_aarch64.h:237`) saves and restores four register pairs plus `x29/x30`,
and each JIT body adds `GLUE_FUNC_ENTER`/`GLUE_RET`. For a 16-statement script
that is still ~29% of runtime.

Removing it means JIT-level batching — emitting the frame loop *inside* the
generated code so the prologue runs once per block rather than once per sample.
That is an invasive EEL2 change with real correctness risk (`spl0..spl63`
rebinding, `@sample` early-exit semantics) and should only be attempted after
Findings 1–4 are shipped and measured. Listed for completeness, not recommended
now.

## Non-findings (do not spend effort here)

- **The AArch64 JIT is active and its codegen is good.** `YSFX_PORTABLE=OFF`
  selects `glue_aarch64.h` + `asm-nseel-aarch64-gcc.c`; the x64 `.S` file in
  `cmake.wdl.txt` is `#if defined(__x86_64__)`-guarded and compiles to nothing.
  Marginal cost is 0.93 ns per statement (≈1 cycle/op). There is no interpreter
  fallback to escape.
- **`-O3`/ThinLTO on the ysfx targets buys nothing.** `ysfx.cpp` currently ships
  at `-O2` with no LTO (verified in `app/.cxx/*/arm64-v8a/build.ninja`), unlike
  `plugin_abstraction`/`audio_engine`. Measured `-O2` vs `-O3` head to head:

  ```
  reverb  O2 p50=7.60us   O3 p50=7.60us
  synth   O2 p50=6.30us   O3 p50=6.25us
  op16    O2 p50=4.27us   O3 p50=4.27us
  ```

  No difference, because the hot code is JIT-emitted at runtime, not compiled.
  Consistent with the existing policy of rejecting unmeasured flags.

## Scale check — is this the cause of the dropouts?

Be careful here. For the scripts measured, one JSFX instance costs
**0.4–0.8% of a 48-frame budget** today. The optimizations above are large in
*relative* terms but a single JSFX plugin is not, on its own, enough to cause a
dropout at the calibrated profile.

What that means:

- Findings 1 and 2 are worth doing — they are cheap, safe, and cut steady-state
  JSFX cost by 1.5–6× depending on script weight, which matters for **rack depth** (how many JSFX
  instances fit before the budget is gone) and for thermal headroom.
- Findings 3 and 4 are the ones that can actually produce an *audible glitch*,
  because they are unbounded work on the RT thread rather than steady cost.
  Finding 3 in particular fires exactly when the user starts transport — which
  matches "разрывы на ревербах" better than any steady-state cost does.
- The reported symptoms should still be confirmed against real telemetry before
  assuming this investigation explains them. Which specific reverb/synth scripts
  glitch, and whether the glitches correlate with transport start, would settle
  it. A very heavy script, or a deep rack, would also change the arithmetic above.

## Delivery order

1. **Finding 1** — `ScopedFlushToZero` around `ysfx_process_float`. App-side
   only, no submodule change, removes 41–54 ns/sample. **Done.**
2. **Finding 2** — slider change-mask bitset, matching the existing
   `NativePlugin::dirty_` idiom. App-side only, 381–388× on that scan. **Done.**
3. **Finding 3** — move `@init` off the audio thread. Removes a ~70 µs × N-plugin
   spike at transport start.
4. **Finding 4** — bounded RAM prealloc in `activate()`. Needs a small ysfx
   accessor; consider upstreaming.
5. Re-measure the rack matrix from `OPTIMIZATION_PLANS.md` with JSFX profiles
   added, then reconsider Finding 5 only if JSFX is still a measured bottleneck.

Per the project's measurement policy, each step is accepted only on a measured
callback/xrun-margin benefit on physical ARM64, with zero callback allocations.

## Reproducing

The harness builds `ysfx` for `arm64-v8a` against the same configuration the app
uses and runs five isolated experiments per script (each in a fresh process, to
avoid FPCR and DVFS cross-contamination between measurements):

```
A  steady state, FPCR.FZ=0   (what ships today)
B  steady state, FPCR.FZ=1   (Finding 1 applied)
C  cold start, lazy EEL RAM
D  cold start, RAM preallocated
E  transport toggling every 200 blocks (Finding 3)
P  paired A/B interleaved in one process, 40x500-block bursts (DVFS-neutral)
```

**Use `P` to finalize any speedup number.** Experiments A and B run in separate
processes and are therefore exposed to clock drift between them; `P` alternates
FZ=0 and FZ=1 bursts against the same warmed instance, so both sides see
identical thermal and DVFS conditions. `P` was added after the runs recorded
here and has not yet been executed — the reference device dropped its wireless
`adb` link before it could run. Re-run it before quoting ratios in a changelog.

```sh
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release .
cmake --build build
adb push build/bench fx libc++_shared.so /data/local/tmp/ysfxbench/
adb shell "cd /data/local/tmp/ysfxbench && LD_LIBRARY_PATH=. \
  taskset f0 ./bench fx/reverb.jsfx 48 A"
```

Always pin to the big cluster and run each experiment in its own process — an
earlier iteration that ran A–E in one process produced a spurious 2× swing
purely from DVFS ramp and leftover FPCR state.
