# Driver findings

A running log of what measurement established, kept because several of these
were believed the other way round first, and the reasons matter more than the
conclusions.

## Instruments lie before code does

**The capture detector was never armed.** `inspectCapture` only judges a block
once the decayed peak exceeds 0.02. The bench loopback sat at 0.0097, so every
"clean" run said nothing at all: a silent loop and a perfect one produce the
same output. Raising the monitor knob to 0.168 made the same configuration
report seven breaks in 120 seconds. A run whose detector never armed now fails
on its own gate rather than passing.

**Detector events did not decide verdicts.** Signal, transfer and capture
discontinuities reached the flight log only. A run with thirty-nine capture
breaks passed every gate it had. They now fail the run.

**Extremes are not the wobble.** Session minimum and maximum occupancy are set
by a single rare stall, so they cannot separate the geometric sawtooth from a
preemption. Percentiles sampled per drain separate them: 92/124/164 against
extremes of 54 and 216 at quantum 64.

**Startup and steady state must be measured apart.** Averaged together they
describe neither: the same build reports a burst of three writes between drains
at startup, where the prime does exactly three, and one in steady state.

**`deferred_no_pcm` counts retries, not damage.** One long pending episode
produces thousands of attempts, and the count already varied by orders of
magnitude with harness polling alone. It must not be read as a severity.

## Things that were never happening

**Ring `mlock` never took effect.** `RLIMIT_MEMLOCK` is 64 KiB on the reference
device and an unaligned 64 KiB ring spans 68 KiB of pages, so `mlock` returned
ENOMEM and `VmLck` stayed at zero in every arm, including the one written to
pin. The optimisation whose latency cost was being debated had never run. With
the rings page aligned so it could run, six randomised blocks found no
difference at all.

**Aligning the quantum to the drain chunk made things worse.** Quantum 48 is a
whole number of 24-frame drains where 64 is not, but the smaller quantum wakes
the render thread every millisecond instead of every 1.33, and the extra
exposure to preemption cost more than the phase alignment saved: the ring hit
zero and capture breaks went from two to twelve.

## Frame conservation

**Silence padding is gone.** A short drain used to be padded so the packet kept
its scheduled length, which invented audio and counted it as played - and since
nothing ever removes inserted frames, every episode pushed the signal
permanently later. Packets are now shortened to what was drained. On the
implicit-feedback path the shortfall is unreachable by construction; only the
free-running path can be short.

**A refused quantum is a lost quantum.** Admission used to discard a rendered
block when the ring could not take it, and the flight recorder shows the break
one millisecond later. The producer now waits for room instead; waiting costs
at most one drain.

## The startup click

The producer published on its own wakeups, so at session start it worked
through the capture pre-roll even though the initial stock already equalled the
target, drove the ring 58 frames past its steady maximum and lost a quantum to
the ceiling. Deeper priming and a startup-only headroom both fix it by keeping
the frames - 1.75 ms permanently, or 64 frames that can only ever be removed by
dropping or resampling. Waiting for room fixes it by keeping the frames only
while they are needed.

## Playback credit, and what it is really a knob for

Pacing the producer by frames the device has played removes the overshoot and
holds far less rendered audio in the pipeline: occupancy 52-108 against
156-236 under wait-for-room, which is 2.5 ms of latency.

Strict credit also starves. Frames held as credit are a right to write, not
audio, so the playable stock is prime minus credit and producer lateness shrinks
the buffer one for one - measured, the ring reached zero, the OUT queue reached
zero, and the producer stood still with ninety free frames in front of it.

A reserve turns it into an axis rather than a rule: the reserve is how far the
producer may run ahead, zero being strict credit and a large reserve being
wait-for-room. One quantum of reserve already removed the collapse - runway 72
instead of 0 - at occupancy 108 instead of 196.

Composing them as "credit if available, else room" does not give both: a
disjunction takes the weaker constraint, so the room rule stops the credit rule
from binding and the latency returns to the wait figure.

## Environment dominates single runs

The same configuration reported six capture breaks in one block, none in the
next and ten in a third. Every conclusion drawn from a single run in this
project has had to be withdrawn, including two of mine on the same evening.
Comparisons are paired blocks with a fixed seed, and the unit is the block.

## Capture had its own silence

`readInputChannels` zero-filled every destination before decoding, so a short
read handed the graph real audio followed by invented silence - the capture-side
twin of the playback padding, and invisible for the same reason. Worse, on this
bench that silence went out through playback and came back through the hardware
loop, where the capture detector counted it as a break and the environment took
the blame. Short reads are now refused whole and counted. Channels the format
does not carry are still zeroed: a channel that does not exist is not a channel
that was invented.

## Three kinds of frame loss wore different names

Padding a short packet, discarding a refused quantum, and a rendered block that
never reached the ring because a wait expired. Each looked like ordinary error
handling in its own function, and none of them was gated. They are the same
event - audio that was produced and never played - and only the first was even
visible in a counter.

## A refusal stopped being a loss

With a depth-one holding slot the refused block is published on the next cycle
instead of being discarded, so `playbackQuantumDrops` now measures pressure and
`lostQuanta` measures damage. Measured over sixty seconds: 718 blocks held,
one lost. Gating on refusals after this change would fail runs in which nothing
was lost, which is how a counter that used to mean damage becomes a counter that
means nothing.

## Held blocks must not be charged twice

The credit for a held block was spent on its first attempt; charging it again on
republication failed at startup, where credit is scarce, and destroyed the block
the holding slot existed to save. Startup admission refusals went from two to
zero once republication stopped re-billing.

## The reserve has an optimum, and it is small

Six randomised blocks of reserve 64, 128 and 192 frames at target 256 with five
transfers:

| reserve | clean | ring p50 | capture breaks |
|---|---|---|---|
| 64 | 5/6 | 124 | 16 |
| 128 | 5/6 | 188 | 14 |
| 192 | 1/6 | 188 | 40 |

Paired over blocks, 64 against 128 differs by 64 frames of occupancy (p=0.031)
with no difference in breaks (p=1.0), so the smaller reserve buys 1.33 ms for
nothing. The largest reserve is worse on both counts: past some point the
producer's lead stops being insurance and starts being latency that still fails.

## What established practice says, and where we differed

Researched against kernel `sound/usb`, ALSA, JACK2, PortAudio, WASAPI, the UAC2
specification and Android's own guidance.

Aligned already: slaving OUT cadence to the capture endpoint under implicit
feedback is exactly what `sound/usb/implicit.c` does; treating a late producer
as an xrun rather than concealing it is universal - ALSA returns `-EPIPE`, JACK
notifies, PortAudio raises `paOutputUnderflow`; and lock-free SPSC without
assuming realtime priority is the consensus design, because locks are only safe
with priority inheritance and Android refuses SCHED_FIFO.

Departures worth knowing:

**A bounded producer lead is standard, but it is expressed as remaining buffer
capacity, not as a separate credit counter.** WASAPI exposes `GetCurrentPadding`
and ALSA `avail_update`; the producer throttles against space. Our credit is a
homegrown formulation of the same idea. It works, and the reserve makes it an
axis, but it is not how anyone else states it.

**Latency is reported as configured depth, never as live occupancy.** The two
are deliberately not conflated: occupancy drives flow control internally, and
the number shown to a person stays still. We were reporting occupancy, which is
precisely why the figure appeared to wander - it was measuring the pipeline
breathe rather than describing its configuration. Now split: configured depth is
the reported latency, live occupancy has its own field.

**A missed presentation deadline should end the epoch, and does not yet.**
Established practice makes it an xrun, and ours drops the block and carries on,
which reads as delivery to everything downstream. Making it terminal was tried
twice and killed sessions during startup, before the transport reached Running -
gating on that state was not enough, because the pipeline is still filling and
the render loop has not settled. It fails the audit as a counted loss for now.
Turning it into a stop needs the startup path measured first; guessing at it
twice was already one time too many.

**snd-usb-audio keeps synchronised URBs under a millisecond and shorter than a
period**, for the stated reason that there is no way to know in advance where
the next period ends - the same strategy as ours at 0.5 ms, but its overall
queue ceiling is far deeper than our 2-3 ms, which is worth remembering before
treating a single clean run as qualification.
