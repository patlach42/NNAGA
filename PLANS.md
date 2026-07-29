# Future plans

## Adaptive parallel processing of independent tracks

### Decision

Do not parallelize the current `Track 1 → mix → Master` path. The master chain consumes the mixed track output, so these two chains are data-dependent and must run in order. Pipelining the master for block $N$ against Track 1 for block $N+1$ would add one full block of latency and complicate transport, state, and plugin-event ordering.

Parallel processing becomes useful when the rack has at least two independent tracks:

```text
Track 1 chain ─┐
Track 2 chain ─┼─ mix ─ Master chain
Track 3 chain ─┘
```

### Proposed design

- Keep every plugin chain sequential; plugins inside a chain depend on the previous plugin's output.
- Parallelize only independent track chains, then join once before mixing and the master chain.
- Use a persistent, pre-created real-time worker pool. Never create threads from the audio callback.
- Preallocate jobs, worker scratch buffers, and mix buffers for the maximum supported graph shape.
- Dispatch without allocation or contended locks; use bounded lock-free generation counters/queues.
- Give workers urgent-audio priority and deliberate CPU affinity. Reserve one core for the render/coordinator thread where the device topology permits it.
- Use a bounded join deadline. A late worker must not block the render thread indefinitely; record the miss and use the graph's defined fail-closed behavior.
- Preserve one immutable `AudioProcessContext` for the whole block so sample position, transport frame, BPM, loop boundary, and plugin events are identical across workers.
- Run the master chain only after all track results for the current block have joined.

### Adaptive policy

Parallel dispatch is not automatically faster. At small quanta (`16–64` frames), cross-core wake-up, cache traffic, and the join can cost more than lightweight DSP. Keep sequential processing unless all of these hold:

1. At least two independent active tracks exist.
2. Their measured/predicted DSP cost exceeds a calibrated dispatch threshold.
3. The current quantum leaves enough deadline runway for cross-core scheduling.
4. Thermal or CPU-pressure state has not made worker wake-up unstable.

Prefer static track-to-worker assignment while the graph is unchanged. Rebalance only on a control thread after graph edits, never in the callback.

### Verification gate

Implement only with benchmarks that compare sequential and parallel modes on real hardware for `16, 32, 64, 128, 256, 512, 1024` frames and representative lightweight, LV2 neural, and Wine VST racks. Required evidence:

- no audio-thread allocations, blocking mutexes, or unbounded waits;
- no transport/event ordering changes;
- no additional block of end-to-end latency;
- lower peak callback time for multi-track heavy racks;
- no regression for one-track-plus-master racks;
- stable sustained run under UI interaction and thermal load.

### Current status

Deferred. The current graph has one track plus master, which is a serial dependency, and the Audient iD4 audit shows USB queue/scheduling—not DSP compute—as the current latency limit. Revisit after multi-track support exists or real racks approach the DSP deadline.
