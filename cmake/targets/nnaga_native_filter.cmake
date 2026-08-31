set(_nnaga_native_sdk "${THIRD_PARTY}/nnaga-native-plugin-sdk")
if(NOT EXISTS "${_nnaga_native_sdk}/CMakeLists.txt")
    message(FATAL_ERROR "nnaga-native-plugin-sdk submodule is missing")
endif()
add_subdirectory("${_nnaga_native_sdk}" "${CMAKE_BINARY_DIR}/nnaga-native-plugin-sdk" EXCLUDE_FROM_ALL)
set(_nnaga_native_repository_dir "${PROJECT_ROOT}/build/native_plugins/arm64-v8a")
set(_nnaga_native_filter_output "${JNILIBS_DIR}/libnnaga_plugin_filter.so")
set(_nnaga_native_filter_repository_output "${_nnaga_native_repository_dir}/libnnaga_plugin_filter.so")
set(_nnaga_native_shuffle_output "${_nnaga_native_repository_dir}/libnnaga_plugin_shuffle.so")
add_custom_command(
    OUTPUT "${_nnaga_native_filter_output}" "${_nnaga_native_filter_repository_output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}" "${_nnaga_native_repository_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nnaga_plugin_filter>" "${_nnaga_native_filter_output}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nnaga_plugin_filter>" "${_nnaga_native_filter_repository_output}"
    DEPENDS nnaga_plugin_filter
    COMMENT "Staging baseline NNAGA Native filter")
add_custom_command(
    OUTPUT "${_nnaga_native_shuffle_output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_nnaga_native_repository_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nnaga_plugin_shuffle>" "${_nnaga_native_shuffle_output}"
    DEPENDS nnaga_plugin_shuffle
    COMMENT "Staging optional NNAGA Shuffle repository artifact")
add_custom_target(nnaga_native_filter_done DEPENDS
    "${_nnaga_native_filter_output}" "${_nnaga_native_filter_repository_output}")
add_custom_target(nnaga_native_shuffle_done DEPENDS "${_nnaga_native_shuffle_output}")
