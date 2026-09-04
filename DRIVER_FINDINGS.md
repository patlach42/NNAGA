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
