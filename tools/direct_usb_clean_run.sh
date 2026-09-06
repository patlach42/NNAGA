#!/system/bin/sh
#
# Runs the Direct USB stress test on a quiet device and puts the bench back.
#
# Why this exists. Background Wi-Fi activity was measured to interrupt USB
# audio about every thirty seconds on the reference device - a dropout no
# driver counter saw, which disappeared entirely when Wi-Fi was switched off.
# But the bench itself is reached over adb-over-TCP, so every measurement taken
# the ordinary way is contaminated by the very thing being measured.
#
# So: run on the device, detached, with Wi-Fi down for the duration. Killing
# Wi-Fi drops adb, which would take an attached shell with it, hence setsid and
# redirected output at the call site:
#
#   adb push tools/direct_usb_clean_run.sh /data/local/tmp/
#   adb shell chmod 755 /data/local/tmp/direct_usb_clean_run.sh
#   adb shell 'nohup setsid sh /data/local/tmp/direct_usb_clean_run.sh 150000 >/dev/null 2>&1 &'
#   # wait, reconnect, then read /data/local/tmp/clean_run/
#
# Note that wifi_scan_always_enabled was already 0 on the reference device, so
# the interference is not background scanning; disabling the radio is what
# works. svc wifi needs no root.
OUT=/data/local/tmp/clean_run
DUR=${1:-120000}
mkdir -p $OUT
: > $OUT/status
echo "start $(date +%s)" >> $OUT/status

am force-stop com.vibes.dsp
am force-stop com.termux
am force-stop com.termux.api
sleep 2

svc wifi disable
echo "wifi_disabled rc=$?" >> $OUT/status
sleep 5

logcat -c
am instrument -w \
  -e class com.vibes.dsp.engine.DirectUsbDeviceStressTest#duplexRateBufferLifecycleStress \
  -e direct_usb_rates 48000 -e direct_usb_buffers 64 -e direct_usb_multipliers 3 \
  -e direct_usb_cycles 1 -e direct_usb_duration_ms $DUR \
  -e direct_usb_flight_recorder true -e direct_usb_poll_ms 250 \
  com.vibes.dsp.test/androidx.test.runner.AndroidJUnitRunner > $OUT/instrument.txt 2>&1
echo "instrument done rc=$?" >> $OUT/status

logcat -d > $OUT/logcat.txt 2>/dev/null
echo "log captured lines=$(wc -l < $OUT/logcat.txt)" >> $OUT/status

svc wifi enable
echo "wifi_enabled rc=$?" >> $OUT/status
echo "end $(date +%s)" >> $OUT/status
