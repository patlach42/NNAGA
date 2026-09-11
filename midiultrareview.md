# Ultrareview — findings on the MIDI work-in-progress

Review of branch `main`, 18 files / 726 insertions. Filtered to the files that
are **not** mine (the in-flight MIDI work).

## Result: nothing was reported against these files

The review returned two findings. **Neither is about the MIDI changes.** Both
are about untracked files from the JSFX work, and both are artifacts of how the
review was scoped — see the next section.

Files reviewed and reported clean:

| File | Change |
| --- | ---: |
| `app/src/main/cpp/plugin/LiveMidiSource.h` | +2 |
| `app/src/main/cpp/plugin/IPlugin.h` | +14 |
| `app/src/main/cpp/plugin/RackGraph.cpp` | +21 / −7 |
| `app/src/main/cpp/plugin/lv2/LV2Plugin.cpp` | +13 / −7 |
| `app/src/main/cpp/engine/AudioEngine.cpp` | +2 / −1 |
| `app/src/main/java/com/vibes/dsp/engine/UsbMidiInputManager.kt` | +56 |
| `app/src/main/java/com/vibes/dsp/ui/live/LiveScreen.kt` | +5 |
| `app/src/main/java/com/vibes/dsp/ui/rack/RackScreen.kt` | +5 |
| `app/src/test/java/com/vibes/dsp/engine/UsbMidiInputManagerTest.kt` | +110 |
| `tests/audio/TestLiveMidiSource.cpp` | +66 |
| `tests/audio/TestRackGraph.cpp` | +69 |
| `vsthost_lib/external/vst_host/vst_host.c` | +30 |
| `vsthost_lib/external/vst_host_vst3/vst3_host.cpp` | +29 |

Treat "no findings" as "this review pass found nothing", not as proof the MIDI
work is correct — the reviewer had no device and could not run the audio path.

## The two findings it did return, and why they are not real *yet*

Both say the same thing: `JsfxRamPrealloc.{h,cpp}` and the fixtures
`SliderGroups.jsfx`, `JitterPdc.jsfx`, `GfxContention.jsfx` are referenced but
missing, so the native build and the JSFX contract tests cannot compile.

They were missing **from git**, not from disk. Those files were new and still
untracked when the review bundle was built, so the reviewer never saw them. On
this machine everything configured, built and passed the whole time:

```
plugin_abstraction (arm64)      links
ysfx_smoke_contract_tests       15 / 15 pass
audio_realtime_tests           178 / 178 pass
```

**Resolved** — they are committed alongside the JSFX fixes, so the build
breakage the reviewer described can no longer happen. The underlying warning is
still worth keeping: without the fixtures the four JSFX contract tests abort at
`ASSERT_NE(plugin, nullptr)` and silently certify nothing, which is exactly the
failure mode the reviewer reasoned through correctly.
