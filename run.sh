#!/bin/bash

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# Unified build-and-run script for Guitar RackCraft.
#
# Usage:
#   ./run.sh              # debug (default)
#   ./run.sh debug        # full (VST) flavor, debug build
#   ./run.sh release      # full (VST) flavor, release build + AAB
#   ./run.sh playstore    # playstore flavor (no VST), release AAB + bundletool install
#
# The `full` flavor builds the Windows-VST host stack (wine/FEX/DXVK/Mesa) from
# source via build.sh — the first build is slow (~60-90 min), incremental after.
# Set BUILD_VST=0 to skip it and build a VST-less `full` APK.

set -e

MODE="${1:-debug}"

# ── Common setup ──────────────────────────────────────────────────────────────

# Prefer JDK 17 (AGP can fail with Java 21 on the jlink step)
for jdk in /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/java-17-openjdk; do
    if [ -d "$jdk" ]; then
        export JAVA_HOME="$jdk"
        break
    fi
done

export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export PATH=$PATH:$ANDROID_HOME/tools:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

check_device() {
    local count
    count=$(adb devices | grep -v "List of devices" | grep "device$" | wc -l)
    echo "$count"
}

# Release keystore — loaded from .env only when an equivalent environment
# variable is absent (the private .env file is not committed).
ENV_FILE="$PROJECT_ROOT/.env"
if [ -f "$ENV_FILE" ]; then
    if [ -z "${RELEASE_STORE_FILE:-}" ]; then
        RELEASE_STORE_FILE="$(set -a; source "$ENV_FILE"; printf '%s' "${RELEASE_STORE_FILE:-}")"
    fi
    if [ -z "${RELEASE_STORE_PASSWORD:-}" ]; then
        RELEASE_STORE_PASSWORD="$(set -a; source "$ENV_FILE"; printf '%s' "${RELEASE_STORE_PASSWORD:-}")"
    fi
    if [ -z "${RELEASE_KEY_ALIAS:-}" ]; then
        RELEASE_KEY_ALIAS="$(set -a; source "$ENV_FILE"; printf '%s' "${RELEASE_KEY_ALIAS:-}")"
    fi
    if [ -z "${RELEASE_KEY_PASSWORD:-}" ]; then
        RELEASE_KEY_PASSWORD="$(set -a; source "$ENV_FILE"; printf '%s' "${RELEASE_KEY_PASSWORD:-}")"
    fi
fi
export RELEASE_STORE_FILE RELEASE_STORE_PASSWORD RELEASE_KEY_ALIAS RELEASE_KEY_PASSWORD

require_release_credentials() {
    local missing=()
    [ -n "${RELEASE_STORE_FILE:-}" ] || missing+=(RELEASE_STORE_FILE)
    [ -n "${RELEASE_STORE_PASSWORD:-}" ] || missing+=(RELEASE_STORE_PASSWORD)
    [ -n "${RELEASE_KEY_ALIAS:-}" ] || missing+=(RELEASE_KEY_ALIAS)
    [ -n "${RELEASE_KEY_PASSWORD:-}" ] || missing+=(RELEASE_KEY_PASSWORD)
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "ERROR: missing release signing credential(s): ${missing[*]}" >&2
        exit 1
    fi
}

# ── Mode dispatch ─────────────────────────────────────────────────────────────

case "$MODE" in

# ══════════════════════════════════════════════════════════════════════════════
# DEBUG
# ══════════════════════════════════════════════════════════════════════════════
debug)
    echo "NNAGA - Debug Build"
    echo "========================="
    echo ""

    echo "Running native build..."
    ./build.sh
    echo ""

    echo "Building debug APK..."
    ./gradlew assembleFullDebug
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected."
        echo "APK: app/build/outputs/apk/full/debug/app-full-debug.apk"
        exit 1
    fi

    echo "Installing..."
    ./gradlew installFullDebug
    echo ""

    echo "Starting app..."
    adb shell am start -n com.vibes.dsp/.MainActivity
    echo ""
    echo "Debug build installed and started."
    ;;

# ══════════════════════════════════════════════════════════════════════════════
# RELEASE
# ══════════════════════════════════════════════════════════════════════════════
release)
    echo "NNAGA - Release Build"
    echo "==========================="
    echo ""

    echo "Running native build..."
    ./build.sh
    echo ""

    echo "Building release AAB + APK..."
    ./gradlew bundleFullRelease assembleFullRelease
    echo ""

    AAB=app/build/outputs/bundle/fullRelease/app-full-release.aab
    if [ -f "$AAB" ]; then
        echo "AAB: $AAB ($(du -sh "$AAB" | cut -f1))"
    fi

    APK=app/build/outputs/apk/full/release/app-full-release.apk
    if [ -f "$APK" ]; then
        echo "APK: $APK ($(du -sh "$APK" | cut -f1))"
    fi
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected — skipping install."
        exit 0
    fi

    if [ -f "$APK" ]; then
        echo "Installing release APK..."
        adb install -r "$APK"
    else
        echo "No signed APK found — skipping install."
        exit 0
    fi
    echo ""

    echo "Starting app..."
    adb shell am start -n com.vibes.dsp/.MainActivity
    echo ""
    echo "Release build installed and started."
    ;;

# ══════════════════════════════════════════════════════════════════════════════
# PLAYSTORE
# ══════════════════════════════════════════════════════════════════════════════
playstore)
    echo "NNAGA - Play Store Build"
    echo "=============================="
    echo ""

    # Ensure bundletool
    BUNDLETOOL="$PROJECT_ROOT/bundletool.jar"
    if [ ! -f "$BUNDLETOOL" ]; then
        BUNDLETOOL_VERSION="1.17.2"
        echo "Downloading bundletool $BUNDLETOOL_VERSION..."
        curl -sL "https://github.com/google/bundletool/releases/download/${BUNDLETOOL_VERSION}/bundletool-all-${BUNDLETOOL_VERSION}.jar" \
            -o "$BUNDLETOOL"
        echo ""
    fi

    echo "Running native build (playstore)..."
    ./build.sh playstore
    echo ""

    echo "Building playstore AAB..."
    ./gradlew bundlePlaystoreRelease
    echo ""

    AAB=$(find app/build/outputs/bundle/playstoreRelease -name '*.aab' 2>/dev/null | head -1)
    if [ -z "$AAB" ]; then
        echo "ERROR: AAB not found"
        exit 1
    fi
    require_release_credentials
    echo "AAB: $AAB ($(du -sh "$AAB" | cut -f1))"
    echo "  Signed with release key — ready for Google Play."
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected."
        echo ""
        echo "To install manually:"
        echo "  java -jar bundletool.jar build-apks \\"
        echo '    --bundle="$AAB" --output=out.apks --local-testing \'
        echo '    --ks="$RELEASE_STORE_FILE" --ks-pass=pass:"$RELEASE_STORE_PASSWORD" \'
        echo '    --ks-key-alias="$RELEASE_KEY_ALIAS" --key-pass=pass:"$RELEASE_KEY_PASSWORD"'
        echo "  java -jar bundletool.jar install-apks --apks=out.apks"
        exit 1
    fi

    # Generate split APKs with --local-testing for PAD simulation
    APKS="$PROJECT_ROOT/build/playstore-local.apks"
    mkdir -p "$(dirname "$APKS")"
    rm -f "$APKS"

    echo "Generating split APKs (local-testing PAD simulation)..."
    java -jar "$BUNDLETOOL" build-apks \
        --bundle="$AAB" \
        --output="$APKS" \
        --local-testing \
        --ks="$RELEASE_STORE_FILE" \
        --ks-pass=pass:"$RELEASE_STORE_PASSWORD" \
        --ks-key-alias="$RELEASE_KEY_ALIAS" \
        --key-pass=pass:"$RELEASE_KEY_PASSWORD"
    echo ""

    echo "Installing on device..."
    java -jar "$BUNDLETOOL" install-apks --apks="$APKS"
    echo ""

    echo "Starting app..."
    adb shell am start -n com.vibes.dsp/.MainActivity
    echo ""
    echo "Play Store build installed and started."
    echo "Install-time asset packs delivered via --local-testing mode."
    ;;

*)
    echo "Usage: $0 [debug|release|playstore]"
    exit 1
    ;;
esac

echo ""
echo "Logs: adb logcat | grep -E 'AudioEngine|NativeBridge|LV2Plugin|PluginBrowser'"
