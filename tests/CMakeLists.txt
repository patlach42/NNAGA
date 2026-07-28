cmake_minimum_required(VERSION 3.22.1)
project(x11_server_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Source directory for X11 modules
set(X11_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/x11)
set(CPP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)

# Platform-independent X11 core modules (no Android deps)
add_library(x11_core STATIC
    ${X11_SRC_DIR}/X11AtomStore.cpp
    ${X11_SRC_DIR}/X11WindowManager.cpp
    ${X11_SRC_DIR}/X11PixmapStore.cpp
    ${X11_SRC_DIR}/X11ConnectionHandler.cpp
    ${X11_SRC_DIR}/X11Framebuffer.cpp
    ${X11_SRC_DIR}/X11PropertyStore.cpp
)
target_include_directories(x11_core PUBLIC
    ${X11_SRC_DIR}
    ${CPP_SRC_DIR}
)

# Unit test executable
add_executable(x11_unit_tests
    x11/TestByteOrder.cpp
    x11/TestAtomStore.cpp
    x11/TestEventBuilder.cpp
    x11/TestWindowManager.cpp
    x11/TestPixmapStore.cpp
    x11/TestConnectionHandshake.cpp
    x11/TestConnectionReply.cpp
    x11/TestFramebuffer.cpp
    x11/TestPropertyStore.cpp
)
target_link_libraries(x11_unit_tests PRIVATE x11_core gtest_main)
# Native direct-USB scheduling contract tests (no Android/libusb I/O).
add_executable(usb_scheduling_tests
    usb/TestUsbScheduling.cpp
)
target_link_libraries(usb_scheduling_tests PRIVATE gtest_main)
target_include_directories(usb_scheduling_tests PRIVATE
    ${CPP_SRC_DIR}/usb
)

enable_testing()
# Allocation-free direct audio callback tests.
add_subdirectory(audio)


# Direct USB duplex lifecycle tests. Keep the host suite usable on machines
# without development libusb headers/libraries.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB QUIET libusb-1.0)
    if(LIBUSB_FOUND)
        add_subdirectory(usb/driver)
    else()
        message(STATUS "USB driver tests: DISABLED (libusb-1.0 not found)")
    endif()
else()
    message(STATUS "USB driver tests: DISABLED (pkg-config not found)")
endif()


# Wire-level integration tests (uses POSIX sockets)
add_executable(x11_wire_tests
    x11/TestWireProtocol.cpp
    x11/TestWireImageOps.cpp
    x11/TestWireGLX.cpp
)
target_link_libraries(x11_wire_tests PRIVATE x11_core gtest_main pthread)
target_include_directories(x11_wire_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/x11)

# Optional: XCB client-level tests (requires libxcb-dev)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(XCB QUIET xcb)
    if(XCB_FOUND)
        add_executable(x11_xcb_tests
            x11/TestXCBClient.cpp
        )
        target_link_libraries(x11_xcb_tests PRIVATE
            x11_core gtest_main pthread ${XCB_LIBRARIES})
        target_include_directories(x11_xcb_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/x11 ${XCB_INCLUDE_DIRS})
        message(STATUS "XCB tests: ENABLED")
    else()
        message(STATUS "XCB tests: DISABLED (libxcb-dev not found)")
    endif()
endif()


# CTest integration
include(GoogleTest)
gtest_discover_tests(x11_unit_tests)
gtest_discover_tests(x11_wire_tests)
if(TARGET x11_xcb_tests)
    gtest_discover_tests(x11_xcb_tests)
endif()
gtest_discover_tests(usb_scheduling_tests)
