# Dusk Audio 4K EQ 2 (DAF) — Android arm64 LV2 bundle
# The DAF shell uses the framework-free FourKEQDSP implementation and exports
# host-visible controls/presets in its LV2 Turtle.  The OpenGL UI is built when
# DAF/DAF-Widgets are available; the generated bundle is copied as a whole so
# its manifest, controls, presets and resources stay in lock-step with the ELF.

set(_dusk_4keq_src    "${THIRD_PARTY}/dusk-audio-plugins/plugins/4k-eq/daf-plugin")
set(_dusk_4keq_build  "${PROJECT_ROOT}/build/dusk_4keq_android")
set(_dusk_4keq_host_build "${PROJECT_ROOT}/build/dusk_4keq_host")
set(_dusk_4keq_bundle "four_k_eq_2.lv2")
set(_dusk_4keq_assets "${ASSETS_DIR}/${_dusk_4keq_bundle}")
set(_dusk_4keq_so     "${_dusk_4keq_build}/bin/${_dusk_4keq_bundle}/four_k_eq_2.so")

if(NOT EXISTS "${_dusk_4keq_src}/CMakeLists.txt")
    message(FATAL_ERROR "Dusk 4K EQ source is missing: ${_dusk_4keq_src}")
endif()

# DAF and DAF-Widgets are pinned repo-local submodules.
set(_dusk_daf_path "${THIRD_PARTY}/DAF")
set(_dusk_dafwidgets_path "${THIRD_PARTY}/DAF-Widgets")

set(_dusk_4keq_stage "${PROJECT_ROOT}/build/dusk_4keq_source")
set(_dusk_4keq_root "${_dusk_4keq_stage}/source-root")
set(_dusk_4keq_source "${_dusk_4keq_root}/plugins/4k-eq/daf-plugin")
set(_dusk_daf_source "${_dusk_4keq_stage}/DAF")
set(_dusk_dafwidgets_source "${_dusk_4keq_stage}/DAF-Widgets")
set(_dusk_crashlog_patch "${PROJECT_ROOT}/cmake/patches/daf_android_crashlog.patch")
set(_dusk_ui_patch "${PROJECT_ROOT}/cmake/patches/dusk_4keq_ui_android.patch")
foreach(_dusk_required_patch IN ITEMS "${_dusk_crashlog_patch}" "${_dusk_ui_patch}")
    if(NOT EXISTS "${_dusk_required_patch}")
        message(FATAL_ERROR "Dusk patch is missing: ${_dusk_required_patch}")
    endif()
endforeach()
# Mirror all source siblings so relative shared-daf includes resolve, while
# keeping pinned submodules untouched.
file(REMOVE_RECURSE "${_dusk_4keq_root}" "${_dusk_daf_source}" "${_dusk_dafwidgets_source}")
file(COPY "${THIRD_PARTY}/dusk-audio-plugins/" DESTINATION "${_dusk_4keq_root}")
file(COPY "${THIRD_PARTY}/DAF/" DESTINATION "${_dusk_daf_source}")
file(COPY "${THIRD_PARTY}/DAF-Widgets/" DESTINATION "${_dusk_dafwidgets_source}")
execute_process(
    COMMAND patch -p1 --forward --no-backup-if-mismatch
    INPUT_FILE "${_dusk_crashlog_patch}"
    WORKING_DIRECTORY "${_dusk_4keq_root}/plugins/shared-daf"
    RESULT_VARIABLE _dusk_crashlog_patch_result)
if(NOT _dusk_crashlog_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply Dusk CrashLog Android patch")
endif()
execute_process(
    COMMAND patch -p1 --forward --no-backup-if-mismatch
    INPUT_FILE "${_dusk_ui_patch}"
    WORKING_DIRECTORY "${_dusk_4keq_source}"
    RESULT_VARIABLE _dusk_ui_patch_result)
if(NOT _dusk_ui_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply Dusk UI Android patch")
endif()
set(_dusk_pkg_wrapper "${_dusk_4keq_build}/pkg-config-x11")
file(WRITE "${_dusk_pkg_wrapper}"
    "#!/bin/sh\nexec env PKG_CONFIG_PATH='${X11_SYSROOT}/lib/pkgconfig:${X11_SYSROOT}/share/pkgconfig' PKG_CONFIG_LIBDIR='${X11_SYSROOT}/lib/pkgconfig:${X11_SYSROOT}/share/pkgconfig' PKG_CONFIG_SYSROOT_DIR='' pkg-config \"$@\"\n")
file(CHMOD "${_dusk_pkg_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
# DAF discovers plugin metadata by loading the completed LV2 binary.  An
# Android binary cannot run on the build host, so generate authoritative TTL
# once with a native host build and use the cross build only for its ELF.
ExternalProject_Add(dusk_4keq_host
    SOURCE_DIR "${_dusk_4keq_source}"
    BINARY_DIR "${_dusk_4keq_host_build}"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DDAF_PATH=${_dusk_daf_source}
        -DDAFWIDGETS_PATH=${_dusk_dafwidgets_source}
        -DDUSK_DAF_INSTALL_LOCAL=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target four_k_eq_2-lv2 -j${NJOBS}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
        "${_dusk_4keq_host_build}/bin/${_dusk_4keq_bundle}/manifest.ttl"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
)

ExternalProject_Add(dusk_4keq_build
    SOURCE_DIR "${_dusk_4keq_source}"
    BINARY_DIR "${_dusk_4keq_build}"
    INSTALL_DIR "${_dusk_4keq_build}/install"
    CMAKE_ARGS
        -DCMAKE_CROSSCOMPILING_EMULATOR=/bin/true
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DCMAKE_BUILD_TYPE=Release
        -DDAF_PATH=${_dusk_daf_source}
        -DDAFWIDGETS_PATH=${_dusk_dafwidgets_source}
        -DDUSK_DAF_INSTALL_LOCAL=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--as-needed
        ${NDK_CCACHE_CMAKE_ARGS}
        -DOPENGL_gl_LIBRARY=${X11_SYSROOT}/lib/libGL.so
        -DOPENGL_opengl_LIBRARY=${X11_SYSROOT}/lib/libGL.so
        -DOPENGL_glx_LIBRARY=${X11_SYSROOT}/lib/libGL.so
        -DOPENGL_INCLUDE_DIR=${X11_SYSROOT}/include
        -DCMAKE_PREFIX_PATH=${X11_SYSROOT}
        -DCMAKE_FIND_ROOT_PATH=${X11_SYSROOT}
        -DCMAKE_INCLUDE_PATH=${X11_SYSROOT}/include
        -DCMAKE_LIBRARY_PATH=${X11_SYSROOT}/lib
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
        -DPKG_CONFIG_EXECUTABLE=${_dusk_pkg_wrapper}
        -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=ON
        -DX11_FOUND=TRUE
        -DX11_INCLUDE_DIR=${X11_SYSROOT}/include
        -DX11_X11_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_X11_LIB=${X11_SYSROOT}/lib/libX11.so
        -DX11_Xext_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xext_LIB=${X11_SYSROOT}/lib/libXext.so
        -DX11_Xcursor_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xcursor_LIB=${X11_SYSROOT}/lib/libXcursor.so
        -DX11_Xrandr_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xrandr_LIB=${X11_SYSROOT}/lib/libXrandr.so
        -DX11_Xrender_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xrender_LIB=${X11_SYSROOT}/lib/libXrender.so
        -DX11_Xi_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xi_LIB=${X11_SYSROOT}/lib/libXi.so
        -DX11_Xfixes_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xfixes_LIB=${X11_SYSROOT}/lib/libXfixes.so
        -DX11_Xau_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xau_LIB=${X11_SYSROOT}/lib/libXau.so
        -DX11_Xdmcp_INCLUDE_PATH=${X11_SYSROOT}/include
        -DX11_Xdmcp_LIB=${X11_SYSROOT}/lib/libXdmcp.so
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target four_k_eq_2-lv2 -j${NJOBS}
    INSTALL_COMMAND ""
    DEPENDS lv2_libs mesa_runtime_libs x11_runtime_libs
    BUILD_BYPRODUCTS "${_dusk_4keq_so}"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
)

watch_external_sources(dusk_4keq_build
    DIRECTORIES "${_dusk_4keq_src}" "${THIRD_PARTY}/dusk-audio-plugins/plugins/shared-daf"
    PATTERNS "*.cpp" "*.hpp" "*.h" "CMakeLists.txt")

# Copy all generated LV2 metadata/resources, including presets.ttl and any UI
# assets.  No UI declaration is invented here: DAF's exporter is authoritative,
# so a UI is either a valid built artifact or absent from the generated TTL.
set(_dusk_4keq_bundle_stamp "${CMAKE_BINARY_DIR}/stamps/dusk_4keq_bundle.stamp")
file(MAKE_DIRECTORY "${ASSETS_DIR}" "${CMAKE_BINARY_DIR}/stamps")
add_custom_command(
    OUTPUT "${_dusk_4keq_bundle_stamp}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_dusk_4keq_assets}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dusk_4keq_assets}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_dusk_4keq_host_build}/bin/${_dusk_4keq_bundle}" "${_dusk_4keq_assets}"
    COMMAND ${CMAKE_COMMAND} -E rm -f
            "${_dusk_4keq_assets}/four_k_eq_2.so"
    COMMAND ${CMAKE_COMMAND} -E touch "${_dusk_4keq_bundle_stamp}"
    DEPENDS dusk_4keq_build dusk_4keq_host
    COMMENT "Installing Dusk 4K EQ 2 LV2 metadata and resources"
    VERBATIM)
add_custom_target(dusk_4keq_bundle DEPENDS "${_dusk_4keq_bundle_stamp}")

lv2_sync_to_jnilibs(dusk_4keq_sync
    "${_dusk_4keq_build}/bin/${_dusk_4keq_bundle}"
    dusk_4keq_build
    INCLUDE_PATTERN "four_k_eq_2(_ui)?\\.so$"
)
add_dependencies(dusk_4keq_sync dusk_4keq_bundle)

add_custom_target(dusk_4keq_done DEPENDS dusk_4keq_sync)
