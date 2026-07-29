# Host-native VST shared-memory ABI and transport regression tests.
set(VSTHOST_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../vsthost_lib/src/main/cpp)

add_executable(vst_transport_tests
    TestVstTransport.cpp
    ${VSTHOST_CPP_DIR}/ipc/SharedRing.cpp
)

target_include_directories(vst_transport_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VSTHOST_CPP_DIR}
)

# shared_layout.h is C-compatible and uses _Alignas; map the C spelling to
# the C++ keyword for this host-only target without modifying production code.
target_compile_definitions(vst_transport_tests PRIVATE _Alignas=alignas)
target_link_libraries(vst_transport_tests PRIVATE GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(vst_transport_tests)
