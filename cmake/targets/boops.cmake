# BOops LV2 plugin (DSP + the complete upstream X11/Cairo UI).
# Upstream is kept unmodified as a pinned git submodule.

set(_boops_src    "${THIRD_PARTY}/BOops")
set(_boops_build  "${PROJECT_ROOT}/build/boops")
set(_boops_assets "${ASSETS_DIR}/BOops.lv2")

file(MAKE_DIRECTORY "${_boops_build}" "${_boops_assets}")

file(GLOB _boops_ttl RELATIVE "${_boops_src}" "${_boops_src}/*.ttl")
file(GLOB _boops_png RELATIVE "${_boops_src}" "${_boops_src}/inc/*.png")
file(GLOB _boops_wav RELATIVE "${_boops_src}" "${_boops_src}/inc/*.wav")
set(_boops_bundle_files LICENSE ${_boops_ttl} ${_boops_png} ${_boops_wav})
set(_boops_assets_files)
foreach(_file IN LISTS _boops_bundle_files)
    set(_source "${_boops_src}/${_file}")
    set(_destination "${_boops_assets}/${_file}")
    get_filename_component(_destination_dir "${_destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${_destination_dir}")
    configure_file("${_source}" "${_destination}" COPYONLY)
    list(APPEND _boops_assets_files "${_destination}")
endforeach()
add_custom_target(boops_assets DEPENDS ${_boops_assets_files})

set(_boops_dsp_sources
    "${_boops_src}/src/BOops.cpp"
    "${_boops_src}/src/Message.cpp"
    "${_boops_src}/src/BUtilities/stof.cpp"
    "${_boops_src}/src/Slot.cpp"
    "${_boops_src}/src/Airwindows/Galactic.cpp"
    "${_boops_src}/src/Airwindows/Infinity2.cpp"
    "${_boops_src}/src/Airwindows/XRegion.cpp"
)

set(_boops_gui_cpp_sources
    "${_boops_src}/src/BOopsGUI.cpp"
    "${_boops_src}/src/SampleChooser.cpp"
    "${_boops_src}/src/PatternChooser.cpp"
    "${_boops_src}/src/Pattern.cpp"
    "${_boops_src}/src/ShapeWidget.cpp"
    "${_boops_src}/src/SelectWidget.cpp"
    "${_boops_src}/src/ValueSelect.cpp"
    "${_boops_src}/src/DownClick.cpp"
    "${_boops_src}/src/UpClick.cpp"
    "${_boops_src}/src/BWidgets/FileChooser.cpp"
    "${_boops_src}/src/BWidgets/HPianoRoll.cpp"
    "${_boops_src}/src/BWidgets/PianoWidget.cpp"
    "${_boops_src}/src/BWidgets/MessageBox.cpp"
    "${_boops_src}/src/BWidgets/TextToggleButton.cpp"
    "${_boops_src}/src/BWidgets/TextButton.cpp"
    "${_boops_src}/src/BWidgets/DrawingSurface.cpp"
    "${_boops_src}/src/BWidgets/PopupListBox.cpp"
    "${_boops_src}/src/BWidgets/ListBox.cpp"
    "${_boops_src}/src/BWidgets/ChoiceBox.cpp"
    "${_boops_src}/src/BWidgets/ItemBox.cpp"
    "${_boops_src}/src/BWidgets/Text.cpp"
    "${_boops_src}/src/BWidgets/UpButton.cpp"
    "${_boops_src}/src/BWidgets/DownButton.cpp"
    "${_boops_src}/src/BWidgets/ToggleButton.cpp"
    "${_boops_src}/src/BWidgets/Button.cpp"
    "${_boops_src}/src/BWidgets/HSlider.cpp"
    "${_boops_src}/src/BWidgets/HScale.cpp"
    "${_boops_src}/src/BWidgets/Knob.cpp"
    "${_boops_src}/src/BWidgets/RangeWidget.cpp"
    "${_boops_src}/src/BWidgets/ValueWidget.cpp"
    "${_boops_src}/src/BWidgets/ImageIcon.cpp"
    "${_boops_src}/src/BWidgets/Icon.cpp"
    "${_boops_src}/src/BWidgets/Label.cpp"
    "${_boops_src}/src/BWidgets/Window.cpp"
    "${_boops_src}/src/BWidgets/Widget.cpp"
    "${_boops_src}/src/BWidgets/BStyles.cpp"
    "${_boops_src}/src/BWidgets/BColors.cpp"
    "${_boops_src}/src/BWidgets/BItems.cpp"
    "${_boops_src}/src/BUtilities/to_string.cpp"
    "${_boops_src}/src/BUtilities/stof.cpp"
    "${_boops_src}/src/BUtilities/vsystem.cpp"
)
set(_boops_gui_c_sources
    "${_boops_src}/src/screen.c"
    "${_boops_src}/src/BWidgets/cairoplus.c"
    "${_boops_src}/src/BWidgets/pugl/implementation.c"
    "${_boops_src}/src/BWidgets/pugl/x11_stub.c"
    "${_boops_src}/src/BWidgets/pugl/x11_cairo.c"
    "${_boops_src}/src/BWidgets/pugl/x11.c"
)

set(_boops_include_dirs
    "${_boops_src}/src"
    "${_boops_src}/src/BWidgets"
    "${_boops_src}/src/BWidgets/pugl"
    "${LV2_COMPAT_DIR}"
    "${LV2_INCLUDE}"
    "${SNDFILE_PREFIX}/include"
)

add_library(boops_dsp SHARED ${_boops_dsp_sources})
lv2_set_dsp_properties(boops_dsp "BOops" "${_boops_build}")
target_include_directories(boops_dsp PRIVATE ${_boops_include_dirs})
target_link_libraries(boops_dsp PRIVATE "${SNDFILE_PREFIX}/lib/libsndfile.a")
add_dependencies(boops_dsp lv2_libs shared_libsndfile)

add_library(boops_gui SHARED ${_boops_gui_cpp_sources} ${_boops_gui_c_sources})
set_target_properties(boops_gui PROPERTIES
    OUTPUT_NAME "BOopsGUI"
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${_boops_build}"
)
target_include_directories(boops_gui PRIVATE
    ${_boops_include_dirs}
    "${X11_SYSROOT}/include"
    "${X11_SYSROOT}/include/cairo"
)
target_compile_options(boops_gui PRIVATE
    -fPIC -DANDROID -O2 -DNDEBUG -fvisibility=hidden
    -DPUGL_HAVE_CAIRO
    -Wno-unused-parameter -Wno-unused-result
    "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
    "$<$<COMPILE_LANGUAGE:C>:-std=c17>"
)
target_compile_definitions(boops_gui PRIVATE PUGL_HAVE_CAIRO)
target_link_options(boops_gui PRIVATE
    -shared -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,noexecstack
    -Wl,--no-undefined
)
target_link_libraries(boops_gui PRIVATE
    "${SNDFILE_PREFIX}/lib/libsndfile.a"
    "${X11_SYSROOT}/lib/libcairo.a"
    "${X11_SYSROOT}/lib/libpixman-1.a"
    "${X11_SYSROOT}/lib/libpng.a"
    -lz
    -L"${X11_SYSROOT}/lib" X11 xcb Xau Xrender
    xshm_stub m log dl
)
add_dependencies(boops_gui boops_assets lv2_libs shared_libsndfile x11_sysroot)
lv2_sync_to_jnilibs(
    boops_sync
    "${_boops_build}"
    "boops_dsp;boops_gui"
    INCLUDE_PATTERN "^BOops(GUI)?\\.so$"
)
add_custom_target(boops_done DEPENDS boops_sync boops_assets)

