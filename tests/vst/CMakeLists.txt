# Host-native VST shared-memory ABI and transport regression tests.
set(VSTHOST_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../vsthost_lib/src/main/cpp)

add_executable(vst_transport_tests
    TestVstTransport.cpp
    ${VSTHOST_CPP_DIR}/ipc/SharedRing.cpp
)
add_executable(vst_parameter_tests
    TestVstParameters.cpp
    ${VSTHOST_CPP_DIR}/ipc/SharedRing.cpp
)

target_include_directories(vst_parameter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VSTHOST_CPP_DIR}
)

target_link_libraries(vst_parameter_tests PRIVATE GTest::gtest_main pthread)

add_executable(wine_plugin_path_tests
    TestWinePluginPath.cpp
)

target_include_directories(wine_plugin_path_tests PRIVATE
    ${VSTHOST_CPP_DIR}
)

target_link_libraries(wine_plugin_path_tests PRIVATE GTest::gtest_main pthread)


target_include_directories(vst_transport_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VSTHOST_CPP_DIR}
)

# shared_layout.h maps its C11 _Alignas spelling to the C++ keyword itself.
target_link_libraries(vst_transport_tests PRIVATE GTest::gtest_main pthread)


include(GoogleTest)
gtest_discover_tests(vst_transport_tests)
gtest_discover_tests(vst_parameter_tests)
gtest_discover_tests(wine_plugin_path_tests)
