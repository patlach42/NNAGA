# Host tests for allocation-free audio callback paths.
# Included by tests/CMakeLists.txt after GoogleTest is configured.

set(AUDIO_APP_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp)

add_executable(audio_realtime_tests
    TestRackGraph.cpp
    TestAudioRealtime.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/PluginChain.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/RackGraph.cpp
    ${AUDIO_APP_CPP_DIR}/engine/AudioRecorder.cpp
    ${AUDIO_APP_CPP_DIR}/utils/WavIO.cpp
)

target_include_directories(audio_realtime_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AUDIO_APP_CPP_DIR}
)

target_link_libraries(audio_realtime_tests PRIVATE GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(audio_realtime_tests)
