set(LV2_TEST_APP_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp)
set(LV2_TEST_INCLUDE_DIR ${LV2_TEST_CORE_INCLUDE_DIR})

add_library(lv2_tiny_gain SHARED
    tiny_gain.c
)
target_include_directories(lv2_tiny_gain PRIVATE ${LV2_TEST_INCLUDE_DIR})
set_target_properties(lv2_tiny_gain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tiny_gain"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bundles/tiny_gain.lv2"
)

configure_file(manifest.ttl "${CMAKE_CURRENT_BINARY_DIR}/bundles/tiny_gain.lv2/manifest.ttl" COPYONLY)
configure_file(tiny_gain.ttl "${CMAKE_CURRENT_BINARY_DIR}/bundles/tiny_gain.lv2/tiny_gain.ttl" COPYONLY)

add_executable(lv2_process_tests
    TestLV2Process.cpp
    ${LV2_TEST_APP_CPP_DIR}/plugin/lv2/LV2Plugin.cpp
    ${LV2_TEST_APP_CPP_DIR}/plugin/lv2/LV2Utils.cpp
)
add_dependencies(lv2_process_tests lv2_tiny_gain)
target_compile_definitions(lv2_process_tests PRIVATE
    HAVE_LV2=1
    LV2_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/bundles"
)
target_include_directories(lv2_process_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LV2_TEST_APP_CPP_DIR}
    ${LV2_TEST_APP_CPP_DIR}/plugin
    ${LV2_TEST_INCLUDE_DIR}
    ${LV2_TEST_LILV_INCLUDE_DIR}
)
target_link_libraries(lv2_process_tests PRIVATE
    GTest::gtest_main
    ${LV2_TEST_LILV_LIB}
    ${LV2_TEST_SRATOM_LIB}
    ${LV2_TEST_SORD_LIB}
    ${LV2_TEST_SERD_LIB}
    ${LV2_TEST_ZIX_LIB}
    dl
    m
    pthread
)

include(GoogleTest)
gtest_discover_tests(lv2_process_tests)
