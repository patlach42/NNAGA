#include <libusb.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

std::atomic<int> gSubmitResult{LIBUSB_SUCCESS};
std::atomic<int> gSubmitCalls{0};
std::atomic<int> gCancelCallbackCalls{0};
std::atomic<int> gMaxIsoPacketSize{28};
std::mutex gTransferMutex;
std::unordered_set<libusb_transfer*> gAcceptedTransfers;
std::vector<std::vector<uint8_t>> gSubmittedPayloads;

// The test executable controls these through the weakly linked test hooks below.
// The transfer callback itself remains production code; only this API boundary
// is mocked so no host USB device or event loop is needed.
extern "C" int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* xfr) {
    gSubmitCalls.fetch_add(1, std::memory_order_relaxed);
    const int result = gSubmitResult.load(std::memory_order_acquire);
    if (result != LIBUSB_SUCCESS) return result;

    std::vector<uint8_t> payload;
    if (xfr && xfr->buffer) {
        for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
            const auto& desc = xfr->iso_packet_desc[packet];
            const auto* begin = xfr->buffer + payload.size();
            payload.insert(payload.end(), begin, begin + desc.length);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gTransferMutex);
        if (xfr) gAcceptedTransfers.insert(xfr);
        gSubmittedPayloads.push_back(std::move(payload));
    }
    return result;
}

extern "C" libusb_device* LIBUSB_CALL
libusb_get_device(libusb_device_handle*) {
    return reinterpret_cast<libusb_device*>(static_cast<uintptr_t>(1));
}
extern "C" void LIBUSB_CALL libusb_exit(libusb_context*) {}
extern "C" void LIBUSB_CALL libusb_close(libusb_device_handle*) {}

extern "C" int LIBUSB_CALL
libusb_get_max_iso_packet_size(libusb_device*, unsigned char) {
    return gMaxIsoPacketSize.load(std::memory_order_acquire);
}

extern "C" int LIBUSB_CALL
libusb_handle_events_timeout(libusb_context*, timeval*) {
    std::this_thread::yield();
    return LIBUSB_SUCCESS;
}

extern "C" int LIBUSB_CALL
libusb_handle_events_timeout_completed(libusb_context*, timeval*, int*) {
    std::this_thread::yield();
    return LIBUSB_SUCCESS;
}
extern "C" int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* xfr) {
    if (!xfr) return LIBUSB_ERROR_NOT_FOUND;

    libusb_transfer_cb_fn callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(gTransferMutex);
        const auto accepted = gAcceptedTransfers.find(xfr);
        if (accepted == gAcceptedTransfers.end())
            return LIBUSB_ERROR_NOT_FOUND;
        gAcceptedTransfers.erase(accepted);
        callback = xfr->callback;
    }

    xfr->status = LIBUSB_TRANSFER_CANCELLED;
    if (callback) {
        gCancelCallbackCalls.fetch_add(1, std::memory_order_relaxed);
        callback(xfr);
    }
    return LIBUSB_SUCCESS;
}

extern "C" void usb_driver_mock_set_submit_result(int result) {
    gSubmitResult.store(result, std::memory_order_release);
}

extern "C" int usb_driver_mock_submit_calls() {
    return gSubmitCalls.load(std::memory_order_acquire);
}
extern "C" int usb_driver_mock_cancel_callback_calls() {
    return gCancelCallbackCalls.load(std::memory_order_acquire);
}

extern "C" void usb_driver_mock_set_max_iso_packet_size(int bytes) {
    gMaxIsoPacketSize.store(bytes, std::memory_order_release);
}

extern "C" int usb_driver_mock_submitted_transfer_count() {
    std::lock_guard<std::mutex> lock(gTransferMutex);
    return static_cast<int>(gSubmittedPayloads.size());
}

extern "C" int usb_driver_mock_submitted_payload_size(int transfer) {
    std::lock_guard<std::mutex> lock(gTransferMutex);
    if (transfer < 0 ||
        transfer >= static_cast<int>(gSubmittedPayloads.size()))
        return 0;
    return static_cast<int>(gSubmittedPayloads[transfer].size());
}

extern "C" uint8_t usb_driver_mock_submitted_payload_byte(int transfer,
                                                            int offset) {
    std::lock_guard<std::mutex> lock(gTransferMutex);
    if (transfer < 0 ||
        transfer >= static_cast<int>(gSubmittedPayloads.size()) ||
        offset < 0 ||
        offset >= static_cast<int>(gSubmittedPayloads[transfer].size()))
        return 0;
    return gSubmittedPayloads[transfer][offset];
}
extern "C" void usb_driver_mock_reset() {
    gSubmitResult.store(LIBUSB_SUCCESS, std::memory_order_release);
    gSubmitCalls.store(0, std::memory_order_release);
    gCancelCallbackCalls.store(0, std::memory_order_release);
    gMaxIsoPacketSize.store(28, std::memory_order_release);
    std::lock_guard<std::mutex> lock(gTransferMutex);
    gAcceptedTransfers.clear();
    gSubmittedPayloads.clear();
}
