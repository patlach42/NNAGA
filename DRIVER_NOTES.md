# Direct USB production profile

- Audient iD4 MKII (`VID 0x2708`, `PID 0x0009`), `48 kHz`, `24-bit` in
  32-bit subslots, four channels: graph buffer `16`, period multiplier `3`.
- USB pump: eight transfers of four high-speed packets. This keeps 4 ms already
  submitted to the kernel while exposing 0.5 ms completion granularity.
- Calibrated userspace watermark: `288` frames (6 ms). On the tested device the
  complete measured host queue was 10.67–10.83 ms, median 10.74 ms. This is
  capture→graph→playback queue time, not analog loopback; ADC/DAC latency is not
  included.
- Four 30-second start/run/stop cycles with a cold `MainActivity` launch during
  cycle 1 passed with zero xruns, transfer failures, lifecycle failures, and
  deadline misses. Artifact: `/tmp/direct-usb-id4-final-stable-o2.json`.
- Targets below 9 ms were rejected: 144/192-frame watermarks produced real
  playback underruns during Android lifecycle/display scheduling. The shortest
  measured queue is not the production-safe queue.
- Debug APKs compile only the native realtime path with `-O2`; UI/JNI debug
  code and symbols remain debuggable. ADPF Performance Hint reporting is active
  when the runtime exposes it.
- CPU affinity and urgent scheduler requests are best-effort. Android/vendor
  policy may ignore or deny them; they are never part of the correctness
  contract.
- Unknown USB devices retain the conservative generic transfer and watermark
  policy. Do not apply the Audient profile without device-specific hardware
  evidence.

## Optimization constraints

- Do not use global `-ffast-math`, `-Ofast`, `-mcpu=native`, fixed SVE, or
  unverified `restrict`/alignment assumptions.
- ThinLTO, PGO, NEON specialization, vectorization pragmas, and thermal-aware
  policy are benchmark candidates, not assumed wins. Validate numerical output
  and callback p50/p95/p99, deadline misses, and xruns on arm64 hardware.
- `io_uring` does not replace USBFS `SUBMITURB`/reap. Realtime scheduler, IRQ
  affinity, cpufreq, usbfs sysctls, and kernel patches require privileges a
  normal Android app does not have.