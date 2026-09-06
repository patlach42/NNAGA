#!/usr/bin/env bash
#
# Confirms that the build actually installed on a device runs its audio engine.
#
# Why this exists. A hash comparison can prove the APK on the device is the one
# that was built, and that its native library is the same binary that passed an
# earlier run. It cannot prove that this installation starts: app data lives
# outside the APK, and a reinstall that wipes it takes the USB device grant
# with it. The engine then never loads, and nothing in the package tells you
# so. That gap is only closed by watching a real session come up.
#
# What counts as up, in order of how much each rules out:
#   - libguitarrackcraft.so mapped into the process  -> the native engine and
#     the plugin host loaded at all;
#   - a "streaming:" line from LibusbUacDriver       -> the USB device was
#     claimed and the isochronous path opened;
#   - no FATAL and no ForegroundServiceDidNotStartInTime -> the process is not
#     being killed out from under the session.
#
# None of this needs the instrumented test, so it does not occupy the bench or
# rewrite the audio settings - it observes an ordinary session.
#
# Usage:  tools/verify_installed_build.sh [adb-serial]
# with the interface plugged in. Grant the USB dialog when it appears.
set -u

PKG=com.vibes.dsp
SERIAL="${1:-}"
adb() { if [ -n "$SERIAL" ]; then command adb -s "$SERIAL" "$@"; else command adb "$@"; fi; }

if ! adb shell true >/dev/null 2>&1; then
    echo "no device reachable${SERIAL:+ at $SERIAL}" >&2
    exit 2
fi

echo "== installed package =="
adb shell pm path $PKG || { echo "$PKG is not installed" >&2; exit 2; }

adb logcat -c 2>/dev/null
adb shell am start -n $PKG/.MainActivity >/dev/null 2>&1
echo "launched; waiting for the engine (grant the USB dialog if it appears)"

pid=""; mapped=""
for _ in $(seq 1 60); do
    sleep 2
    pid=$(adb shell pidof $PKG 2>/dev/null | tr -d '\r')
    [ -n "$pid" ] || continue
    mapped=$(adb shell "run-as $PKG cat /proc/$pid/maps" 2>/dev/null \
        | grep -c 'libguitarrackcraft\.so')
    [ "${mapped:-0}" -gt 0 ] && break
done

fail=0
if [ "${mapped:-0}" -gt 0 ]; then
    echo "PASS  native engine mapped into pid $pid"
else
    echo "FAIL  libguitarrackcraft.so never appeared in the process"
    echo "      the engine did not start - check the USB grant and the device pick"
    fail=1
fi

if adb logcat -d 2>/dev/null | grep -q 'LibusbUacDriver.*streaming:'; then
    echo "PASS  USB streaming opened:"
    adb logcat -d 2>/dev/null | grep 'LibusbUacDriver.*streaming:' | tail -1 | sed 's/^/      /'
else
    echo "WARN  no streaming line yet - the device may not be selected"
fi

if adb logcat -d 2>/dev/null | grep -qE 'FATAL|ForegroundServiceDidNotStartInTime'; then
    echo "FAIL  the process is crashing:"
    adb logcat -d 2>/dev/null | grep -E 'FATAL|ForegroundServiceDidNotStartInTime' \
        | head -3 | sed 's/^/      /'
    fail=1
else
    echo "PASS  no crash and no foreground-service kill"
fi

exit $fail
