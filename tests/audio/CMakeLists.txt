# Host tests for allocation-free audio callback paths.
# Included by tests/CMakeLists.txt after GoogleTest is configured.

set(AUDIO_APP_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp)

add_executable(audio_realtime_tests
    TestRackGraph.cpp
    TestAudioRealtime.cpp
    TestAudioCpuAffinity.cpp
    TestWavIO.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/PluginChain.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/RackGraph.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/PluginRegistry.cpp
    ${AUDIO_APP_CPP_DIR}/utils/WavIO.cpp
)

target_include_directories(audio_realtime_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AUDIO_APP_CPP_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../liblowlatencyaudio/include
)
target_link_libraries(audio_realtime_tests PRIVATE GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(audio_realtime_tests)
add_executable(plugin_chain_lifecycle_tests
    TestPluginChainLifecycle.cpp
    ${AUDIO_APP_CPP_DIR}/plugin/PluginChain.cpp
)

target_include_directories(plugin_chain_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AUDIO_APP_CPP_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../liblowlatencyaudio/include
)
target_link_libraries(plugin_chain_lifecycle_tests PRIVATE GTest::gtest_main pthread)

gtest_discover_tests(plugin_chain_lifecycle_tests)
