# Use the pinned in-tree ysfx checkout; do not fetch another copy for tests.
set(YSFX_PORTABLE ON CACHE BOOL "" FORCE)
set(YSFX_GFX ON CACHE BOOL "" FORCE)
set(YSFX_PLUGIN OFF CACHE BOOL "" FORCE)
set(YSFX_TESTS OFF CACHE BOOL "" FORCE)
set(YSFX_TOOLS OFF CACHE BOOL "" FORCE)
set(YSFX_SKIP_CHECKSUM ON CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../3rd_party/ysfx
    ${CMAKE_CURRENT_BINARY_DIR}/ysfx)

add_executable(ysfx_smoke_contract_tests
    TestYsfxSmoke.cpp
    TestJsfxPluginContract.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/plugin/PluginChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/jsfx/JsfxPlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/jsfx/JsfxPluginFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/jsfx/JsfxUiHost.cpp)
target_compile_definitions(ysfx_smoke_contract_tests PRIVATE
    NNAGA_SMOKE_PATH="${CMAKE_SOURCE_DIR}/../app/src/main/assets/jsfx/Effects/NNAGA/NNAGA_Smoke.jsfx"
    NNAGA_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures")
target_link_libraries(ysfx_smoke_contract_tests PRIVATE
    ysfx::ysfx GTest::gtest_main pthread)
target_include_directories(ysfx_smoke_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../3rd_party/ysfx/include)

include(GoogleTest)
gtest_discover_tests(ysfx_smoke_contract_tests)
