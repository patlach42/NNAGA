#!/usr/bin/env bash
# Bootstraps the vsthost_lib FEX-pivot build tree.
#
# Sources are managed as git submodules in the parent NNAGA repo:
#   - Wine          external/wine-upstream      (github.com/wine-mirror/wine @ wine-11.14)
#   - FEX-Emu       external/fex-upstream       (github.com/FEX-Emu/FEX @ FEX-2607)
#   - llvm-mingw    external/llvm-mingw         (github.com/mstorsjo/llvm-mingw @ 20260616)
#
# Android Bionic adaptations live in patches/wine/ and are applied at build
# time by scripts/build-wine-android.sh / scripts/build-wine-pe.sh — NOT by
# this script. setup-fex-pivot.sh only verifies the submodules are present
# and builds the unmodified upstream llvm-mingw toolchain from source.
#
# Re-runnable: skips work that's already complete. Delete a submodule's
# working tree (or remove the install/ marker) to force re-init.

set -euo pipefail

# On development hosts with a DOS/PE binfmt handler, Autoconf can execute
# arm64ec conftest.exe through host Wine instead of recognizing it as a cross
# binary. A crashing guest then opens winedbg and blocks the whole toolchain
# build. Re-exec in a private mount namespace with an empty binfmt registry;
# output paths stay on the host filesystem and the global handler is unchanged.
if [[ -e /proc/sys/fs/binfmt_misc/DOSWin && -z "${GRC_ISOLATED_BINFMT:-}" ]]; then
    if ! command -v unshare >/dev/null || ! command -v mount >/dev/null; then
        echo "error: active DOSWin binfmt handler would run foreign PE configure probes" >&2
        echo "install util-linux with unshare/mount or disable the handler for this build" >&2
        exit 1
    fi
    exec unshare --user --map-root-user --mount --fork -- /bin/bash -c '
        set -euo pipefail
        mount --make-rprivate /
        mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
        export GRC_ISOLATED_BINFMT=1
        exec "$@"
    ' _ "$0" "$@"
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# Submodule paths within this lib (relative to repo_root which is vsthost_lib).

patch_is_applied() {
    local patch_name="$1"
    case "$patch_name" in
        0001-pass-clang-resource-major.patch)
            grep -Fqx -- '        -DCLANG_VERSION_MAJOR="${CLANG_RESOURCE_DIR##*/}" \' \
                "$LLVM_MINGW_DIR/build-compiler-rt.sh"
            ;;
        0002-openmp-pass-clang-resource-major.patch)
            grep -Fqx -- 'CLANG_RESOURCE_DIR="$("$PREFIX/bin/clang" --print-resource-dir)"' \
                "$LLVM_MINGW_DIR/build-openmp.sh" &&
                grep -Fqx -- '        -DCLANG_VERSION_MAJOR="${CLANG_RESOURCE_DIR##*/}" \' \
                    "$LLVM_MINGW_DIR/build-openmp.sh"
            ;;
        0003-openmp-include-generated-header.patch)
            grep -Fqx -- '    OPENMP_HEADER_DIR="$PWD/lib/clang/${CLANG_RESOURCE_DIR##*/}/include"' \
                "$LLVM_MINGW_DIR/build-openmp.sh" &&
                grep -Fqx -- '        -DCMAKE_C_FLAGS_INIT="$CFGUARD_CFLAGS -I$OPENMP_HEADER_DIR" \' \
                    "$LLVM_MINGW_DIR/build-openmp.sh" &&
                grep -Fqx -- '        -DCMAKE_CXX_FLAGS_INIT="$CFGUARD_CFLAGS -I$OPENMP_HEADER_DIR" \' \
                    "$LLVM_MINGW_DIR/build-openmp.sh"
            ;;
        *) return 1 ;;
    esac
}

apply_llvm_mingw_patches() {
    local patch_dir="$repo_root/patches/llvm-mingw"
    local patch
    for patch in "$patch_dir"/*.patch; do
        [[ -e "$patch" ]] || continue
        if git -C "$LLVM_MINGW_DIR" apply --check "$patch" 2>/dev/null; then
            git -C "$LLVM_MINGW_DIR" apply "$patch"
            echo "[+] applied llvm-mingw patch: $(basename "$patch")"
        elif patch_is_applied "$(basename "$patch")" ||
            git -C "$LLVM_MINGW_DIR" apply --reverse --check "$patch" 2>/dev/null; then
            echo "[=] llvm-mingw patch already applied: $(basename "$patch")"
        else
            echo "error: llvm-mingw patch no longer applies: $patch" >&2
            exit 1
        fi
    done
}

WINE_DIR="external/wine-upstream"
FEX_DIR="external/fex-upstream"
LLVM_MINGW_DIR="external/llvm-mingw"

require_submodule() {
    local label="$1" path="$2"
    if [ ! -d "$path/.git" ] && [ ! -f "$path/.git" ]; then
        echo "error: $label submodule not initialized at $path" >&2
        echo "  run from the NNAGA root: git submodule update --init --recursive $path" >&2
        return 1
    fi
    local head
    head=$(cd "$path" && git rev-parse --short HEAD)
    echo "[=] $label: $path @ $head"
}

# --- check submodules are populated -----------------------------------------
require_submodule "wine"        "$WINE_DIR"
require_submodule "fex"         "$FEX_DIR"
require_submodule "llvm-mingw"  "$LLVM_MINGW_DIR"
apply_llvm_mingw_patches

# --- build llvm-mingw locally (one-shot, ~30-60 min first run) --------------
# Output goes to external/llvm-mingw/install/. The source-revision marker
# prevents a compiler built from an older submodule pin from being reused.
LLVM_MINGW_INSTALL="$LLVM_MINGW_DIR/install"
LLVM_MINGW_SOURCE_REV=$(git -C "$LLVM_MINGW_DIR" rev-parse HEAD)
LLVM_MINGW_REVISION_FILE="$LLVM_MINGW_INSTALL/.source-revision"
LLVM_MINGW_INSTALLED_REV=""
if [ -f "$LLVM_MINGW_REVISION_FILE" ]; then
    LLVM_MINGW_INSTALLED_REV=$(cat "$LLVM_MINGW_REVISION_FILE")
fi

if [ ! -x "$LLVM_MINGW_INSTALL/bin/clang" ] ||
   [ "$LLVM_MINGW_INSTALLED_REV" != "$LLVM_MINGW_SOURCE_REV" ]; then
    echo "[+] building llvm-mingw locally — first run takes ~30-60 min and ~6-8 GB intermediates"
    echo "    output: $LLVM_MINGW_INSTALL"
    (
        cd "$LLVM_MINGW_DIR"
        # Upstream build-all.sh takes the install prefix as its sole positional arg.
        # --disable-lldb / --disable-clang-tools-extra trim ~2 GB and ~15 min from
        # the build; we never use either tool. ARM64EC support is in -mingw-w64,
        # not gated by these flags.
        ./build-all.sh \
            --disable-lldb \
            --disable-clang-tools-extra \
            "$(pwd)/install"
    )
    printf '%s\n' "$LLVM_MINGW_SOURCE_REV" > "$LLVM_MINGW_REVISION_FILE"
else
    echo "[=] llvm-mingw: $LLVM_MINGW_INSTALL/bin/clang already built for $LLVM_MINGW_SOURCE_REV"
fi

echo
echo "=== ready ==="
echo "  wine:        $WINE_DIR              ($(cd "$WINE_DIR" && cat VERSION))"
echo "  fex:         $FEX_DIR               ($(cd "$FEX_DIR" && git rev-parse --short HEAD))"
echo "  llvm-mingw:  $LLVM_MINGW_INSTALL   ($("$LLVM_MINGW_INSTALL/bin/clang" --version | head -1))"
echo
echo "next steps:"
echo "  scripts/build-wine-android.sh  (applies patches/wine/ then cross-compiles wine for Android)"
echo "  scripts/build-wine-pe.sh       (PE-side ARM64X DLLs)"
echo "  scripts/build-fex-pe.sh        (FEX-Emu PE DLLs)"
echo "  scripts/build-vst-host.sh      (vst_host.exe / vst_host_x86.exe)"
