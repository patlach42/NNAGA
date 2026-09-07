# The sweep behind the shipped defaults

Every arm below ran the same pinned frame: quantum 32, period multiplier 2,
5 transfers, 4 packets per transfer, write headroom 416, credit admission with a
32 frame reserve, `rt_priority` applied, loopback detectors armed, and the NNAGA
interface open in front. 45 s cycles.

`out_ms` is `estimated_host_queue_latency_ms`, and it is a **round trip**, not an
output-only figure: the capture ring enters it directly through
`max(Q, captureRing, captureTransfer) + Q + playbackTarget`.

## Capture target

| arm | target | capture ring | hard timeouts | deadline misses | verdict |
|---|---|---|---|---|---|
| `capA56` | 56 (the old automatic value) | 61-70 | 0 | 0 | clean, 3.44-4.13 ms |
| `capB28c` | 28 | 39-54 | 0 | 0 | clean, 4/4 |
| `capB28long` | 28, eight cycles | 39-63 | 0 | 0 | clean, 2.67-3.15 ms |
| `capC16` | 16 | 27-43 | **1** | **1** | breaks: 29 frames against a 32 frame quantum |

`capA56`'s telemetry was recovered from a host-side logcat dump; the arm's own
copy was lost to the post-run `logcat -d` hang that the runner no longer uses.

## Playback target

| arm | target | out_ms | zero runway | xruns | verdict |
|---|---|---|---|---|---|
| `capB28long` | 64 (the old automatic value) | 2.67-3.15 ms | 0 of 8 | 0 of 8 | clean, one discontinuity cluster |
| `pb56b` | 56, eight cycles | **2.50-3.00 ms** | **0 of 8** | **0 of 8** | clean on every counter |
| `pb56` | 56 | 2.50-3.50 ms | 0 of 4 | 0 of 4 | first two cycles lost to a USB permission dialog |
| `pb48c` | 48 | 2.50-2.83 ms | **2 of 4** | **2 of 4** | past the edge |

## defaults6

The attempt to verify the shipped defaults end to end. It shows `packets=4` and
`capture_target_frames=28` live, and then the interface began dropping its
session on every start, so the playback target rule is verified by test through
the real driver path but not yet on hardware. A physical reconnect is needed to
finish it.
