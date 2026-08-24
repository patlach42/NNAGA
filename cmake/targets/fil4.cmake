# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# Build x42 fil4 (mono + stereo DSP) for Android arm64-v8a.
# The upstream OpenGL/X11 UI is intentionally not built on Android; the
# upstream modgui is materialized in the bundle and is the host UI fallback.

set(_fil4_src "${THIRD_PARTY}/fil4.lv2")
set(_fil4_build "${CMAKE_CURRENT_BINARY_DIR}/fil4")
set(_fil4_assets "${ASSETS_DIR}/fil4.lv2")
set(_fil4_lv2ttl "${_fil4_src}/lv2ttl")

if(NOT EXISTS "${_fil4_src}/src/lv2.c")
    message(FATAL_ERROR "fil4 source is missing: ${_fil4_src}/src/lv2.c")
endif()
if(NOT EXISTS "${LV2_INCLUDE}/lv2/core/lv2.h")
    message(FATAL_ERROR "LV2 headers are missing: ${LV2_INCLUDE}")
endif()

file(MAKE_DIRECTORY "${_fil4_build}" "${_fil4_assets}")

# Materialize the upstream templates without invoking its host-only Makefile.
set(LV2NAME fil4)
set(LIB_EXT .so)
set(LV2GUI fil4UI_gl)
set(UI_TYPE "ui:X11UI")
set(UI_REQ "")
set(URISUFFIX mono)
set(NAMESUFFIX " Mono")
set(CTLSIZE 65888)
set(SIGNATURE "")
set(VERSION "")
set(UITTL "")
set(MODBRAND "")
set(MODLABEL "")

set(_fil4_manifest_base "${_fil4_build}/manifest.base.ttl")
configure_file("${_fil4_lv2ttl}/manifest.ttl.in" "${_fil4_manifest_base}" @ONLY)
file(READ "${_fil4_manifest_base}" _fil4_manifest)

# modgui metadata is valid without a native ui:binary and points only at
# resources copied below, so no dangling native UI reference is emitted.
foreach(_fil4_suffix mono stereo)
    if(_fil4_suffix STREQUAL mono)
        set(NAMESUFFIX " Mono")
    else()
        set(NAMESUFFIX " Stereo")
    endif()
    set(URISUFFIX "${_fil4_suffix}")
    configure_file("${_fil4_lv2ttl}/manifest.modgui.in"
                   "${_fil4_build}/manifest.modgui.${_fil4_suffix}.ttl" @ONLY)
    file(READ "${_fil4_build}/manifest.modgui.${_fil4_suffix}.ttl" _fil4_modgui)
    string(APPEND _fil4_manifest "\n${_fil4_modgui}")
endforeach()
file(WRITE "${_fil4_assets}/manifest.ttl" "${_fil4_manifest}")

# Main plugin TTL and its mono/stereo port blocks are generated exactly as in
# upstream git2lv2.mk, but at configure time for deterministic cross builds.
set(URISUFFIX mono)
set(NAMESUFFIX " Mono")
set(CTLSIZE 65888)
configure_file("${_fil4_lv2ttl}/fil4.ttl.in" "${_fil4_build}/fil4.base.ttl" @ONLY)
configure_file("${_fil4_lv2ttl}/fil4.ports.ttl.in" "${_fil4_build}/fil4.mono.ports.ttl" @ONLY)
file(READ "${_fil4_build}/fil4.base.ttl" _fil4_ttl)
file(READ "${_fil4_build}/fil4.mono.ports.ttl" _fil4_mono_ports)
file(READ "${_fil4_lv2ttl}/fil4.mono.ttl.in" _fil4_mono_audio)
string(APPEND _fil4_ttl "\n${_fil4_mono_ports}\n${_fil4_mono_audio}")
set(URISUFFIX stereo)
set(NAMESUFFIX " Stereo")
set(CTLSIZE 131424)
configure_file("${_fil4_lv2ttl}/fil4.ports.ttl.in" "${_fil4_build}/fil4.stereo.ports.ttl" @ONLY)
file(READ "${_fil4_build}/fil4.stereo.ports.ttl" _fil4_stereo_ports)
file(READ "${_fil4_lv2ttl}/fil4.stereo.ttl.in" _fil4_stereo_audio)
string(APPEND _fil4_ttl "\n${_fil4_stereo_ports}\n${_fil4_stereo_audio}")
file(WRITE "${_fil4_assets}/fil4.ttl" "${_fil4_ttl}")

# Copy all upstream modgui resources; no generated or host-specific files are
# introduced into the submodule.
file(COPY "${_fil4_src}/modgui" DESTINATION "${_fil4_assets}")

set_source_files_properties("${_fil4_src}/src/lv2.c" PROPERTIES LANGUAGE CXX)
add_library(fil4_dsp SHARED "${_fil4_src}/src/lv2.c")
target_include_directories(fil4_dsp PRIVATE
    "${_fil4_src}/src"
    "${LV2_INCLUDE}"
    "${LV2_COMPAT_DIR}"
)
lv2_set_dsp_properties(fil4_dsp fil4 "${_fil4_build}")

# lv2_sync_to_jnilibs places libfil4.so in jniLibs (the Android loader path),
# while the bundle itself contains only TTL and modgui resources.
lv2_sync_to_jnilibs(fil4_sync "${_fil4_build}" fil4_dsp
    INCLUDE_PATTERN "^fil4\\.so$")
add_custom_target(fil4_done DEPENDS fil4_sync)
