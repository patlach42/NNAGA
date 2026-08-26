set(CODEC_APP_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp)

add_executable(rack_state_codec_tests
    TestRackStateCodec.cpp
    ${CODEC_APP_CPP_DIR}/plugin/RackStateCodec.cpp
)

target_include_directories(rack_state_codec_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CODEC_APP_CPP_DIR}
    ${CODEC_APP_CPP_DIR}/plugin
)
target_link_libraries(rack_state_codec_tests PRIVATE GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(rack_state_codec_tests)
