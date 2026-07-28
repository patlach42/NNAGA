# Include this fragment from the host tests project. It compiles the production
# driver unchanged and mocks only the libusb submit boundary, allowing
# deterministic callback/ring/lifecycle tests without hardware.

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

if(NOT DEFINED CPP_SRC_DIR)
    set(CPP_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../app/src/main/cpp)
endif()
include(GoogleTest)

set(USB_DRIVER_TEST_DIR ${CMAKE_CURRENT_LIST_DIR})
add_executable(usb_driver_tests
    ${USB_DRIVER_TEST_DIR}/UsbDriverTests.cpp
    ${USB_DRIVER_TEST_DIR}/MockLibusbApi.cpp
    ${CPP_SRC_DIR}/usb/libusb_uac_driver.cpp
)
target_compile_features(usb_driver_tests PRIVATE cxx_std_17)
target_include_directories(usb_driver_tests PRIVATE
    ${USB_DRIVER_TEST_DIR}
    ${USB_DRIVER_TEST_DIR}/android
    ${CPP_SRC_DIR}
    ${CPP_SRC_DIR}/usb
)
target_link_libraries(usb_driver_tests PRIVATE
    gtest_main
    PkgConfig::LIBUSB
    pthread
)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(usb_driver_tests)
endif()
