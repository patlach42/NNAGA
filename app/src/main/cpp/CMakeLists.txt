# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of NNAGA.
#
# NNAGA is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# NNAGA is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with NNAGA. If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.22.1)

project("guitarrackcraft")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# LV2 Libraries Configuration
# Set LV2_LIBS_DIR to the directory containing compiled LV2 libraries
# Example: set(LV2_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lv2")
set(LV2_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lv2" CACHE PATH "Directory containing LV2 libraries")

# Check if LV2 libraries are available
set(LV2_AVAILABLE FALSE)
if(EXISTS "${LV2_LIBS_DIR}/include/lilv-0/lilv/lilv.h" OR EXISTS "${LV2_LIBS_DIR}/include/lilv/lilv.h")
    set(LV2_AVAILABLE TRUE)
    message(STATUS "LV2 libraries found at: ${LV2_LIBS_DIR}")
    
    # Include LV2 headers (lilv.h includes "lv2/core/lv2.h" so need base include too)
    include_directories(${LV2_LIBS_DIR}/include)
    if(EXISTS "${LV2_LIBS_DIR}/include/lilv-0")
        include_directories(${LV2_LIBS_DIR}/include/lilv-0)
        include_directories(${LV2_LIBS_DIR}/include/serd-0)
        include_directories(${LV2_LIBS_DIR}/include/sord-0)
    endif()
    
    # LV2 libraries (versioned names from build_lv2_libs.sh: lilv-0, serd-0, sord-0, sratom-0, zix-0)
    # Prefer static libraries to avoid packaging .so.0 files
    set(LV2_LIB_DIR "${LV2_LIBS_DIR}/lib")
    # Try static library first, then shared
    set(LILV_LIB "")
    if(EXISTS "${LV2_LIB_DIR}/liblilv-0.a")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.a")
    elseif(EXISTS "${LV2_LIB_DIR}/liblilv-0.so.0")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.so.0")
    elseif(EXISTS "${LV2_LIB_DIR}/liblilv-0.so")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.so")
    endif()
    if(NOT LILV_LIB)
        find_library(LILV_LIB NAMES lilv-0 lilv PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    endif()
    find_library(SERD_LIB NAMES serd-0 serd PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SORD_LIB NAMES sord-0 sord PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SRATOM_LIB NAMES sratom-0 sratom PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(ZIX_LIB NAMES zix-0 zix PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    if(NOT SERD_LIB AND EXISTS "${LV2_LIB_DIR}/libserd-0.a")
        set(SERD_LIB "${LV2_LIB_DIR}/libserd-0.a")
    endif()
    if(NOT SORD_LIB AND EXISTS "${LV2_LIB_DIR}/libsord-0.a")
        set(SORD_LIB "${LV2_LIB_DIR}/libsord-0.a")
    endif()
    if(NOT SRATOM_LIB AND EXISTS "${LV2_LIB_DIR}/libsratom-0.a")
        set(SRATOM_LIB "${LV2_LIB_DIR}/libsratom-0.a")
    endif()
    if(NOT ZIX_LIB AND EXISTS "${LV2_LIB_DIR}/libzix-0.a")
        set(ZIX_LIB "${LV2_LIB_DIR}/libzix-0.a")
    endif()
    
    if(LILV_LIB AND SERD_LIB AND SORD_LIB)
        message(STATUS "Found LV2 libraries: lilv-0, serd-0, sord-0")
    else()
        message(WARNING "Some LV2 libraries not found. LV2 support will be disabled.")
        set(LV2_AVAILABLE FALSE)
    endif()
else()
    message(STATUS "LV2 libraries not found. LV2 backend will be disabled.")
    message(STATUS "To enable LV2 support, compile libraries and set LV2_LIBS_DIR")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/lv2
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/x11
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/nnaga-native-plugin-sdk/include
)
# Direct USB Audio Class backend is provided by the local standalone submodule.
set(LOWLATENCY_AUDIO_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../liblowlatencyaudio"
    CACHE PATH "Path to the liblowlatencyaudio source tree")
if(NOT EXISTS "${LOWLATENCY_AUDIO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "liblowlatencyaudio not found at ${LOWLATENCY_AUDIO_ROOT}; initialize the submodule or set LOWLATENCY_AUDIO_ROOT")
endif()
add_subdirectory("${LOWLATENCY_AUDIO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/liblowlatencyaudio")

# Android Oboe backend (AAudio first with Oboe fallback).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/oboe"
                 "${CMAKE_CURRENT_BINARY_DIR}/oboe")

# ysfx JSFX engine (native AArch64 EEL, Android UI host)
set(YSFX_PORTABLE OFF CACHE BOOL "" FORCE)
set(YSFX_GFX ON CACHE BOOL "" FORCE)
set(YSFX_PLUGIN OFF CACHE BOOL "" FORCE)
set(YSFX_TESTS OFF CACHE BOOL "" FORCE)
set(YSFX_TOOLS OFF CACHE BOOL "" FORCE)
set(YSFX_SKIP_CHECKSUM ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/ysfx"
                 "${CMAKE_CURRENT_BINARY_DIR}/ysfx")
foreach(ysfx_android_target ysfx-private lice wdl-base eel2)
    if(TARGET ${ysfx_android_target})
        target_compile_options(${ysfx_android_target} PRIVATE
            "-include${CMAKE_CURRENT_SOURCE_DIR}/jsfx/ysfx_android_compat.h")
    endif()
endforeach()

# Plugin abstraction layer
add_library(plugin_abstraction STATIC
    plugin/PluginChain.cpp
    plugin/RackGraph.cpp
    plugin/PluginRegistry.cpp
    plugin/PluginUIGuard.cpp
    plugin/PluginUIManager.cpp
    plugin/RackStateCodec.cpp
    plugin/native/NativePlugin.cpp
    plugin/native/NativePluginFactory.cpp
    jsfx/JsfxPlugin.cpp
    jsfx/JsfxPluginFactory.cpp
)
target_include_directories(plugin_abstraction PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/ysfx/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/nnaga-native-plugin-sdk/include")
target_link_libraries(plugin_abstraction PRIVATE
    lowlatencyaudio::lowlatencyaudio
    ysfx
    dl)

# LV2 backend (conditional compilation)
if(LV2_AVAILABLE)
    add_library(lv2_backend STATIC
        plugin/lv2/LV2Plugin.cpp
        plugin/lv2/LV2PluginFactory.cpp
        plugin/lv2/LV2PluginUI.cpp
        plugin/lv2/LV2Utils.cpp
    )
    target_link_libraries(lv2_backend
        ${LILV_LIB}
        ${SERD_LIB}
        ${SORD_LIB}
        ${SRATOM_LIB}
        ${ZIX_LIB}
    )
    add_definitions(-DHAVE_LV2=1)
    message(STATUS "LV2 backend enabled")
else()
    # Create stub implementation when LV2 is not available
    add_library(lv2_backend STATIC
        plugin/lv2/LV2Plugin.cpp
        plugin/lv2/LV2PluginFactory.cpp
        plugin/lv2/LV2PluginUI.cpp
        plugin/lv2/LV2Utils.cpp
    )
    add_definitions(-DHAVE_LV2=0)
    message(STATUS "LV2 backend disabled (stub mode)")
endif()

# Shared utilities
add_library(utils STATIC
    utils/WavIO.cpp
)

# Audio engine
add_library(audio_engine STATIC
    engine/AudioEngine.cpp
    engine/AndroidOboeBackend.cpp
)
target_link_libraries(audio_engine
    utils
    lowlatencyaudio::lowlatencyaudio
    oboe
)
target_compile_options(lowlatencyaudio PRIVATE -O3)

# Third-party/general targets keep conservative optimization. Host realtime
# targets use -O3; ThinLTO and optional PGO are configured independently below.
foreach(general_target
        lv2_backend)
    target_compile_options(${general_target} PRIVATE -O2)
endforeach()

foreach(rt_target
        plugin_abstraction
        audio_engine)
    target_compile_options(${rt_target} PRIVATE -O3)
endforeach()

option(GRC_ENABLE_AUDIO_THINLTO
       "Enable ThinLTO for the native realtime audio path" ON)
if(GRC_ENABLE_AUDIO_THINLTO)
    foreach(lto_target
            plugin_abstraction
            audio_engine)
        target_compile_options(${lto_target} PRIVATE -flto=thin)
    endforeach()
    target_compile_options(lowlatencyaudio PRIVATE -flto=thin)
endif()

set(GRC_AUDIO_PGO_MODE "OFF" CACHE STRING
    "Audio PGO mode: OFF, GENERATE, or USE")
set_property(CACHE GRC_AUDIO_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(GRC_AUDIO_PGO_PROFILE "" CACHE FILEPATH
    "Merged arm64 LLVM profile for GRC_AUDIO_PGO_MODE=USE")
string(TOUPPER "${GRC_AUDIO_PGO_MODE}" GRC_AUDIO_PGO_MODE)
set(GRC_AUDIO_PGO_COMPILE_FLAG "")
set(GRC_AUDIO_PGO_LINK_FLAG "")
if(GRC_AUDIO_PGO_MODE STREQUAL "GENERATE")
    set(GRC_AUDIO_PGO_COMPILE_FLAG
        "-fprofile-instr-generate=/data/user/0/com.vibes.dsp/files/grc-audio-%p.profraw")
    set(GRC_AUDIO_PGO_LINK_FLAG "${GRC_AUDIO_PGO_COMPILE_FLAG}")
elseif(GRC_AUDIO_PGO_MODE STREQUAL "USE")
    if(NOT IS_ABSOLUTE "${GRC_AUDIO_PGO_PROFILE}" OR
       NOT EXISTS "${GRC_AUDIO_PGO_PROFILE}")
        message(FATAL_ERROR
            "GRC_AUDIO_PGO_PROFILE must be an existing absolute .profdata path")
    endif()
    set(GRC_AUDIO_PGO_COMPILE_FLAG
        "-fprofile-instr-use=${GRC_AUDIO_PGO_PROFILE}")
    set(GRC_AUDIO_PGO_LINK_FLAG "${GRC_AUDIO_PGO_COMPILE_FLAG}")
elseif(NOT GRC_AUDIO_PGO_MODE STREQUAL "OFF")
    message(FATAL_ERROR
        "GRC_AUDIO_PGO_MODE must be OFF, GENERATE, or USE")
endif()

if(GRC_AUDIO_PGO_COMPILE_FLAG)
    foreach(pgo_target
            plugin_abstraction
            audio_engine)
        target_compile_options(${pgo_target} PRIVATE
            "${GRC_AUDIO_PGO_COMPILE_FLAG}")
    endforeach()
    target_compile_options(lowlatencyaudio PRIVATE
        "${GRC_AUDIO_PGO_COMPILE_FLAG}")
endif()

# XShm stub - used when plugin UI .so references MIT-SHM (fallback to XPutImage)
add_library(xshm_stub STATIC
    xshm_stub.c
)

# X11 native display (EGL + minimal X11 server, ANativeWindow)
add_library(x11_native_display STATIC
    x11/X11NativeDisplay.cpp
    x11/X11Worker.cpp
    x11/X11AtomStore.cpp
    x11/X11WindowManager.cpp
    x11/X11PixmapStore.cpp
    x11/X11ConnectionHandler.cpp
    x11/X11Framebuffer.cpp
    x11/X11PropertyStore.cpp
)

add_library(guitarrackcraft SHARED
    jni/NativeBridge.cpp
    jni/NativeContextAccess.cpp
    jni/JsfxBridge.cpp
    jni/RackStateBridge.cpp
    jsfx/JsfxUiHost.cpp
)
target_link_libraries(x11_native_display
    log
    android
    EGL
    GLESv2
    lowlatencyaudio::lowlatencyaudio
)


target_link_libraries(guitarrackcraft
    plugin_abstraction
    audio_engine
    lv2_backend
    x11_native_display
    xshm_stub
    ysfx
    log
    android
    nativewindow
)

# The final shared-library link consumes ThinLTO bitcode from the three
# realtime static archives. JNI/LV2/X11 translation units remain outside LTO.
if(GRC_ENABLE_AUDIO_THINLTO)
    target_link_options(guitarrackcraft PRIVATE -flto=thin)
endif()

if(GRC_AUDIO_PGO_LINK_FLAG)
    target_link_options(guitarrackcraft PRIVATE
        "${GRC_AUDIO_PGO_LINK_FLAG}")
endif()

# Link LV2 libraries if available
if(LV2_AVAILABLE)
    target_link_libraries(guitarrackcraft
        ${LILV_LIB}
        ${SERD_LIB}
        ${SORD_LIB}
        ${SRATOM_LIB}
        ${ZIX_LIB}
    )
endif()

# VST hosting via :vsthost_lib (full flavor only — set by HAS_VST_HOST CMake
# arg from app/build.gradle.kts productFlavors.full.externalNativeBuild).
# When this define is set, NativeBridge.cpp:nativeInit will call
# vsthost::createVstFactory and register the result with PluginRegistry.
if(HAS_VST_HOST)
    find_package(vsthost_lib REQUIRED CONFIG)
    target_link_libraries(guitarrackcraft vsthost_lib::vsthost)
    target_compile_definitions(guitarrackcraft PRIVATE HAS_VST_HOST=1)
endif()
