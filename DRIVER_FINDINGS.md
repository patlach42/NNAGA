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

## The arithmetic that found the last loss

One quantum was still lost per session, almost always in the first second, and
the state captured at the moment of loss looked contradictory: room exhausted
while credit was plentiful, and the same signature under every reserve. The
reserve sweep could not tell its arms apart on it.

The stock accounting settled it. With `ring + queued + credit = prime` as the
invariant, the three snapshots were over by exactly whole quanta:

| arm | ring + queued + credit - prime | unaccounted quanta |
|---|---:|---:|
| r0 | 232 + 120 + 96 - 256 = 192 | 3 |
| r64 | 240 + 120 + 152 - 256 = 256 | 4 |
| r128 | 240 + 120 + 88 - 256 = 192 | 3 |

The cause was an overcorrection of the earlier double-charge fix: the credit
wait only tests, the charge inside publication happens after the room check, so
a block refused for room reached the holding slot unpaid and republished with
charging disabled. Every held publication silently granted a whole quantum of
lead - three or four of them being more unaccounted stock than any reserve
under test, which is exactly why no reserve made any difference.

Charging on entry to the slot instead removed the last loss: zero lost quanta,
at startup and in steady state, with occupancy unchanged.

It also revealed how close the pipeline runs: 45253 blocks held in sixty
seconds, against roughly 45000 render cycles. The holding slot is not an
exception path any more, it is the normal route. Zero loss is necessary but it
is not sufficient - a mechanism that engages on every single block has no margin
left for the next disturbance.

## Four ways to account for a held block, three of them wrong

The holding slot needs an answer to when its block pays its credit, and each
plausible answer failed differently. Measured on one configuration, sixty
seconds, target 256 with five transfers and a reserve of 64:

| | lost | held | ring p50 | credit at loss |
|---|---:|---:|---:|---:|
| A: publish held for free | 1 | 718 | 244 | +96 |
| B: charge on entry, ignore the result | 0 | 45253 | 252 | - |
| C: charge on entry, force the debt | 1 | 1087 | 204 | -168 |
| D: reserve before rendering | 1 | 731 | 228 | -40 |

**A** breaks the ledger: every held publication grants a whole quantum of lead,
and three or four of them are more unaccounted stock than any reserve under
test - which is why a reserve sweep running at the time could not tell its arms
apart.

**B** loses nothing, but 45253 holds against roughly 45000 render cycles means
the slot is the normal route rather than an exception, and a mechanism that
engages on every block has no margin for the next disturbance.

**C** looks like honest accounting and is not: forcing the debt drove credit to
-168 against a reserve floor of -64, turning a bounded lead into an unbounded
overdraft, and the tighter dynamics starved the producer further.

**D** is the right shape - reserve credit and room before capture is consumed,
so a block never exists without somewhere to go - but a first implementation
made it worse: 605 admission refusals, because waiting for room is not the same
as reserving it. Between the check and the publication lies the whole graph
render, and the held block from the previous cycle takes the space in between.
The contract requires the slot to be emptied before capture is read at all,
which this did not do.

Reverted to A pending a correct implementation. The lesson is the shape of the
mistake rather than the numbers: three of these were defensible on paper, and
only the ledger arithmetic and the hardware told them apart.

## The ceiling was the problem, not the admission rule

Five admission policies were tried against the last remaining loss and every
one of them lost exactly one quantum at startup. What they shared was the state
at the moment of loss: `ring + queued` between 348 and 360 frames against a
logical capacity of `target + headroom` = 256 + 104 = 360. Credit was always
positive - the right to write existed, the room did not.

So the pipeline was hitting its own ceiling, and no rule about who may publish
can help when there is nowhere to publish to. Doubling the headroom to 208
settled it at once:

| | lost | held | ring p50 | reported latency |
|---|---:|---:|---:|---:|
| target 256, headroom 104 | 1 | 718 | 236 | 384 |
| target 256, headroom 208 | 0 | 3 | 316 | 384 |
| target 192, headroom 208 | 0 | 3 | 252 | 320 |

The holding slot went from 718 engagements per minute to three, which is what
an exception path should look like.

Note the middle row: raising the ceiling alone costs 80 frames of real
occupancy, about 1.7 ms, while the reported latency does not move - it is
computed from the target. Lowering the target by the same amount recovers it,
and the last row is better than the original on both counts rather than being a
trade: no loss, an exception path that is exceptional again, and 64 frames less
latency than where this started.

The lesson is that the working point sat against the ceiling, so every
disturbance became a refusal, and five rounds of rewriting the admission rule
were five rounds spent on the wrong layer. The stock arithmetic is what
eventually pointed at it.

## The bench hears the phone

Three consecutive cycles reported eight to eleven capture discontinuities each,
across all three configurations, and then the next cycle was clean again. The
cause was an alarm going off on the device: another app taking the audio path
disturbs the device itself, and the hardware loopback faithfully records the
result as breaks in our capture.

This is a measurement hazard rather than a driver defect, and it is invisible in
the counters - a disturbed cycle looks exactly like a badly configured one. The
runner now records concurrent audio playback and pending alarms per cycle, so
such a cycle can be excluded on evidence.

It also explains a pattern seen repeatedly today: a configuration that fails
three cycles running and then passes six. Before blaming geometry for that, ask
what else the phone was doing.

## Where the capture breaks come from

Counting them was not the same as knowing them. The flight log separates two
populations.

**Two breaks were the detector describing itself.** Every run, every
configuration, at about 0.23 seconds, with the decayed peak at 0.03 to 0.06
against a steady 0.44: the level was still climbing, so an ordinary sample step
measured against a small peak looked enormous. The modulation detector had this
fixed with a warmup and the discontinuity detector never did. With a tenth of a
second of settling the phantom pair is gone and the remaining events all occur
at full level.

**The real ones arrive in bursts.** In a nine-break cycle they fell at 84.8 s,
93.3 s and 111.6 s - two or three consecutive quanta each time, tens of seconds
apart, always at full level. That is not a sawtooth and not geometry: it is
something outside stopping the pipeline for a moment.

The telemetry from the same cycles names it. `max_completion_gap_ns` reaching
3.34 ms with `max_missing_drains` at six means USB service stopped for six drain
periods together. The ring stayed full and nothing was lost - the driver
survives it - but capture is physically interrupted for those milliseconds and
the loopback records exactly that.

So the breaks are service gaps, not admission or geometry. Which is also why
every configuration showed roughly the same count once the alarm was off, and
why chasing them through the admission rules produced nothing.

## What removed the last lost quantum is not established

Two candidates were tested by building without each: the credit charge on entry
to the holding slot, and the wait against the target. Both builds still show
zero lost quanta, so neither is the cause. Either something earlier did it -
holding a refused block rather than discarding it is the obvious candidate - or
the loss depended on conditions that are currently absent, since the machine had
just been quietened.

Recorded as unresolved rather than credited to whichever change happened to be
in the tree when it stopped reproducing. The attribution was nearly made on that
basis, and it would have been wrong.

## Disturbance is now injected, not waited for

The natural loss disappeared when the machine was quietened, which left nothing
to test against: a passing run would only have meant the disturbance was absent.
Two stalls are now injected deliberately, once each, a second into the steady
window, and they are kept separate because they are different faults. A render
stall delays the producer while USB keeps draining - the case the holding slot
exists for. A service stall stops completions being processed, so the ring
stops draining and capture URBs are not resubmitted.

Four milliseconds of each, three times the quantum period and the length seen in
the wild:

| stall | lost | held | work overruns | service gaps |
|---|---:|---:|---:|---:|
| service 4 ms | 0 | 4 | 0 | 93 |
| render 4 ms | 0 | 3 | 1 | 121 |

No frames lost under either. That is the first evidence for the pipeline
surviving this rather than an absence of evidence against it.

The separated counter earns its place in the same table. A render stall is work
overrunning its period and is counted; a service stall is the stream's own clock
pacing us and is not. The old counter would have flagged both, and it is the one
runs were being failed on.

`service_gaps` is also new and immediately useful: 93 to 121 pauses longer than
two transfer periods in a single minute. The maximum gap was visible before, so
the picture looked like one rare stall; the frequency says the bus is
interrupted constantly and the pipeline absorbs it.

## The instrument disturbs the thing it measures

The worst service pause was traced to what was outstanding when it happened:
five transfers in flight, nothing deferred, ring full. The driver had done
everything it could and simply was not scheduled - our libusb event thread was
preempted, which already runs pinned to the big cores at nice -19 after Android
refuses SCHED_FIFO.

Then the same configuration measured with instrumentation off:

| | worst gap | gaps/min | breaks |
|---|---:|---:|---:|
| all detectors + recorder | 6.22 ms | 22 | 3 |
| transfer detector off | 2.87 ms | 166 | 1 |
| everything off | 1.46 ms | 20 | - |

The worst gap falls fourfold when the instruments are off, and halves when just
the transfer-continuity detector is. That detector runs inside the USB
completion callback, unpacking and comparing every frame of every drain - 48000
frames a second on the thread whose timeliness everything depends on. It is now
off unless asked for.

The gap *count* does not follow the same order, and no explanation is offered
for that here: it varies from 20 to 218 between runs of identical
configurations, so a single measurement cannot separate it from the room. Only
the worst-gap figure is claimed.

What this costs retroactively: every service-gap and capture-break number
recorded today was taken with the expensive detector on. Comparisons between
arms remain valid, since all arms paid the same tax, but the absolute picture -
"the bus is interrupted constantly" - was partly us interrupting it.
