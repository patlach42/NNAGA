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

## Stability qualification

One configuration - target 256, five transfers, credit with a 64 frame reserve
- run twelve times for two minutes each, with a deliberate 4 ms stall injected
on two cycles in every three, and the expensive detector off so the instrument
is not the disturbance. The pass criterion was stated before the run: zero lost
quanta across all twelve, including the eight disturbed ones.

| injected | cycles | lost | held (median) | capture breaks | worst gap |
|---|---:|---:|---:|---:|---:|
| none | 4 | 0 | 4 | 15 | 6.38 ms |
| render 4 ms | 4 | 0 | 3 | 5 | 4.09 ms |
| service 4 ms | 4 | 0 | 4 | 20 | 4.50 ms |

Zero lost frames in twenty-four minutes of continuous audio. The holding slot
engaged three or four times per cycle, so it remains the exception path it was
meant to be rather than the pipeline.

Two things the table says that the verdict column does not.

A service stall costs four times the capture breaks of a render stall of the
same length - twenty against five. That asymmetry is structural: when USB
servicing stops, capture URBs are not resubmitted and the input simply does not
arrive, and no amount of buffering recovers samples that were never delivered.
The holding slot protects the output path and cannot protect this one.

The worst pause of the whole run, 6.38 ms, happened in a cycle with nothing
injected. The environment produces disturbances larger than the ones being
injected deliberately, which makes the qualification stricter than designed and
confirms once more that the source is outside the driver.

Most cycles are marked FAIL, on the capture-break gate. That gate was left as it
is rather than softened to fit the result: it correctly reports that the input
signal was interrupted. But an interrupted input and a failing pipeline are
different things, and the second did not happen once.

## Two disturbances, opposite resources

The stability qualification injected a render stall and a service stall and
reported that both were survived. Sweeping the geometry underneath them says
why they have to be treated as different faults rather than two samples of one.

A **render stall** delays the producer while USB keeps draining. What covers it
is stock: frames already written and not yet played. A **service stall** stops
completions, so the ring stops draining while the producer keeps rendering.
What covers it is room: free frames between the working level and the admission
ceiling. Deepening the queue buys the first and spends the second.

Measured on the reference device with four milliseconds injected, one stall per
cycle, three cycles per arm, at 48 kHz and four channels:

| target | admission | headroom | output latency | render 4 ms | service 4 ms |
|---:|---|---:|---:|---|---|
| 64 | credit 32 | auto 208 | 3.75 ms | starved | held |
| 96 | credit 32 | auto 208 | 4.08 ms | starved | held |
| 128 | credit 32 | auto 208 | 5.42 ms | held | - |
| 256 | wait | auto 208 | 7.92 ms | - | **2 quanta lost** |
| 256 | wait | 416 | 9.25 ms | - | held |

The deep queue is the one that fails the service stall, and it fails it for a
reason the shallow queues do not have: sitting near its ceiling, it has nowhere
to put what the producer keeps rendering. Raising the headroom to 416 frames
fixes it outright - zero lost, admission margin from 0 to 256 frames, held
quanta from 939 to none - and changes nothing about the stall itself.

## Output latency is exactly render-stall survival

Stock is what is in the ring plus what has already been submitted to USB, and
that sum is the output latency. So the survivable render stall and the latency
are not two numbers to trade off against each other: they are the same number.

The boundary falls exactly where the arithmetic puts it. Output latency 3.75 ms
and 4.08 ms both starve on a 4 ms stall; 5.42 ms holds. At the designed optimum
- quantum 32, target 128, credit with a 32 frame reserve, headroom 416 - output
latency is 6.75 ms: it holds 4 ms of either kind and starves on 8 ms of render.

This bounds the whole exercise. No admission rule, quantum or transfer count
can survive a producer stall longer than the audio it is holding, so the
minimum latency that is stable in given conditions is the worst render-side
preemption those conditions produce. Geometry below that bound does not buy
latency, it buys dropouts.

Room behaves differently and is nearly free - it costs ring memory, not delay -
but only under credit admission. Waiting for room lets the producer float up to
the ceiling, so headroom turns into occupancy: the same 256 target measured
7.92 ms of output latency at headroom 208 and 9.25 ms at 416. Credit pins the
working level near the target and leaves the headroom as what it is for.

## The scheduler levers that are actually available

Checked on the reference device rather than assumed: `su` is absent and the
app's `RLIMIT_RTPRIO` is 0, so `SCHED_FIFO` cannot be granted and the fallback
to `nice -19` on the big cores is not a preference but the ceiling. The ADPF
session already carries both the render and the libusb event thread, and has
since the calibrated USB path was hardened - the remaining gap there was that
it bound their thread ids once and never rebound them, which is now fixed.

## The envelope that sets the floor

Eight minutes of continuous duplex audio, eight cycles of one minute, nothing
injected, on a queue deep enough that nothing could fail and the numbers
describe the room rather than a collapse. Wi-Fi left up, because the question
is what holds in the conditions the device is actually used in.

Reported per cycle, which is the only way it means anything:

| cycle | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| render lateness, ms | 5.42 | 1.79 | 2.93 | 1.44 | 1.55 | 2.18 | 1.52 | 2.28 |

The first cycle is the session starting, not the environment: 5.42 ms against a
2.93 ms worst among the other seven. Pooling them produced a figure nearly
twice the steady one, and an earlier draft of this section did exactly that.
The steady envelope with no interface open is under 3 ms; zero quanta were lost
across the whole run, and the worst service gap was 5.08 ms with 283 gaps
counted.

Read against the bound above, an output latency near 3 ms covers the steady
envelope of this sample. That is a sample, not the device: it is eight minutes
on one machine with one set of things running.

## Affinity was never applied at all

Both audio threads ask for the two top-ranked CPUs when they start, and the
main thread asks to be kept off them. Read back from `/proc/<tid>/status`
while a session was running, every thread in the process - `UsbAudioRender`
and `UsbIsoEvents` included - reported `Cpus_allowed_list: 0-7`. That held with
the interface in front and with it closed, which ruled out the first guess that
a cpuset transition was overwriting the mask.

The cause is two faults in the same three lines, and the first hid the second.

`applyCurrentThreadAudioAffinity` and its UI twin read the current mask with
the raw `sched_getaffinity` system call and test the result against zero. The
libc wrapper returns zero on success; the raw call returns **the number of
bytes it copied**. So the test was true on every successful call and both
functions returned before setting anything.

Correcting that test exposed the second fault. The raw call writes only those
bytes - eight of them on this machine - and leaves the rest of a 128 byte
`cpu_set_t` holding whatever was on the stack. Measured directly: the call
returns 8, `CPU_COUNT` on the result reads 47 on a 16 CPU host, the derived
mask therefore names CPUs that do not exist, and `sched_setaffinity` refuses it
with `EINVAL`. Zeroing the set before the call fixes it, and the mask then
narrows as designed.

So the pinning has never been in force, on any device, for either thread - and
the fallback path around it, `nice -19` after Android refuses `SCHED_FIFO`, is
all that has ever been running. The mask helpers had unit tests throughout;
what had no test was the code that applies them, which is why a function that
always returned early looked healthy for as long as it did. There is one now,
and it fails on the old code.

What this is worth in latency is not yet measured: the fix is in the tree and
was not on the device when the envelopes above were taken.

## What an open interface costs

The same eight-minute measurement, same geometry, with the app's own interface
brought to the front and left there. The activity is started a few seconds into
the run rather than before it, because bringing it up first makes
`am instrument` restart the process; the cycle it lands in therefore carries
the activity launch and the first composition, and is reported but not read as
steady state.

| render lateness, ms | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| interface closed | 5.42 | 1.79 | 2.93 | 1.44 | 1.55 | 2.18 | 1.52 | 2.28 |
| interface open | 10.13 | 6.55 | 2.72 | 3.12 | 3.40 | 4.82 | 4.32 | 4.17 |

Discarding the launch cycle from each, the steady envelope goes from 2.93 ms to
4.82 ms - and the second cycle of the open run still reads 6.55 ms, so the
interface is not fully settled a minute in either. Service gaps counted rose
from 283 to 505 and the worst from 5.08 ms to 6.38 ms.

An open interface therefore costs roughly a factor of two on both envelopes,
which by the bound above is a factor of two on the minimum latency that holds.
This is a driver whose latency depends on whether its own window is visible.

The mechanism is not the garbage collector. Both audio threads are plain
`std::thread`s: there is no `AttachCurrentThread` anywhere in the native code
and no `JNIEnv` in the engine at all, so ART never suspends them for a
collection. Moving the engine to its own process would therefore buy nothing on
that account, and would risk losing the top-app cpuset the process currently
sits in while its activity is in front. What the interface costs is CPU
contention and scheduler placement, which a process boundary does not remove.

## Pinning the USB service thread

With the affinity calls finally reaching the kernel, where to put the two
threads became a real question rather than a dormant one. The comment on the
mask helper had said since it was written that the render and event threads
need separate performance cores; nothing had ever enforced it, because both
threads were handed the same two-core pool and the placement left to the
scheduler.

Five arrangements, same geometry, interface open, five cycles each, first cycle
discarded as the session starting:

| arrangement | worst render lateness | worst USB gap | gaps per cycle | verdicts |
|---|---:|---:|---:|---|
| not pinned (the broken state) | 4.82 ms | 5.65 ms | 290-505 | clean |
| both threads on {6,7} | 5.22 ms | 7.79 ms | 116-176 | clean |
| render {7}, service {6} | 6.70 ms | 4.45 ms | - | 2 cycles starved |
| service {5}, render {6,7} | 5.14 ms | 8.30 ms | ~1900 | every cycle failed |
| **service {6}, render {6,7}** | 6.20 ms | 5.33 ms | **36-85** | 4 of 5 clean |

Two things fell out of this that were not guesses beforehand.

**The core below the pool is not a spare core.** This device runs six cores at
3.63 GHz and two at 4.61. Moving USB servicing to the fastest of the slow six
took the gap count from around 150 a cycle to around 1900 and failed every
cycle. Whatever else servicing needs, it needs to stay on the fast pair.

**The two threads should not be symmetric.** Giving each an exclusive core cut
the worst gap to its best figure of the five, and cost the render thread the
ability to migrate: it starved in two cycles out of five. Holding servicing to
one core of the pair while the render thread keeps both is what worked - the
gap count fell about fivefold against the unpinned state, nothing starved, and
four of five cycles were clean.

The asymmetry follows from what each thread can recover from. A completion the
device is waiting on cannot be made up later; a late graph cycle is exactly
what the queue is holding audio for. So servicing gets a core it never has to
leave, and the render thread keeps somewhere to go.

What is not claimed: that this lowers the latency floor. Render lateness reads
higher in the winning arrangement than in the unpinned one, 6.20 ms against
4.82 ms, and that difference is inside the run-to-run scatter seen repeatedly
here. The gap count difference is an order of magnitude and is not. So the
claim is about service gaps and starvation, and about those only.

## The lateness column in that table does not mean what it says

Reviewed with omp, and two of the objections stand.

**`max_scheduler_lateness_ns` is not scheduler lateness.** The render loop
records `finished - (began + quantumPeriod)`, and `began` is before the capture
wait. The maximum therefore folds in capture pacing, descheduling, DSP, the
credit wait, the room wait and publication. It is a cycle overrun, and naming
it after one of its terms invited exactly the reading it got here. Scheduler
latency would be the time between becoming runnable and running, which is not
measured at all.

**The two sides were not even the same epoch.** The harness resets the USB
envelope at the warmup boundary; nothing reset the engine's own maxima, which
were zeroed only when a session started. So the render figures covered startup
and steady state together while the USB figures covered steady state alone -
which is why the first cycle of every run read worst, and why the correction
for that had to be applied by hand. Both sides are now reset at the same
boundary.

**And the unpinned arm was not a control.** The same syscall fix that let audio
affinity reach the kernel also let the UI affinity call in `MainActivity`
reach it for the first time. So "unpinned" against "pinned" compares no
affinity at all against audio affinity *and* UI affinity together, and cannot
attribute the difference to either. Separating them needs one binary with both
switchable at runtime, which does not exist yet.

What survives all three objections is the one number none of them touch.

## The graph must not be allowed onto the slow cores

`peak_dsp_ns` measures the graph's own execution time and nothing else - no
waits, no pacing, no publication. Across five runs and three placements of the
render thread, with the interface open and everything else held:

| render thread allowed on | peak graph cycle |
|---|---:|
| the fast pair {6,7} | 120 us, 143 us |
| the slow six {0-5} | 3596 us |
| everything {0-7}, unpinned | 3273 us, 3444 us |

Leaving the thread unpinned is not neutral: it produces the same result as
pinning it to the slow cores, because that is where the platform puts it. The
peak cycle rises about twenty-five fold, to roughly 3.4 ms against a 1.33 ms
budget, and every cycle in those runs missed its deadline.

The average says nothing here - a typical cycle costs about ten microseconds
either way. It is the peak that has to fit in the quantum, and on the slow
cluster it does not.

This is the one placement result that does not rest on the disputed metric, and
it is the opposite of what the platform's general advice would suggest. It also
bounds what "don't set affinity" can mean for this driver: the graph needs the
fast pair, whatever is decided about servicing.

## The peak graph cycle was never the graph working

`peak_dsp_ns` brackets the graph with a wall clock, so it charges the block for
time the thread was not running. Reading the thread's own CPU clock over the
same span separates the two, and both are vDSO reads, so the render thread pays
almost nothing for the answer. The difference - wall minus CPU - is the block
being descheduled mid-flight.

Same geometry, interface open, five cycles, first discarded:

| | peak graph block | of which off-CPU | CPU actually used |
|---|---:|---:|---:|
| render on the fast pair, servicing on one of it | 87-198 us | 77-180 us | ~20 us |
| no affinity at all | 752-1738 us | 707-1713 us | ~30 us |

Between 88% and 98% of the figure is preemption. The graph's own work is a few
tens of microseconds either way and never came close to the 1.33 ms budget.

This corrects the reading given earlier in this file, that the graph "has
throughput to spare on average and none at the peak". It has throughput to
spare at the peak too. What it does not have, unpinned, is uninterrupted
possession of a core: the same block takes ten times longer because it is
descheduled ten times more, not because the cores it lands on are slower.

The practical conclusion does not change - the graph belongs on the fast pair -
but the quantity to minimise does. Off-CPU time inside the block is the thing,
and it is now measured directly instead of being inferred from a number that
mixed it with work.

## Which half of the affinity fix did the work

The syscall fix enabled audio affinity and UI affinity together, so nothing
measured before could attribute anything to either. With both switchable at
runtime in one binary, four arms in randomised order, four cycles each, first
discarded:

| audio affinity | UI affinity | service gaps per cycle |
|---|---|---:|
| off | off | 330-441 |
| off | on | 400-453 |
| on | off | 99-135 |
| on | on | 70-104 |

Audio affinity is what cuts the gap count, by roughly a factor of four. UI
affinity on its own does nothing measurable - if anything the arm reads
slightly worse than no affinity at all - but on top of audio affinity it takes
another quarter off.

The arms in this square ran with the render thread unpinned, which the section
above shows is the wrong place for it, so the square settles the attribution
question and not the placement one.

## The interface has to be open before the audio starts

Raising the activity a few seconds into a run put the app switch and the first
composition inside the measured window, and it was audible. The test now raises
it itself, after the probe has claimed the USB interface and before any session
starts, and waits for it to settle. Raising it before the probe was tried and
Android then refused to open the interface, so the order is: claim the device,
open the window, start the audio.

Evidence rather than assumption: the runner samples the resumed activity while
it measures. The runs below report the app in front for eighteen samples out of
nineteen, the odd one being the moment it was raised.

## Depth against starvation, with affinity working

Quantum 32, five transfers, credit with a 32 frame reserve, headroom 416,
interface open throughout, four cycles of 45 s per depth, first cycle included
because it no longer carries the app switch.

| target | ring, median | output latency | cycles starved | quanta lost |
|---:|---:|---:|---:|---:|
| 96 | 124-148 | 5.4 ms | 4 of 4 | 0 |
| 128 | 188-212 | 6.6 ms | 1 of 4 | 0 |
| 160 | 220-260 | 7.4 ms | 1 of 4 | 0 |
| 192 | 244-268 | 8.1 ms | 1 of 4 | 0 |

Not one rendered block was lost at any depth: the holding slot did its job
everywhere. What fails is the other end - the device pulling from a ring that
has nothing in it - and that is what the depth has to cover.

Below 128 frames of target it fails every cycle. Above it the rate falls to
about one cycle in four and stops improving with depth, which says the
remaining failures are not a depth problem: the cycles that starve at 160 and
192 are the ones carrying a 4.0 and 4.6 ms off-CPU spike, and no queue this
side of ten milliseconds covers a spike that lands badly.

So the honest reading is a floor around 6.6 ms of output latency for this
device with its own interface open, and a residue of roughly one disturbed
cycle in four that is the environment rather than the geometry. Whether that
residue is acceptable is a product decision, not a measurement.

## The signal the driver was giving ADPF

The hint session reports `dspNs` as the work the period took. That figure is the
graph's CPU cost, about ten microseconds against a target of thirteen hundred,
so the system has been told for as long as this has existed that the workload
finishes in under one percent of its deadline. Which is true, and is also an
invitation to place it on a slow core and clock it down - the placement this
file has spent several sections working around.

API 35 has the call that says both things: a work period's wall time and the
CPU time inside it, reported separately. It is now implemented and selectable,
along with reporting nothing at all, so the three can be compared.

At target 128, four cycles each, one run apiece:

| ADPF signal | cycles starved | worst off-CPU block |
|---|---:|---:|
| off | 1 of 4 | 0.27 ms |
| CPU cost only, as before | 1 of 4 | 0.41 ms |
| wall and CPU separately | 2 of 4 | 1.52 ms |

Nothing is separated at this sample size - one starved cycle against two is
inside the scatter every arm in this file has shown. The default therefore
stays on the old signal, which is what every other measurement here was taken
with, and the new path stays available behind an argument. The argument for it
remains a good one and is still unmeasured; a default is not the place for a
hypothesis.

## The interface's own cadence is most of the tail

Two pieces of periodic work run in the process that owns the render thread: the
rack polls four JNI meters every 17 ms, and the transport display writes Compose
state on every display frame, which on this 120 Hz panel is 120 state changes a
second. Both are now settable, so their cost is measurable rather than
arguable.

Quantum 32, target 128, interface open, four cycles of 45 s per run, the arms
run back to back and repeated once each. Worst off-CPU inside a graph block,
per cycle:

| interface cadence | cycle 1 | 2 | 3 | 4 | starvation events |
|---|---:|---:|---:|---:|---:|
| meters 66 ms, no frame clock | 154 | 111 | 153 | 122 | 1 |
| meters 66 ms, no frame clock | 229 | 81 | 471 | 72 | 1 |
| meters 17 ms, frame clock on | 1625 | 2052 | 150 | 157 | 2 |
| meters 17 ms, frame clock on | 70 | 79 | 262 | 3284 | 4 |

Microseconds. No quiet cycle went above 0.5 ms; three of the eight loud cycles
went above 1.6 ms, one to 3.3 ms. Starvation events follow: two across the
quiet runs against six across the loud ones.

The depth sweep above showed the residual failures were the cycles carrying a
multi-millisecond off-CPU spike, and this is where a large part of those spikes
come from. It is the app's own interface, not the system, and it is the first
lever in this file that acts on the tail rather than absorbing it.

Varying them separately says which one it is, and it is not the meters.

| meters | frame clock | worst off-CPU per cycle, us | worst |
|---|---|---|---:|
| 66 ms | off | 154, 111, 153, 122 / 229, 81, 471, 72 | 471 |
| 17 ms | off | 280, 136, 223, 225 | 280 |
| 66 ms | on | 406, 3094, 1511, 1882 | 3094 |
| 17 ms | on | 1625, 2052, 150, 157 / 70, 79, 262, 3284 | 3284 |

Twelve cycles with the frame clock off, and not one above 0.5 ms. Twelve with
it on, and six above 1.5 ms. The meter cadence makes no difference either way:
sixty-hertz meters with the frame clock off are as quiet as fifteen-hertz ones.

So the cost is the per-frame Compose state write, not the JNI polling, and the
fix is correspondingly narrow. What the frame clock buys is a transport readout
that moves smoothly between snapshots rather than stepping; it does not need a
display frame to do that. Driving the same extrapolation from a timer at ten
or fifteen hertz would keep a readout that reads seconds looking continuous
while writing state a tenth as often, and the meters can stay where they are.

That change is not made here: it is a change to how the interface looks, and it
belongs to whoever owns that. What is established is which of the two candidates
costs anything, and that it costs several milliseconds of render-thread tail.

## Driving the transport readout from a timer instead of the frame clock

The readout the frame clock feeds shows whole seconds - `formatElapsedTime`
truncates with `toLong()` - and bars, beats and sixteenths, which at 240 BPM
change sixteen times a second. It was being advanced 120 times a second. Around
119 of every 120 updates changed nothing anyone could see.

So this is not the product trade-off the previous section left open. The same
extrapolation now runs from a 33 ms timer, above both readouts with room to
spare, and the per-frame path is kept only so the measurement that condemned it
can be reproduced. Same geometry, target 128, meters left at their usual 17 ms,
interface open, four cycles a run:

| transport clock | cycles starved, of 8 | worst off-CPU |
|---|---:|---:|
| every display frame | 6 | 3.28 ms |
| off entirely | 2 (of 12) | 0.47 ms |
| 33 ms timer | 1 | 1.74 ms, and 0.32 ms in the other seven |

One run of the timer arm was the first fully clean run at this depth in the
whole exercise: four cycles, no starvation, nothing to report. The other had a
single starved cycle.

So the timer keeps the feature and gets the quiet: it is as good as removing
the readout's motion altogether, and it costs nothing visible. The meters were
never the problem and are untouched at sixty hertz.

## What the transport fix did to the floor

The depth sweep before the fix put the floor at a target of 128 frames, because
96 starved in every one of its four cycles. Repeated with the transport readout
on its timer and nothing else changed, two runs of four cycles at each depth:

| target | ring, median | output latency | cycles starved |
|---:|---:|---:|---|
| 64 | 100-148 | 4.6-5.6 ms | 4 of 4 |
| 96 | 148-188 | 5.6-6.4 ms | 4 of 8 |
| 128 | 180-220 | 6.3-7.1 ms | 1 of 8 |

The first run at 96 starved in only one cycle of four and it was tempting to
call the floor moved. The second starved in three, so the pair reads 4 of 8 and
the floor has not moved: 128 is still where it holds. Recorded because the
single run was written up here as progress before the repeat contradicted it,
and one run at this scatter has now misled twice.

What the fix did change is the character of the failures. At 64 the off-CPU
figures are 75 to 203 microseconds - nothing is being descheduled, the queue is
simply too shallow for the device, which is a geometry limit rather than an
interference one. Before the fix the failures at every depth carried
multi-millisecond spikes.

So: floor still at a target of 128, about 6.6 ms of output latency, and the
remaining failures at that depth are one cycle in eight rather than one in
four.

## The submitted runway is not slack

Of the 6.6 ms at the floor, 2.5 ms is frames already handed to USB - five
transfers of twenty-four. The obvious next millisecond is to submit four
instead. Two runs of four cycles, target 128, everything else unchanged:

| transfers | submitted | output latency | cycles starved |
|---:|---:|---:|---|
| 5 | 120 frames, 2.5 ms | 6.3-7.1 ms | 1 of 8 |
| 4 | 96 frames, 2.0 ms | 5.4-5.8 ms | 7 of 8 |

Half a millisecond of runway costs seven starved cycles against one. And it is
not interference doing it: the off-CPU figures across the second run are 71, 85,
890 and 80 microseconds, so nothing was being descheduled - the device simply
ran out of submitted frames before the servicing thread put more in.

That is the asymmetry between the two halves of the latency. Ring frames cover
the render thread being late; submitted frames cover the *servicing* thread
being late, and they are the only thing that does. Servicing is interrupted
tens of times a cycle even in the quiet runs, so the runway is doing continuous
work and has no spare depth in it.

Which closes the obvious route down from 6.6 ms. Latency here is two reserves
against two different faults, and neither has slack at this point: the ring
starves at 96 frames of target, the runway starves at four transfers. Going
lower means making the servicing thread late less often, not holding fewer
frames against it.

## Servicing is not preempted mid-work; it is waiting to run

The render thread has had an off-CPU figure for a while. Servicing now has two.

The first brackets the completion callback with the same wall-minus-CPU
subtraction. Across four cycles it reads 34, 75, 203 and 56 microseconds, while
the gap between successive callbacks in the same cycles reaches 3.4 to 6.9
milliseconds. So the callback is not being descheduled halfway through
resubmitting a transfer - whatever costs those milliseconds happens before the
callback starts, not inside it.

The second reads the thread's cumulative runqueue wait from
`/proc/self/task/<tid>/schedstat`, sampled once a second from the policy thread
and never from an audio thread. Over a 45 second cycle it accumulates 649 to
959 ms - between one and a half and two percent of wall time spent runnable and
not running. At roughly two thousand completions a second that averages about
eight microseconds a wakeup.

What this does and does not establish. It establishes that the servicing thread
spends a real fraction of its life waiting for a CPU, on a device where it is
pinned to a 4.6 GHz core and nothing else of ours may run there. It does not
attribute the worst gaps: forty gaps of three milliseconds would be 120 ms,
which fits inside the 700 ms total with room to spare, and so would an even
spread of eight microseconds across ninety thousand wakeups. Cumulative
schedstat cannot tell those apart, and the maximum single wait is not exposed
by the kernel here.

Separating them needs per-wakeup evidence - `sched_wakeup` against
`sched_switch` for that one tid, which Perfetto can collect on this device for
a short diagnostic run. That has not been done.

## The completions are ready and nobody collects them

A normal wakeup of the event loop collects one completion. Counting how many
one wakeup actually collects, and recording the worst, answers what the gap
alone could not.

| cycle | completions collected by one wakeup | worst gap | callback off-CPU |
|---:|---:|---:|---:|
| 1 | 5 | 4.5 ms | 43 us |
| 2 | 5 | 6.9 ms | 66 us |
| 3 | 7 | 5.9 ms | 99 us |
| 4 | 7 | 4.1 ms | 88 us |

Five to seven at once, against a steady state of one. So during those
milliseconds the device delivered the whole set of transfers in flight and they
sat completed until something came back to take them. The bus was not quiet;
the servicing thread was not there.

Together with the callback's own off-CPU time - tens of microseconds while the
gaps are milliseconds - that places the whole cost before the callback runs.
The thread is not descheduled mid-work; it is not run at all for several
milliseconds while work is waiting.

This also explains why cutting the runway to four transfers failed so badly.
The gaps consume the entire set in flight, so the depth of that set is exactly
how long the device can be left alone. Five transfers survive a gap that takes
five; four do not.

Getting under the current floor therefore means making those wakeups happen,
not holding more frames against them missing. What is left to establish is
whether the thread is runnable and unscheduled or not woken at all, which needs
`sched_wakeup` against `sched_switch` for that tid.

Found on the way: the event loop existed as two identical copies, one in
`ensureEventThread` and one inside the implicit-feedback capture path, and this
device runs the second. The first attempt at this measurement instrumented the
first and reported zero. They are now one function.

## Runnable, and not given a CPU

The event loop now reads its own thread's cumulative runqueue wait across each
iteration, and records the pair for the longest iteration that collected more
than one completion - that is, for the worst gap.

| cycle | collected | iteration | of which runqueue wait |
|---:|---:|---:|---:|
| 1 | 6 | 4.99 ms | 4.45 ms |
| 2 | 6 | 3.01 ms | 2.34 ms |
| 3 | 6 | 4.95 ms | 4.43 ms |
| 4 | 5 | 7.01 ms | 6.58 ms |

Eighty-nine to ninety-four percent of the gap is the thread sitting on a
runqueue. It was woken. It was not run.

So the remaining question from the previous section is answered, and answered
the less convenient way: this is not a wakeup that fails to arrive, it is a
scheduler that does not dispatch. On a thread pinned to a 4.6 GHz prime core,
at nice -19, in a process holding the top-app cpuset, with an ADPF session
naming it.

Which raises the obvious suspect. Servicing is held to core 6, and the render
thread is allowed on 6 and 7. The two are the only threads of ours that may run
there, and they are both nice -19. When measurement first tried giving them a
core each, the split produced the best service gap of any arrangement - 4.45 ms
against 7.79 for the shared pair - and was rejected because the render thread,
left with one core, starved. The render thread's own tail has since fallen by
an order of magnitude with the transport clock fixed, so that trade may no
longer be the one it was.

## It is not our render thread holding the core

If the servicing thread waits on a runqueue for milliseconds, the first suspect
is the only other thread of ours allowed there. Giving each an exclusive core
tests it directly.

| arrangement | runqueue wait at the worst gap | starvation events per cycle |
|---|---:|---|
| service {6}, render {6,7} | 4.4-6.6 ms | 0-1 across 8 cycles |
| service {6} exclusive, render {7} | 3.9-4.0 ms | 25 to 52 |
| service {6,7}, render {6,7} | 2.4-12.8 ms | 0 across 8 cycles |

Moving the render thread off core 6 barely moved the servicing thread's
runqueue wait - 3.9 ms against 4.4 - so the render thread was not what it was
waiting behind. Whatever occupies that core for milliseconds at a time is not
in this process, and without root there is nothing here that can move it.

The split also collapsed for the reason it collapsed before: a render thread
with one core and nowhere to migrate starves the device tens of times a cycle.

Letting servicing use both prime cores is at least as good as holding it to
one, and one of its cycles absorbed a 13.3 ms gap - 12.8 ms of it runqueue wait
- without starving anything. Eight cycles each, no starvation either way, so
the two are not separated by this and the default is left where it is. What is
separated is the split, which is worse than both by a factor of thirty.

## The core it waits for is idle

If the servicing thread waits on a runqueue for milliseconds, something should
be occupying the core. Per-CPU time over a run says otherwise.

| cpu | user | system | irq | softirq | busy |
|---:|---:|---:|---:|---:|---:|
| 6 | 374 | 2221 | 131 | 1 | 17.2% |
| 7 | 368 | 133 | 59 | 0 | 3.6% |
| 0 | 2694 | 1916 | 900 | 410 | 39.3% |
| 1 | 1463 | 762 | 1093 | 749 | 26.3% |

Core 6 is eighty-three percent idle. Its system time dwarfs its user time and
almost certainly is the servicing thread itself - reaping URBs is an ioctl, so
our own work on that core is kernel time, and two thousand wakeups a second at
seventy microseconds each comes to about what is there. Interrupt work lives on
cores 0 and 1, not here.

Nor is the process being held back by a bandwidth limit: the top-app cpu group
has `cfs_quota_us` of -1, `nr_throttled` of zero, `uclamp.max` of max, and a
cpuset of 0-7.

So the measurements disagree with each other in a way this file cannot resolve.
The thread is runnable for milliseconds; the core it is pinned to is idle; no
quota is throttling it; the only other thread of ours allowed there was moved
away and it changed nothing. Either the runqueue accounting means something
narrower than it appears to, or the platform's scheduler is not dispatching a
runnable thread onto an idle core it is affine to.

That is where an application-level investigation ends. Separating those two
needs `sched_wakeup` against `sched_switch` for the tid, which Perfetto can
collect here, and it has not been done.

## Core control was parking the core it was pinned to

`/sys/devices/system/cpu/cpu6/core_ctl` on this device, with audio idle:

```
enable 1        active_cpus 1     need_cpus 1
min_cpus 1      max_cpus 2        offline_delay_ms 100
busy_up_thres 60                  busy_down_thres 30
CPU: 6  Online: 1  Paused: 0
CPU: 7  Online: 1  Paused: 1
```

Core control keeps one of the two prime cores parked unless the cluster passes
sixty percent busy. Audio holds it at about seventeen. A parked core is online
and runs nothing, and it is not always the same one.

That is the whole contradiction resolved. A thread pinned to one prime core is
runnable with nowhere to run every time core control parks that particular
core; the core reads idle in `/proc/stat` because parked time is idle time; no
quota is involved; and moving the other thread away could not help, because the
other thread was never the obstacle.

It also explains the two results that made no sense next to each other. Giving
servicing both prime cores removed starvation entirely across eight cycles - it
can use whichever one is awake. Giving each thread its own core was the worst
arrangement measured, thirty times worse than either, because then both threads
are pinned to single cores and one of those is parked at any moment.

The rule that follows is narrow and portable: pin to a cluster, never to a
core. A single-core mask is a bet that the platform will keep that core
running, and this platform explicitly does not. The default is changed
accordingly.

Worth noting what this cost to find. The single-core placement looked best in
the arm that first compared them, and the reasoning for it - servicing does
little work and wants one fast core with no migrations - was sound and wrong.
It took the callback-collection count to show the completions were piling up,
the thread's own runqueue wait to show it was runnable through them, and the
per-CPU breakdown to show the core was idle, before the contradiction was sharp
enough to point at the platform rather than at us.

## Widening past the prime cluster does not help either

If core control leaves only one prime core awake and two audio threads want it,
the obvious next move is to let them use the fastest core below the cluster as
well. Measured, servicing on cpus 5-7 instead of 6-7:

| servicing allowed on | service gaps per cycle | worst gap |
|---|---:|---:|
| 6-7 | 27-55 | 4.4-6.9 ms |
| 5-7 | 962-1179 | 7.0-7.2 ms |

Twenty times the gaps. This agrees with the earlier arm that pinned servicing to
core 5 outright and saw about 1900 gaps a cycle: the mid cluster at 3.63 GHz is
not a place USB servicing can live, and giving it the option is the same as
sending it there.

So the prime cluster is both the floor and the ceiling for these threads: a
single core of it is a trap because core control parks cores, and anything
outside it is too slow. The residual runqueue wait - four to six milliseconds
at the worst gap - is what it costs to have two latency-critical threads and,
much of the time, one awake core to run them on. Nothing available from an
application changes that.

## The admission policy the app was actually running

Credit admission has been selectable since it was added, and only the stress
harness ever selected it. `nativeSetDirectUsbAdmissionPolicy` and its reserve
had no caller outside the test, so every ordinary launch of the app ran
wait-for-room - the policy measurement stopped recommending as soon as the two
were compared, and the one that spends the write headroom on latency rather
than leaving it as room.

Both are now read from settings at session start, defaulting to credit with a
32 frame reserve, which is what every arm in this file used. Verified through
the ordinary launch rather than the harness: the app's own diagnostic reports
`ring(playback=291)` against a target of 256, which is the target plus the
reserve. Under wait-for-room the same geometry floats toward target plus
headroom, 464.

The affinity default arrives the same way and was checked in the same run: both
audio threads report cpus 6-7.

Worth naming the shape of this mistake, because it is the second of its kind
here. A knob that only a test sets is a knob the product does not have, and
both times the measurements were sound while the thing measured was not what
shipped.

## Two things closed by measuring them

**The statistics poll costs nothing.** The rack refreshes stats, transport and
clip slots every 200 ms, and it does far more per turn than the meters do. Made
settable and compared at 200 ms against 2000 ms, four cycles each: worst
off-CPU per cycle 214, 2212, 187, 252 microseconds against 1838, 253, 304, 279.
One outlier apiece and nothing between them; no starvation either way. The
cadence stays.

That is what the meters showed too, and it sharpens the earlier result rather
than repeating it: sixty-hertz JNI polling is free and so is this, while a
per-display-frame Compose state write costs milliseconds. The cost was never
the polling rate. It was writing state on the frame callback.

**The startup click no longer reproduces.** Credit admission used to lose a
quantum during the fill phase - a held block whose wait for room expired - and
it appeared wherever admission was tight, on credit and on a small headroom
alike. Run now at the geometry the app ships with, quantum 64, multiplier 4,
headroom 208, credit with a 32 frame reserve: `first_loss_ring` reads -1 in
every cycle, no quantum drops, admission margin 112 to 120 frames.

No mechanism was found for it and none is claimed. What changed underneath it
is that the audio threads are now actually on the fast cluster and the
interface no longer takes milliseconds off the render thread, so the fill phase
finishes without a block waiting long enough to expire. Recorded as not
reproducing rather than as fixed, and worth re-checking if either of those
regresses.

## Ground truth from the scheduler

Perfetto records `sched_switch` and `sched_wakeup` on this device without root,
and trace_processor reads the result locally. Twenty seconds with audio running
at the current defaults, nothing else touched during the recording.

Per CPU over those twenty seconds:

| cpu | idle | busy | longest unbroken idle |
|---|---:|---:|---:|
| 4 | 16930 ms | 3022 ms | 55 ms |
| 5 | 16936 ms | 2985 ms | 44 ms |
| 6 | 19656 ms | **128 ms** | **508 ms** |
| 7 | 14854 ms | 4961 ms | 17 ms |

A core idle for 508 ms without interruption is not a quiet core, it is a core
taken out of service. Core control parks cpu6 for essentially the whole trace,
and both audio threads consequently live on cpu7: servicing ran 3143 ms there
against 103 ms on cpu6, the graph 1260 ms against 14 ms.

So the two latency-critical threads share one core because the platform only
offers one, and they wait for each other on it: 40055 runnable waits totalling
828 ms. During the longest of them the trace shows cpu6 sitting in `swapper`
and cpu7 held by the display's `crtc_commit` thread or by our own.

This also settles why each placement behaved as it did. Both threads on the
pool is the only arrangement that works because the pool has one usable core in
it; giving each an exclusive core pins one of them to a parked core and starves
the device; and moving the render thread away changed nothing because the two
were never on separate cores to begin with.

**A correction to the figures above.** The worst runnable wait in this clean
trace is 1.38 ms, and the mean across forty thousand waits is 20 microseconds.
An earlier trace, recorded while this session was polling the device over adb,
showed 6.4 ms - and the trace names the cause: `grep` and `dumpsys`, our own
monitoring, running on cpu6 during the worst wait. The four to six milliseconds
reported from schedstat in the sections above were measured the same way, with
adb polling alongside, and should be read as an upper bound that includes the
instrument. The mechanism is unchanged; its magnitude in an undisturbed system
is smaller than this file said.

## The audible breaks, and what silenced them

A listener reported breaks while the telemetry reported nothing. Both halves of
that turned out to be true, and the second was a defect in its own right.

**Nothing was listening.** All three signal detectors - capture discontinuity,
capture modulation, playback discontinuity - returned early unless the flight
recorder was armed, and the harness set their thresholds inside the same
condition. A zero threshold disables a detector outright, so asking for the
loopback check on its own reported `capture-detector-never-armed` in every
cycle while a 0.40 peak signal sat on the inspected channel. Counting a fault
and recording its context are separate requests, and they are separate now.

With the detectors actually armed, the configuration that had reported nothing
reported 31, 8 and 10 capture discontinuities across three cycles, with
lost_quanta, starvation and xruns all zero throughout. The breaks were real and
none of the counters that decide a verdict could see them.

**What silenced them.** Core control keeps one prime core parked, which is what
the Perfetto trace showed and what leaves both audio threads on one core. With
`min_cpus` raised to two, same geometry, detectors armed, three cycles each:

| | capture breaks per cycle | service gaps | worst gap |
|---|---|---|---|
| core control as shipped | 31, 8, 10 | 31, 13, 19 | 4.4, 3.1, 3.5 ms |
| min_cpus = 2 | **7, 0, 0** | 6, 1, 1 | 4.3, 1.0, 1.0 ms |

The second and third cycles are silent. The seven in the first are the cycle
that starts the session.

Repeated, with the same tweak and the same geometry: capture breaks 0, 2 and 0,
service gaps 4, 1 and 0, and all three cycles pass outright - the first time in
this file that a full run has. Across the two runs with the tweak, six cycles
carry nine breaks between them and five of the six are clean; across the run
without it, three cycles carry forty-nine and none is clean.

So the audible fault and the parked core are the same finding, and the tweak
that addresses it needs root, which this device now grants to the app. Finding
its sysfs path took two corrections worth recording: core control lives on the
cluster's first CPU rather than its fastest, and the cluster comes from the
cpufreq policy's `related_cpus` - `core_siblings_list` names the whole package
here, which pointed the search at the little cluster and applied the change to
the wrong four cores. The revert restored them, which is the only reason that
mistake cost nothing.

**Still open.** Two to four discontinuities per cycle in the rendered signal
itself, before it reaches USB, in both arms. The track loops about once a
second, so forty wraps a cycle, and these are not those. Not yet investigated.

## The breaks in the rendered signal are the harness stopping

The two to four discontinuities a cycle in the playback signal, left open above,
are the session stop. The flight recorder places them exactly: they arrive in
pairs 200 to 300 nanoseconds apart, one per channel, both at frame 0 of a
block, with steps of 0.28 and 0.34 against an output peak of 0.366 - close to
full scale. Their timestamps are 45.377 s apart in a run of 45 s cycles, and
they land 2173728 frames into the session, which at 48 kHz is 45.29 s.

One event per cycle, at the moment the harness stops the session, counted twice
because both channels see it. Not a driver defect, and the reason to record it
is that it consumed a line in the verdict for the whole run: a cycle that is
otherwise clean fails on `signal-discontinuity-growth-exceeded` because it was
switched off at the end.

## Real-time scheduling, and the interrupt

Both were in the tweak list untested because the device had no root. It has now,
granted to the app rather than to the shell, so the harness is the only place
they can be applied - which is what it does, before the session for the ones
that set a limit and again after it for the ones that set a policy on a thread.

**The real-time tweak did not work as written.** It raises `RLIMIT_RTPRIO` with
`prlimit`, and this device has no `prlimit`. It has `chrt`, which sets the
policy on a thread directly and therefore works on threads that already exist -
the limit never could, because each thread asks for its policy once, when it
starts. Servicing is given the higher priority of the two, at the bottom of the
real-time band: it runs every half millisecond against the graph's one and a
third, and a completion the device is waiting on cannot be made up later.

Toybox takes the pid before the priority, the reverse of util-linux. Passing
them the other way round sets the policy on a pid that happens to equal the
priority, and reports success.

**Measured against the listener's criterion,** capture discontinuities per
cycle with the detectors armed, same geometry throughout:

| | breaks per cycle | total | clean cycles |
|---|---|---:|---|
| nothing applied | 31, 8, 10 | 49 | 0 of 3 |
| USB interrupt moved to the core that stays awake | 14, 3, 6 | 23 | 0 of 3 |
| core control, both prime cores awake | 7, 0, 0 / 0, 2, 0 | 9 | 5 of 6 |
| **real-time policy, servicing above the graph** | **0, 0, 0 / 0, 0, 1** | **1** | **5 of 6** |

The real-time policy is the strongest of the three by a wide margin, and it is
the only one that cleaned the first cycle of a run - the one that carries the
session start.

**The interrupt tweak is close to a no-op here.** It writes the big-core mask,
and the interrupt is already on 6-7; its effective affinity is cpu6, which is
the core core control parks. Pinning it to cpu7 alone - the core that stays
awake - halves the breaks, which says the parked core does reach the interrupt
path, but it is not where the fault mostly lives.

Service gap counts do not order the same way: the real-time arm shows 10 to 21
gaps a cycle against 1 to 6 for core control, while producing far fewer audible
breaks. Gaps are not the audible quantity and should stop being read as one.
