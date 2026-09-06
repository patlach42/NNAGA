#!/system/bin/sh
#
# Runs one Direct USB stress arm on the device, detached, and leaves the
# evidence in $OUT. Detached because the bench is reached over adb-over-TCP:
# a dropped connection must not take a measurement with it.
#
# Wi-Fi is deliberately left alone. The goal is the minimum latency that holds
# in the conditions the device actually runs in, and the radio is one of them.
#
#   adb push tools/minlat_arm.sh /data/local/tmp/
#   adb shell chmod 755 /data/local/tmp/minlat_arm.sh
#   adb shell 'nohup setsid sh /data/local/tmp/minlat_arm.sh <name> <buffer> \
#       <multiplier> <transfers> <headroom> <admission> <reserve> \
#       <render_stall_us> <service_stall_us> <audio_affinity> <ui_affinity> \
#       <service_cpus> <adpf_mode> <ui_meter_ms> <ui_frame_clock> <ui_clock_ms> \
#       <ui_stats_ms> <tweaks|-> <require_loopback> <output_pair> <input_channel> \
#       <cycles> <duration_ms> \
#       >/dev/null 2>&1 &'
NAME=$1
BUF=$2
MULT=$3
TRANSFERS=$4
HEADROOM=$5
# 0 waits for room, 1 paces by frames the device has played. The reserve is how
# far the producer may run ahead under credit: zero is strict credit, which was
# measured to starve, and a large reserve degenerates to wait-for-room.
ADMISSION=$6
RESERVE=$7
# Disturbance is injected rather than waited for. A render stall delays the
# producer while USB keeps draining; a service stall stops completions, so the
# ring stops draining and capture URBs are not resubmitted. Different faults,
# so they are separate knobs and never fired in the same arm.
RENDER_STALL=$8
SERVICE_STALL=$9
shift 9
# Affinity as two independent factors plus where servicing sits. The syscall
# fix turned audio and UI affinity on together, so an arm that cannot vary them
# separately cannot say which one moved a number.
AUDIO_AFF=$1
UI_AFF=$2
SVC_CPUS=$3
# 0 off, 1 the old CPU-only signal, 2 wall and CPU reported separately.
ADPF=$4
# The interface's own periodic work: meter poll period in ms, and whether the
# transport display follows the display frame clock.
UI_METER_MS=$5
UI_FRAME_CLOCK=$6
UI_CLOCK_MS=$7
UI_STATS_MS=$8
shift 8
# Comma separated tweak ids applied for the run and reverted after it, or "-"
# for none. Privileged tweaks are only reachable from the app's own uid, so
# this is the only place they can be measured.
TWEAKS=$1
[ "$TWEAKS" = "-" ] && TWEAKS=""
# Arms the analog loopback detectors. Off by default because they need a
# return path, but when there is one they are the only thing in the harness
# that hears what a listener hears.
LOOPBACK=$2
# Which output pair carries the tone and which input channel is watched. An
# interface with an internal loop returns playback on the pair fed by the
# playback pair, not on input one, and the detector only arms on a channel
# that actually carries signal.
OUT_PAIR=$3
IN_CHAN=$4
# Packets per transfer. Pinned rather than left automatic: the automatic policy
# picks eight here, which makes a transfer 48 frames instead of 24 and doubles
# the submitted runway. Leaving it unpinned let the geometry drift between arms
# and invalidated a comparison.
PACKETS=$5
CYCLES=$6
DUR=$7
OUT=/data/local/tmp/minlat/$NAME
rm -rf $OUT
mkdir -p $OUT
: > $OUT/status
echo "start $(date +%s) buffer=$BUF multiplier=$MULT transfers=$TRANSFERS headroom=$HEADROOM admission=$ADMISSION reserve=$RESERVE render_stall_us=$RENDER_STALL service_stall_us=$SERVICE_STALL audio_affinity=$AUDIO_AFF ui_affinity=$UI_AFF service_cpus=$SVC_CPUS adpf=$ADPF ui_meter_ms=$UI_METER_MS ui_frame_clock=$UI_FRAME_CLOCK ui_clock_ms=$UI_CLOCK_MS ui_stats_ms=$UI_STATS_MS tweaks=$TWEAKS loopback=$LOOPBACK out_pair=$OUT_PAIR in_chan=$IN_CHAN packets=$PACKETS cycles=$CYCLES duration_ms=$DUR" >> $OUT/status

am force-stop com.vibes.dsp
sleep 2
logcat -c

# The interface is raised by the test itself, before it starts any audio, so
# the app switch cannot land inside a measured window. This shell only watches
# that it really was in front: a run claiming an open interface has to show it.
(
  while [ ! -f $OUT/instrument.txt ] || ! grep -q "^OK\|^FAILURES" $OUT/instrument.txt 2>/dev/null; do
    dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity >> $OUT/foreground.txt
    sleep 10
  done
) &
UI_PID=$!

am instrument -w \
  -e class com.vibes.dsp.engine.DirectUsbDeviceStressTest#duplexRateBufferLifecycleStress \
  -e direct_usb_rates 48000 \
  -e direct_usb_buffers $BUF \
  -e direct_usb_multipliers $MULT \
  -e direct_usb_transfers $TRANSFERS \
  -e direct_usb_headroom $HEADROOM \
  -e direct_usb_admission $ADMISSION \
  -e direct_usb_credit_reserve $RESERVE \
  -e direct_usb_render_stall_us $RENDER_STALL \
  -e direct_usb_service_stall_us $SERVICE_STALL \
  -e direct_usb_audio_affinity $AUDIO_AFF \
  -e direct_usb_ui_affinity $UI_AFF \
  -e direct_usb_service_cpus $SVC_CPUS \
  -e direct_usb_open_ui 1 \
  -e direct_usb_adpf $ADPF \
  -e direct_usb_ui_meter_ms $UI_METER_MS \
  -e direct_usb_ui_frame_clock $UI_FRAME_CLOCK \
  -e direct_usb_ui_clock_ms $UI_CLOCK_MS \
  -e direct_usb_ui_stats_ms $UI_STATS_MS \
  -e direct_usb_tweaks "$TWEAKS" \
  -e direct_usb_require_loopback $LOOPBACK \
  -e direct_usb_output_pair $OUT_PAIR \
  -e direct_usb_input_channel $IN_CHAN \
  -e direct_usb_packets $PACKETS \
  -e direct_usb_cycles $CYCLES \
  -e direct_usb_duration_ms $DUR \
  -e direct_usb_poll_ms 250 \
  com.vibes.dsp.test/androidx.test.runner.AndroidJUnitRunner > $OUT/instrument.txt 2>&1
echo "instrument rc=$?" >> $OUT/status
kill $UI_PID 2>/dev/null
echo "foreground_vibes=$(grep -c 'com.vibes.dsp' $OUT/foreground.txt 2>/dev/null) foreground_samples=$(wc -l < $OUT/foreground.txt 2>/dev/null)" >> $OUT/status

# Bounded: an unbounded dump hung here once with the run already finished, and
# a measurement that cannot be collected is a measurement lost. The tail is
# generous enough for several cycles of telemetry.
logcat -d -t 40000 > $OUT/logcat.txt 2>/dev/null
echo "log lines=$(wc -l < $OUT/logcat.txt)" >> $OUT/status
# Context, not the geometry of record. The test restores the saved buffer and
# multiplier when it finishes, so this file says what the app is configured
# for - notably whether thermal safety is armed - while the geometry each
# cycle actually ran is read from the telemetry line itself.
run-as com.vibes.dsp cat /data/data/com.vibes.dsp/shared_prefs/audio_settings.xml > $OUT/audio_settings.xml 2>/dev/null
echo "end $(date +%s)" >> $OUT/status
