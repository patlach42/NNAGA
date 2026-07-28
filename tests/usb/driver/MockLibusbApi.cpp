#include <libusb.h>

#include <atomic>

namespace {
std::atomic<int> gSubmitResult{LIBUSB_SUCCESS};
std::atomic<int> gSubmitCalls{0};
}

// The test executable controls these through the weakly linked test hooks below.
// The transfer callback itself remains production code; only this API boundary
// is mocked so no host USB device or event loop is needed.
extern "C" int LIBUSB_CALL libusb_submit_transfer(libusb_transfer*) {
    gSubmitCalls.fetch_add(1, std::memory_order_relaxed);
    return gSubmitResult.load(std::memory_order_acquire);
}

extern "C" void usb_driver_mock_set_submit_result(int result) {
    gSubmitResult.store(result, std::memory_order_release);
}

extern "C" int usb_driver_mock_submit_calls() {
    return gSubmitCalls.load(std::memory_order_acquire);
}

extern "C" void usb_driver_mock_reset() {
    gSubmitResult.store(LIBUSB_SUCCESS, std::memory_order_release);
    gSubmitCalls.store(0, std::memory_order_release);
}
