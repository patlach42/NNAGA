// SPDX-License-Identifier: GPL-3.0-or-later
// See libusb_uac_driver.h.

#include "libusb_uac_driver.h"
#include "UsbScheduling.h"
#include "utils/ThreadUtils.h"

#include <android/log.h>
#include <libusb.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <utility>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define TAG "LibusbUacDriver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace monotrypt::usb {

namespace {

// USB Audio Class spec constants we don't get from libusb.h.
constexpr uint8_t USB_CLASS_AUDIO       = 0x01;
constexpr uint8_t SUBCLASS_AUDIOCONTROL = 0x01;
constexpr uint8_t SUBCLASS_AUDIOSTREAM  = 0x02;

constexpr uint8_t CS_INTERFACE          = 0x24;
constexpr uint8_t AC_HEADER             = 0x01;
constexpr uint8_t AC_INPUT_TERMINAL     = 0x02;
constexpr uint8_t AC_OUTPUT_TERMINAL    = 0x03;
constexpr uint8_t AC_CLOCK_SOURCE       = 0x0A;
constexpr uint8_t AC_CLOCK_SELECTOR     = 0x0B;
constexpr uint8_t AC_CLOCK_MULTIPLIER   = 0x0C;
constexpr uint8_t AS_GENERAL            = 0x01;
constexpr uint8_t AS_FORMAT_TYPE        = 0x02;
constexpr uint8_t FORMAT_TYPE_I         = 0x01;

// UAC2: SET_CUR on Sample Rate (CS_SAM_FREQ_CONTROL) of a clock
// source entity. UAC1: SET_CUR on SAMPLING_FREQ_CONTROL of the iso
// data endpoint, 3-byte LE rate. Both share bRequest = 0x01.
constexpr uint8_t REQ_SET_CUR              = 0x01;
constexpr uint16_t CS_SAM_FREQ_CONTROL_SEL = 0x01;

// The iD4 profile keeps the same 4 ms kernel runway as eight 0.5 ms batches.
// Short completion intervals reduce steady-state queue granularity; exact
// rational frame accounting avoids the conservative max-packet floor.
constexpr int kDefaultNumTransfers = 4;
constexpr int kId4NumTransfers = 8;
constexpr int kId4PacketsPerTransfer = 4;
constexpr uint16_t kAudientVendorId = 0x2708;
constexpr uint16_t kAudientId4ProductId = 0x0009;
// Playback backlog is deliberately bounded: target watermark plus one max graph block.
// 64 KiB covers 2048 frames at the largest supported 8ch/32-bit format.
constexpr size_t kRingBytes = kPlaybackRingBytes;
constexpr size_t kCaptureRingBytes = kPlaybackRingBytes;


// Produced/consumed bytes count, not frame count. We pack interleaved
// PCM contiguously so byte-level accounting is the natural unit.
inline size_t ringSize(size_t head, size_t tail) {
    // head and tail are monotonic uint64-ish counters masked at access
    // time, so this works correctly across 32-bit wrap.
    return head - tail;
}

bool isClassDescriptor(const uint8_t* p, size_t remaining,
                       uint8_t descType, uint8_t subtype) {
    if (remaining < 3) return false;
    return p[1] == descType && p[2] == subtype;
}

// Walks an `extra` blob (returned by libusb in interface_descriptor /
// config_descriptor) and invokes `cb` for each descriptor in turn.
// `cb(p, len) -> bool` returns true to stop the walk early.
template<typename Cb>
void walkExtra(const uint8_t* extra, int extraLen, Cb&& cb) {
    int i = 0;
    while (i + 2 <= extraLen) {
        int len = extra[i];
        if (len < 2 || i + len > extraLen) break;
        if (cb(extra + i, len)) return;
        i += len;
    }
}

void subtractQueued(std::atomic<uint64_t>& value, uint64_t amount) noexcept {
    uint64_t current = value.load(std::memory_order_relaxed);
    while (current != 0) {
        const uint64_t next = current > amount ? current - amount : 0;
        if (value.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) return;
    }
}
} // namespace
void signalWakeFd(int fd) noexcept {
    if (fd < 0) return;
    eventfd_t one = 1;
    (void)eventfd_write(fd, one);
}
void drainWakeFd(int fd) noexcept {
    if (fd < 0) return;
    eventfd_t value = 0;
    while (eventfd_read(fd, &value) == 0) {}
}
bool pollWakeFd(int fd, int timeoutMs) noexcept {
    if (fd < 0) return false;
    pollfd pfd{fd, POLLIN, 0};
    int result;
    do { result = poll(&pfd, 1, timeoutMs); } while (result < 0 && errno == EINTR);
    return result > 0;
}

LibusbUacDriver::LibusbUacDriver() {
    ring_.resize(kRingBytes);
    ringMask_ = kRingBytes - 1;
    captureRing_.resize(kCaptureRingBytes);
    captureRingMask_ = kCaptureRingBytes - 1;
    captureWakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}
LibusbUacDriver::~LibusbUacDriver() {
    stop();
    close();
    if (ctx_ && (inflight_.load(std::memory_order_acquire) != 0 ||
                 captureInflight_.load(std::memory_order_acquire) != 0)) {
        // Normal control-path teardown is bounded. Destruction is the final
        // ownership barrier: callbacks still reference this object and its
        // buffers, so keep servicing their cancellation instead of freeing
        // callback-visible storage.
        LOGW("waiting for deferred USB cancellations before destruction");
        stopRequested_.store(true, std::memory_order_release);
        for (auto* xfr : transfers_) if (xfr) libusb_cancel_transfer(xfr);
        for (auto* xfr : feedbackTransfers_) if (xfr) libusb_cancel_transfer(xfr);
        for (auto* xfr : captureTransfers_) if (xfr) libusb_cancel_transfer(xfr);
        while (inflight_.load(std::memory_order_acquire) != 0 ||
               captureInflight_.load(std::memory_order_acquire) != 0) {
            timeval tv{0, 5000};
            libusb_handle_events_timeout(ctx_, &tv);
        }
        stop();
        close();
    }
    if (captureWakeFd_ >= 0) {
        ::close(captureWakeFd_);
        captureWakeFd_ = -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

std::string LibusbUacDriver::lastErrorDetail() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastErrorDetail_;
}

std::vector<ClockRateRange> LibusbUacDriver::supportedRates() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return supportedRates_;
}

namespace {

// Members `lastError_` / `lastErrorDetail_` are private, so we route
// through a small lambda created at the failure site. Keeping the
// mechanism inline (rather than a setter method) avoids polluting the
// public surface with something only the implementation needs.

} // namespace

// Sets [code] + [detail], logs at WARN. Internal-only — every call site
// in this file calls it through a lambda capture so the lock + log
// stay co-located with the failure context.
namespace {
struct ErrorSink {
    std::atomic<StartError>* code;
    std::mutex* m;
    std::string* detail;
    void operator()(StartError c, std::string text) const {
        code->store(c, std::memory_order_release);
        std::lock_guard<std::mutex> lock(*m);
        *detail = std::move(text);
        LOGW("start error %d: %s", static_cast<int>(c), detail->c_str());
    }
};
} // namespace

bool LibusbUacDriver::ensureContext() {
    if (contextReady_.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (contextReady_.load(std::memory_order_relaxed)) return true;

    int rc = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, nullptr);
    if (rc != LIBUSB_SUCCESS) {
        LOGW("libusb_set_option(NO_DEVICE_DISCOVERY) -> %d", rc);
    }
    rc = libusb_init(&ctx_);
    if (rc != LIBUSB_SUCCESS) {
        LOGE("libusb_init failed: %d", rc);
        ctx_ = nullptr;
        return false;
    }
    contextReady_.store(true, std::memory_order_release);
    LOGI("libusb context ready");
    return true;
}

bool LibusbUacDriver::open(int fileDescriptor) {
    std::lock_guard<std::recursive_mutex> sessionLock(sessionMutex_);
    if (!ensureContext()) return false;
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transfers_.empty() || !feedbackTransfers_.empty() ||
        !captureTransfers_.empty() ||
        inflight_.load(std::memory_order_acquire) != 0 ||
        captureInflight_.load(std::memory_order_acquire) != 0) {
        lifecycleFailures_.fetch_add(1, std::memory_order_relaxed);
        LOGW("open refused: previous USB transfers are still live");
        return false;
    }
    if (device_ != nullptr && fd_ == fileDescriptor) return true;
    if (device_ != nullptr) {
        libusb_close(device_);
        device_ = nullptr;
        fd_ = -1;
    }
    libusb_device_handle* handle = nullptr;
    int rc = libusb_wrap_sys_device(ctx_,
                                    static_cast<intptr_t>(fileDescriptor),
                                    &handle);
    if (rc != LIBUSB_SUCCESS || handle == nullptr) {
        LOGE("libusb_wrap_sys_device(fd=%d) -> %d", fileDescriptor, rc);
        return false;
    }
    device_ = handle;
    fd_ = fileDescriptor;
    libusb_device_descriptor descriptor{};
    const int descriptorResult = libusb_get_device_descriptor(
        libusb_get_device(device_), &descriptor);
    lowLatencyProfile_ =
        descriptorResult == LIBUSB_SUCCESS &&
        descriptor.idVendor == kAudientVendorId &&
        descriptor.idProduct == kAudientId4ProductId;
    transferCount_ =
        lowLatencyProfile_ ? kId4NumTransfers : kDefaultNumTransfers;
    libusb_set_auto_detach_kernel_driver(device_, 1);
    LOGI("opened device via fd=%d vid=%04x pid=%04x profile=%s",
         fileDescriptor,
         descriptorResult == LIBUSB_SUCCESS ? descriptor.idVendor : 0,
         descriptorResult == LIBUSB_SUCCESS ? descriptor.idProduct : 0,
         lowLatencyProfile_ ? "audient-id4-low-latency" : "generic");
    return true;
}

void LibusbUacDriver::close() {
    stop();
    std::lock_guard<std::recursive_mutex> sessionLock(sessionMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transfers_.empty() || !feedbackTransfers_.empty() ||
        !captureTransfers_.empty() ||
        inflight_.load(std::memory_order_acquire) != 0 ||
        captureInflight_.load(std::memory_order_acquire) != 0) {
        LOGW("close deferred: USB transfers are still live");
        return;
    }
    if (device_ != nullptr) {
        libusb_close(device_);
        device_ = nullptr;
        fd_ = -1;
        format_ = {};
        LOGI("closed device");
    }
}

std::vector<UsbFormatCandidate> LibusbUacDriver::enumerateFormats() {
    std::vector<UsbFormatCandidate> result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) return result;
    libusb_device* dev = libusb_get_device(device_);
    libusb_config_descriptor* config = nullptr;
    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) rc = libusb_get_config_descriptor(dev, 0, &config);
    if (rc != LIBUSB_SUCCESS || !config) return result;

    uint16_t uacVersion = 0x0200;
    uint8_t controlIface = 0xFF;
    std::vector<uint8_t> clocks;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO) continue;
            if (alt.bInterfaceSubClass == SUBCLASS_AUDIOCONTROL) {
                controlIface = alt.bInterfaceNumber;
                walkExtra(alt.extra, alt.extra_length, [&](const uint8_t* p, int len) {
                    if (isClassDescriptor(p, len, CS_INTERFACE, AC_HEADER) && len >= 5)
                        uacVersion = uint16_t(p[3]) | (uint16_t(p[4]) << 8);
                    else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_SOURCE) && len >= 4)
                        clocks.push_back(p[3]);
                    return false;
                });
            }
        }
    }
    auto add = [&](int rate, int bits, int bytes, int channels) {
        if (rate <= 0 || (bits != 16 && bits != 24 && bits != 32) ||
            (bytes != 2 && bytes != 3 && bytes != 4) || channels < 2) return;
        for (const auto& f : result)
            if (f.sampleRateHz == rate && f.bitsPerSample == bits &&
                f.bytesPerSample == bytes && f.channels == channels) return;
        result.push_back({rate, bits, bytes, channels});
    };
    struct Uac2Depth {
        int bits;
        int bytes;
        int channels;
    };
    std::vector<Uac2Depth> uac2Depths;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                alt.bInterfaceSubClass != SUBCLASS_AUDIOSTREAM ||
                alt.bAlternateSetting == 0) continue;
            int channels = 0, bits = 0, bytes = 0;
            const uint8_t* fmt = nullptr; int fmtLen = 0;
            walkExtra(alt.extra, alt.extra_length, [&](const uint8_t* p, int len) {
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_GENERAL) &&
                    uacVersion >= 0x0200 && len >= 16) channels = p[10];
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_FORMAT_TYPE) &&
                    len >= 6 && p[3] == FORMAT_TYPE_I) {
                    fmt = p; fmtLen = len;
                    if (uacVersion >= 0x0200) {
                        bytes = p[4]; bits = p[5];
                    } else if (len >= 8) {
                        channels = p[4]; bytes = p[5]; bits = p[6];
                    }
                }
                return false;
            });
            if (!fmt || channels < 2 || (bits != 16 && bits != 24 && bits != 32) ||
                bytes < (bits + 7) / 8 || bytes > 4) continue;
            const libusb_endpoint_descriptor* out = nullptr;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS &&
                    !(ep.bEndpointAddress & 0x80) &&
                    ((ep.bmAttributes >> 4) & 3) == 0) { out = &ep; break; }
            }
            if (!out) continue;
            if (uacVersion >= 0x0200) uac2Depths.push_back({bits, bytes, channels});
            if (uacVersion < 0x0200) {
                int kind = fmt[7];
                auto rd24 = [](const uint8_t* p) {
                    return int(p[0]) | (int(p[1]) << 8) | (int(p[2]) << 16);
                };
                if (kind == 0 && fmtLen >= 14) {
                    int lo = rd24(fmt + 8), hi = rd24(fmt + 11);
                    add(lo, bits, bytes, channels); if (hi != lo) add(hi, bits, bytes, channels);
                } else {
                    for (int k = 0; k < kind && 8 + 3 * k + 3 <= fmtLen; ++k)
                        add(rd24(fmt + 8 + 3 * k), bits, bytes, channels);
                }
            }
        }
    }
    if (uacVersion >= 0x0200 && controlIface != 0xFF) {
        uint8_t rng[256];
        for (uint8_t id : clocks) {
            int nbytes = libusb_control_transfer(device_, 0xA1, 0x02, 0x0100,
                uint16_t((id << 8) | controlIface), rng, sizeof(rng), 1000);
            if (nbytes < 2) continue;
            int n = rng[0] | (rng[1] << 8);
            for (int k = 0; k < n && 2 + 12 * (k + 1) <= nbytes; ++k) {
                const uint8_t* t = rng + 2 + 12 * k;
                uint32_t lo = uint32_t(t[0]) | (uint32_t(t[1]) << 8) |
                              (uint32_t(t[2]) << 16) | (uint32_t(t[3]) << 24);
                uint32_t hi = uint32_t(t[4]) | (uint32_t(t[5]) << 8) |
                              (uint32_t(t[6]) << 16) | (uint32_t(t[7]) << 24);
                uint32_t step = uint32_t(t[8]) | (uint32_t(t[9]) << 8) |
                                (uint32_t(t[10]) << 16) | (uint32_t(t[11]) << 24);
                // A range is represented by endpoints and, for a small
                // discrete step, its interior values.
                for (uint32_t r = lo; r <= hi && r - lo <= 256u * (step ? step : 1u);
                     r += step ? step : (r == lo ? hi - lo : 0)) {
                    for (const auto& d : uac2Depths)
                        add(static_cast<int>(r), d.bits, d.bytes, d.channels);
                    if (!step || r == hi) break;
                }
            }
        }
    }
    // Some UAC2 interfaces expose the active clock through a terminal or
    // selector rather than a standalone CLOCK_SOURCE. Enumeration cannot
    // safely resolve that topology before claiming the interface, but start()
    // already does. Return conservative standard-rate candidates instead of
    // hiding a perfectly usable device; start() remains authoritative.
    if (uacVersion >= 0x0200 && result.empty() && !uac2Depths.empty()) {
        constexpr int kCommonRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
        for (int rate : kCommonRates) {
            for (const auto& depth : uac2Depths)
                add(rate, depth.bits, depth.bytes, depth.channels);
        }
    }
    libusb_free_config_descriptor(config);
    return result;
}
int LibusbUacDriver::captureChannelCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) return 0;
    libusb_config_descriptor* config = nullptr;
    libusb_device* dev = libusb_get_device(device_);
    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) rc = libusb_get_config_descriptor(dev, 0, &config);
    if (rc != LIBUSB_SUCCESS || !config) return 0;
    int maximum = 0;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const auto& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                alt.bInterfaceSubClass != SUBCLASS_AUDIOSTREAM ||
                alt.bAlternateSetting == 0) continue;
            int channels = 0;
            bool pcm = false;
            bool hasUac2General = false;
            walkExtra(alt.extra, alt.extra_length, [&](const uint8_t* p, int len) {
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_GENERAL) &&
                    len >= 11) {
                    channels = p[10];
                    hasUac2General = true;
                }
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_FORMAT_TYPE) &&
                    len >= 6 && p[3] == FORMAT_TYPE_I) {
                    pcm = true;
                    if (!hasUac2General && len >= 7 && p[4] > 0) channels = p[4];
                }
                return false;
            });
            bool input = false;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS &&
                    (ep.bEndpointAddress & 0x80)) {
                    input = true;
                    break;
                }
            }
            if (input && pcm && channels > maximum) maximum = channels;
        }
    }
    libusb_free_config_descriptor(config);
    return maximum;
}


// --- UAC2 enumeration -------------------------------------------------

bool LibusbUacDriver::selectAltSetting(int sampleRateHz, int bitsPerSample,
                                      int channels, int bytesPerSample,
                                      StreamFormat* out_fmt) {
    libusb_device* dev = libusb_get_device(device_);
    libusb_config_descriptor* config = nullptr;
    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(dev, 0, &config);
    }
    if (rc != LIBUSB_SUCCESS || !config) {
        LOGE("get_config_descriptor -> %d", rc);
        return false;
    }

    // First pass: locate AudioControl interface, decode bcdADC
    // (UAC version) from its Header class-specific descriptor, and
    // for UAC2 inventory every clock-bearing entity (CLOCK_SOURCE /
    // CLOCK_SELECTOR / CLOCK_MULTIPLIER) and every terminal so we
    // can resolve which clock the streaming path actually uses.
    // Bathys (and many other UAC2 DACs) don't expose a standalone
    // CLOCK_SOURCE — the clock is referenced via the Input or
    // Output Terminal's bCSourceID, so naively grabbing the first
    // CLOCK_SOURCE we find leaves clockId=0 → SET_CUR fails with
    // wIndex=0x0001.
    uint8_t controlIface = 0xFF;
    uint16_t uacVersion = 0x0200;
    bool foundControl = false;

    // Per-terminal: (terminalID -> bCSourceID).
    struct TermClock { uint8_t termId; uint8_t clockId; };
    std::vector<TermClock> terminals;
    // All clock-bearing entities, with their subtype so we can
    // distinguish CLOCK_SOURCE (settable rate) from CLOCK_SELECTOR
    // (selects between sources — SET_CUR on it picks pin index, not
    // rate) and CLOCK_MULTIPLIER (derived from another clock).
    struct ClockEntity { uint8_t id; uint8_t subtype; uint8_t baseId; };
    std::vector<ClockEntity> clockEntities;

    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                alt.bInterfaceSubClass != SUBCLASS_AUDIOCONTROL) continue;
            controlIface = alt.bInterfaceNumber;
            walkExtra(alt.extra, alt.extra_length,
                [&](const uint8_t* p, int len) {
                    if (isClassDescriptor(p, len, CS_INTERFACE, AC_HEADER) && len >= 5) {
                        uacVersion = static_cast<uint16_t>(p[3]) |
                                     (static_cast<uint16_t>(p[4]) << 8);
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_SOURCE) && len >= 4) {
                        clockEntities.push_back({p[3], AC_CLOCK_SOURCE, 0});
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_SELECTOR) && len >= 6) {
                        // CLOCK_SELECTOR layout: bClockID @p[3],
                        // bNrInPins @p[4], baCSourceID(1..N) @p[5..].
                        // We grab the first input-pin source as the
                        // "underlying" clock to set the rate on —
                        // SET_CUR on the selector itself selects the
                        // pin index, not the rate.
                        clockEntities.push_back({p[3], AC_CLOCK_SELECTOR, p[5]});
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_MULTIPLIER) && len >= 5) {
                        clockEntities.push_back({p[3], AC_CLOCK_MULTIPLIER, p[4]});
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AC_INPUT_TERMINAL) && len >= 8) {
                        // UAC2 IT: bTerminalID @p[3], bCSourceID @p[7].
                        terminals.push_back({p[3], p[7]});
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AC_OUTPUT_TERMINAL) && len >= 9) {
                        // UAC2 OT: bTerminalID @p[3], bCSourceID @p[8].
                        terminals.push_back({p[3], p[8]});
                    }
                    return false;
                });
            foundControl = true;
            break;
        }
        if (foundControl) break;
    }
    if (!foundControl) {
        LOGE("no AudioControl interface found");
        libusb_free_config_descriptor(config);
        return false;
    }
    LOGI("AudioControl iface %u, UAC version 0x%04x%s — %zu clock entities, %zu terminals",
         controlIface, uacVersion,
         (uacVersion < 0x0200) ? " (UAC1)" : " (UAC2)",
         clockEntities.size(), terminals.size());
    for (const auto& ce : clockEntities) {
        const char* kind =
            ce.subtype == AC_CLOCK_SOURCE ? "SOURCE" :
            ce.subtype == AC_CLOCK_SELECTOR ? "SELECTOR" :
            ce.subtype == AC_CLOCK_MULTIPLIER ? "MULTIPLIER" : "?";
        LOGI("  clock id=%u %s baseId=%u", ce.id, kind, ce.baseId);
    }
    for (const auto& tc : terminals) {
        LOGI("  terminal id=%u → clock id=%u", tc.termId, tc.clockId);
    }

    // Second pass: walk AudioStreaming alt settings and find one
    // matching the requested rate/depth/channels. UAC2 doesn't list
    // rates in AS_FORMAT_TYPE (they're on the clock source via
    // CS_SAM_FREQ_CONTROL GET_RANGE, which we skip — we just attempt
    // SET_CUR and let the device STALL if unsupported).
    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                alt.bInterfaceSubClass != SUBCLASS_AUDIOSTREAM) continue;
            if (alt.bAlternateSetting == 0) continue;   // alt 0 = idle

            int altChannels = 0, altBits = 0, altSubslot = 0;
            uint8_t altTerminalLink = 0;
            bool rateSupportedByDescriptor = (uacVersion >= 0x0200);
            walkExtra(alt.extra, alt.extra_length,
                [&](const uint8_t* p, int len) {
                    if (isClassDescriptor(p, len, CS_INTERFACE, AS_GENERAL)) {
                        // UAC2 AS_GENERAL is 16 bytes and carries
                        // bTerminalLink @p[3] (which terminal this AS
                        // is paired with) and bNrChannels @p[10].
                        if (uacVersion >= 0x0200 && len >= 16) {
                            altTerminalLink = p[3];
                            altChannels = p[10];
                        }
                    } else if (isClassDescriptor(p, len, CS_INTERFACE, AS_FORMAT_TYPE)
                               && len >= 4 && p[3] == FORMAT_TYPE_I) {
                        if (uacVersion >= 0x0200) {
                            // UAC2 FORMAT_TYPE_I is exactly 6 bytes:
                            //   bLength, bDescType, bSubType,
                            //   bFormatType @p[3], bSubslotSize @p[4],
                            //   bBitResolution @p[5].
                            if (len >= 6) {
                                altSubslot = p[4];
                                altBits = p[5];
                            }
                        } else if (len >= 7) {
                            // UAC1 FORMAT_TYPE_I starts at 8 bytes:
                            //   bNrChannels @p[4], bSubframeSize @p[5],
                            //   bBitResolution @p[6], bSamFreqType @p[7],
                            //   then a sample-frequency table (continuous
                            //   range or discrete list of 24-bit LE rates).
                            altChannels = p[4];
                            altSubslot = p[5];
                            altBits = p[6];
                            // Walk the sample-frequency table to
                            // verify the requested rate is supported.
                            // Type=0 means continuous [lo, hi]; other
                            // values are the discrete rate count.
                            //
                            // Also push every advertised rate into
                            // supportedRates_ so the UI can list what
                            // this UAC1 device supports — UAC1 has no
                            // clock entities, so we use clockId=0 as
                            // the synthetic "endpoint clock" marker.
                            // Only do this for alts that match our
                            // requested channels/bits, otherwise an
                            // unrelated 5.1 alt would pollute the list.
                            if (len >= 8 && altChannels == channels
                                && altBits == bitsPerSample) {
                                int kind = p[7];
                                int reqRate = sampleRateHz;
                                if (kind == 0 && len >= 14) {
                                    auto rd24 = [](const uint8_t* q) {
                                        return static_cast<uint32_t>(q[0]) |
                                               (static_cast<uint32_t>(q[1]) << 8) |
                                               (static_cast<uint32_t>(q[2]) << 16);
                                    };
                                    uint32_t lo = rd24(p + 8);
                                    uint32_t hi = rd24(p + 11);
                                    rateSupportedByDescriptor =
                                        (static_cast<uint32_t>(reqRate) >= lo &&
                                         static_cast<uint32_t>(reqRate) <= hi);
                                    std::lock_guard<std::mutex> elock(errorMutex_);
                                    bool dup = false;
                                    for (const auto& e : supportedRates_) {
                                        if (e.minHz == lo && e.maxHz == hi) {
                                            dup = true; break;
                                        }
                                    }
                                    if (!dup) supportedRates_.push_back({0, lo, hi, 0});
                                } else if (kind > 0) {
                                    rateSupportedByDescriptor = false;
                                    std::lock_guard<std::mutex> elock(errorMutex_);
                                    for (int k = 0; k < kind; ++k) {
                                        int off = 8 + k * 3;
                                        if (off + 3 > len) break;
                                        uint32_t hz =
                                            static_cast<uint32_t>(p[off]) |
                                            (static_cast<uint32_t>(p[off + 1]) << 8) |
                                            (static_cast<uint32_t>(p[off + 2]) << 16);
                                        if (static_cast<int>(hz) == reqRate) {
                                            rateSupportedByDescriptor = true;
                                        }
                                        bool dup = false;
                                        for (const auto& e : supportedRates_) {
                                            if (e.minHz == hz && e.maxHz == hz) {
                                                dup = true; break;
                                            }
                                        }
                                        if (!dup) supportedRates_.push_back({0, hz, hz, 0});
                                    }
                                }
                            } else if (len >= 8) {
                                // Channels/bits don't match — keep the
                                // rate-supported check working without
                                // polluting the inventory.
                                int kind = p[7];
                                int reqRate = sampleRateHz;
                                if (kind == 0 && len >= 14) {
                                    auto rd24 = [](const uint8_t* q) {
                                        return static_cast<uint32_t>(q[0]) |
                                               (static_cast<uint32_t>(q[1]) << 8) |
                                               (static_cast<uint32_t>(q[2]) << 16);
                                    };
                                    uint32_t lo = rd24(p + 8);
                                    uint32_t hi = rd24(p + 11);
                                    rateSupportedByDescriptor =
                                        (static_cast<uint32_t>(reqRate) >= lo &&
                                         static_cast<uint32_t>(reqRate) <= hi);
                                } else if (kind > 0) {
                                    rateSupportedByDescriptor = false;
                                    for (int k = 0; k < kind; ++k) {
                                        int off = 8 + k * 3;
                                        if (off + 3 > len) break;
                                        uint32_t hz =
                                            static_cast<uint32_t>(p[off]) |
                                            (static_cast<uint32_t>(p[off + 1]) << 8) |
                                            (static_cast<uint32_t>(p[off + 2]) << 16);
                                        if (static_cast<int>(hz) == reqRate) {
                                            rateSupportedByDescriptor = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    return false;
                });
            LOGI("AS alt if=%u alt=%u extra=%d: terminal=%u channels=%d bits=%d subslot=%d",
                 alt.bInterfaceNumber, alt.bAlternateSetting, alt.extra_length,
                 altTerminalLink, altChannels, altBits, altSubslot);

            if (bytesPerSample > 0 && altSubslot != bytesPerSample) continue;
            if (altChannels != channels || altBits != bitsPerSample) continue;
            if (!rateSupportedByDescriptor) {
                LOGW("alt %u advertises %dch/%db but not %d Hz",
                     alt.bAlternateSetting, channels, bitsPerSample, sampleRateHz);
                continue;
            }

            // Find the OUT iso data EP and (optionally) the IN iso
            // feedback EP. Async UAC2 endpoints have:
            //   bmAttributes bits 1:0 = 01 (iso)
            //                bits 3:2 = 01 (async)
            //                bits 5:4 = 00 data, 01 feedback.
            // Direction lives in bEndpointAddress bit 7 (1 = IN).
            // Also check that the requested rate FITS in the alt's
            // wMaxPacketSize against its bInterval — devices often
            // expose multiple alts (e.g. one for telephony at 24
            // kHz, one for music at 48 kHz+) and naively picking
            // the first one that matches channels/bits leaves us
            // shoving 44.1k samples into packets sized for 24k,
            // which the DAC plays at its own clock rate as
            // pitch-shifted distortion.
            const libusb_endpoint_descriptor* iso = nullptr;
            const libusb_endpoint_descriptor* feedback = nullptr;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) continue;
                bool isIn = (ep.bEndpointAddress & 0x80) != 0;
                uint8_t usage = (ep.bmAttributes >> 4) & 0x03;
                if (!isIn && usage == 0x00 && !iso) iso = &ep;
                else if (isIn && usage == 0x01 && !feedback) feedback = &ep;
            }
            if (!iso) continue;

            bool isHs = libusb_get_device_speed(dev) >= LIBUSB_SPEED_HIGH;
            int microframesPerInterval = isHs
                ? (1 << (iso->bInterval > 0 ? iso->bInterval - 1 : 0))
                : iso->bInterval;     // FS: bInterval is in ms
            int microframesPerSecond = isHs ? 8000 : 1000;
            int packetsPerSec = microframesPerInterval > 0
                ? microframesPerSecond / microframesPerInterval
                : microframesPerSecond;
            int frameStride = (altSubslot ? altSubslot : bitsPerSample / 8) * channels;
            // Admission requires the nominal ceiling packet to fit exactly.
            // A valid endpoint can have no spare frame (for example FS
            // 48 kHz, stereo S16: 48 * 4 = 192 bytes). Fractional-rate
            // headroom is negotiated later only when the physical MPS has it.
            const int nominalFramesPerPacket =
                (sampleRateHz + packetsPerSec - 1) / packetsPerSec;
            const int reqBytesPerPacket = nominalFramesPerPacket * frameStride;
            int actualMps = iso->wMaxPacketSize & 0x07FF;
            int extraTransactions = ((iso->wMaxPacketSize >> 11) & 0x3) + 1;
            int realMps = actualMps * extraTransactions;
            if (reqBytesPerPacket > realMps) {
                LOGW("alt %u (mps=%d, bInterval=%u) can't fit %dHz/%dch/%db "
                     "(needs %d bytes/packet) — skipping",
                     alt.bAlternateSetting, realMps, iso->bInterval,
                     sampleRateHz, channels, bitsPerSample,
                     reqBytesPerPacket);
                continue;
            }

            out_fmt->sampleRateHz = sampleRateHz;
            out_fmt->bitsPerSample = bitsPerSample;
            out_fmt->bytesPerSample = altSubslot ? altSubslot : bitsPerSample / 8;
            out_fmt->channels = channels;
            out_fmt->interfaceNumber = alt.bInterfaceNumber;
            out_fmt->altSetting = alt.bAlternateSetting;
            out_fmt->terminalLink = altTerminalLink;
            out_fmt->endpointAddress = iso->bEndpointAddress;
            out_fmt->syncEndpointAddress = iso->bSynchAddress;
            out_fmt->maxPacketSize = iso->wMaxPacketSize;
            // Resolve the rate-bearing CLOCK_SOURCE entity for this
            // streaming alt by walking the topology graph:
            //   AS_GENERAL.bTerminalLink → terminal → bCSourceID
            //   → if SELECTOR: follow its first input-pin source
            //   → if MULTIPLIER: follow its base-clock-source
            //   → continue until we land on a CLOCK_SOURCE
            // Fall back to first CLOCK_SOURCE in the device, then to
            // any clock entity, then to 0 (skip SET_CUR). The "follow
            // selector" step is what fixes Bathys-style topologies
            // where AS_GENERAL.bTerminalLink → terminal.bCSourceID
            // points at a CLOCK_SELECTOR (subtype 0x0B), and SET_CUR
            // on a Selector picks a pin index — not a sample rate.
            uint8_t resolvedClock = 0;
            for (const auto& tc : terminals) {
                if (tc.termId == altTerminalLink) {
                    resolvedClock = tc.clockId;
                    break;
                }
            }
            // Walk through Selector/Multiplier indirection up to a
            // bounded depth (4 — UAC2 doesn't actually constrain
            // this, but real topologies don't exceed 2). Stops on
            // the first CLOCK_SOURCE.
            for (int hop = 0; hop < 4 && resolvedClock != 0; ++hop) {
                const ClockEntity* ent = nullptr;
                for (const auto& ce : clockEntities) {
                    if (ce.id == resolvedClock) { ent = &ce; break; }
                }
                if (!ent) break;
                if (ent->subtype == AC_CLOCK_SOURCE) break;
                if (ent->baseId == 0) break;
                resolvedClock = ent->baseId;
            }
            if (resolvedClock == 0) {
                for (const auto& ce : clockEntities) {
                    if (ce.subtype == AC_CLOCK_SOURCE) {
                        resolvedClock = ce.id;
                        break;
                    }
                }
            }
            if (resolvedClock == 0 && !clockEntities.empty()) {
                resolvedClock = clockEntities.front().id;
            }
            out_fmt->clockSourceId = resolvedClock;
            out_fmt->controlInterfaceNum = controlIface;
            // Persist every candidate so setSampleRate can fall
            // through them in priority order: SOURCE first (most
            // likely to accept SET_CUR), then SELECTOR/MULTIPLIER,
            // then anything else.
            out_fmt->candidateClockIds.clear();
            for (const auto& ce : clockEntities)
                if (ce.subtype == AC_CLOCK_SOURCE)
                    out_fmt->candidateClockIds.push_back(ce.id);
            for (const auto& ce : clockEntities)
                if (ce.subtype != AC_CLOCK_SOURCE)
                    out_fmt->candidateClockIds.push_back(ce.id);
            out_fmt->isHighSpeed =
                libusb_get_device_speed(dev) >= LIBUSB_SPEED_HIGH;
            out_fmt->bInterval = iso->bInterval;
            out_fmt->uacVersion = uacVersion;
            if (feedback) {
                out_fmt->feedbackEndpointAddress = feedback->bEndpointAddress;
                out_fmt->feedbackMaxPacketSize = feedback->wMaxPacketSize;
                out_fmt->feedbackInterval = feedback->bInterval;
            }
            found = true;
            LOGI("matched alt %u on iface %u: %dch %d-bit, data ep 0x%02x "
                 "mps=%d (real %d) bInterval=%u clockId=%u%s",
                 alt.bAlternateSetting, alt.bInterfaceNumber,
                 channels, bitsPerSample, iso->bEndpointAddress,
                 iso->wMaxPacketSize, realMps, iso->bInterval, resolvedClock,
                 feedback ? " + feedback" : " (no feedback EP)");
            break;
        }
    }

    libusb_free_config_descriptor(config);
    if (!found) {
        LOGE("no AS alt setting matches %dHz/%d-bit/%dch",
             sampleRateHz, bitsPerSample, channels);
    }
    return found;
}

void LibusbUacDriver::captureRangeForClock(uint8_t clockId) {
    if (clockId == 0 || !device_) return;
    uint8_t rng[256] = {0};
    int gr = libusb_control_transfer(
        device_, /*bmRequestType=*/0xA1, /*RANGE=*/0x02,
        static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
        static_cast<uint16_t>(
            (clockId << 8) | format_.controlInterfaceNum),
        rng, sizeof(rng), 1000);
    if (gr < 2) {
        LOGW("captureRangeForClock(%u): GET_RANGE returned %d (no rate "
             "inventory available — UI won't show supported rates)",
             clockId, gr);
        return;
    }
    int n = rng[0] | (rng[1] << 8);
    std::lock_guard<std::mutex> elock(errorMutex_);
    for (int i = 0; i < n && (2 + (i + 1) * 12) <= gr; ++i) {
        const uint8_t* t = rng + 2 + i * 12;
        uint32_t mn = uint32_t(t[0]) | (uint32_t(t[1]) << 8) |
                      (uint32_t(t[2]) << 16) | (uint32_t(t[3]) << 24);
        uint32_t mx = uint32_t(t[4]) | (uint32_t(t[5]) << 8) |
                      (uint32_t(t[6]) << 16) | (uint32_t(t[7]) << 24);
        uint32_t res = uint32_t(t[8]) | (uint32_t(t[9]) << 8) |
                       (uint32_t(t[10]) << 16) | (uint32_t(t[11]) << 24);
        bool dup = false;
        for (const auto& e : supportedRates_) {
            if (e.minHz == mn && e.maxHz == mx && e.resHz == res) {
                dup = true; break;
            }
        }
        if (!dup) supportedRates_.push_back({clockId, mn, mx, res});
    }
    LOGI("captureRangeForClock(%u): %d subrange(s) cached", clockId, n);
}

bool LibusbUacDriver::setSampleRate(uint32_t hz) {
    if (format_.uacVersion >= 0x0200) {
        // UAC2 §5.2.5.1.1 — SET_CUR(SAM_FREQ_CONTROL) on a clock
        // entity, 32-bit LE.
        //   bmRequestType = 0x21 (Class | Interface | Host-to-Device)
        //   wValue        = CS_SAM_FREQ_CONTROL << 8
        //   wIndex        = (clockId << 8) | controlInterfaceNum
        //   wLength       = 4
        // Topology resolution can be non-obvious on some devices
        // (Bathys + 2 clock entities + 4 terminals seen in logs);
        // try the resolved clockSourceId first, then fall through to
        // every other candidate. Same for the GET_CUR readback.
        std::vector<uint8_t> tryIds;
        if (format_.clockSourceId != 0) tryIds.push_back(format_.clockSourceId);
        for (uint8_t id : format_.candidateClockIds) {
            if (id != 0 && id != format_.clockSourceId) tryIds.push_back(id);
        }
        if (tryIds.empty()) {
            LOGW("UAC2: no clock entities at all — skipping SET_CUR "
                 "(assuming fixed-rate clock at %u Hz)", hz);
            return true;
        }

        uint8_t data[4] = {
            static_cast<uint8_t>(hz & 0xFF),
            static_cast<uint8_t>((hz >> 8) & 0xFF),
            static_cast<uint8_t>((hz >> 16) & 0xFF),
            static_cast<uint8_t>((hz >> 24) & 0xFF),
        };
        int rc = -1;
        uint8_t winningId = 0;
        for (uint8_t id : tryIds) {
            int r = libusb_control_transfer(
                device_, 0x21, REQ_SET_CUR,
                static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
                static_cast<uint16_t>(
                    (id << 8) | format_.controlInterfaceNum),
                data, 4, 1000);
            if (r == 4) { rc = r; winningId = id; break; }
            LOGW("UAC2 SET_CUR clockId=%u -> %d (will try next)", id, r);
        }
        if (rc == 4) {
            if (winningId != format_.clockSourceId) {
                LOGI("UAC2: clock entity id=%u accepted SET_CUR (topology "
                     "resolution had picked id=%u — promoting)",
                     winningId, format_.clockSourceId);
                format_.clockSourceId = winningId;
            }
            // Capture the rate inventory on the success path too so
            // the Settings UI can show "supports 44.1 / 48 / 88.2 /
            // 96 / 176.4 / 192 kHz" beneath the active rate. Failures
            // already get this via the diagnostic loop below; on
            // success we only need to ask the winning clock entity.
            captureRangeForClock(winningId);
            return true;
        }
        // STALL or I/O error: many UAC2 DACs (Focal Bathys included)
        // run a fixed-rate hardware clock that doesn't accept
        // programmatic rate changes — the rate is whatever the
        // hardware was wired for, and the host just sends data at
        // that rate. Verify the device's GET_CUR matches what we
        // want; if so, proceed without SET_CUR. If GET_CUR is also
        // a different rate, log a warning but still proceed (audio
        // may pitch-shift, but at least it'll play).
        // UAC2 §5.2.1: bRequest is 0x01 (CUR) for SET and GET both —
        // direction is in bmRequestType (0x21 SET, 0xA1 GET). For
        // each candidate, try GET_CUR; if any reports a non-zero
        // rate that matches our request, proceed (fixed-rate clock
        // case). Also try GET_RANGE so the log shows what rates the
        // device actually claims to support — invaluable diagnostic.
        uint32_t devHz = 0;
        for (uint8_t id : tryIds) {
            uint8_t cur[4] = {0, 0, 0, 0};
            int gc = libusb_control_transfer(
                device_, /*bmRequestType=*/0xA1, /*bRequest=*/REQ_SET_CUR,
                static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
                static_cast<uint16_t>(
                    (id << 8) | format_.controlInterfaceNum),
                cur, 4, 1000);
            uint32_t hzGot = (gc == 4)
                ? (uint32_t(cur[0]) | (uint32_t(cur[1]) << 8) |
                   (uint32_t(cur[2]) << 16) | (uint32_t(cur[3]) << 24))
                : 0;
            // GET_RANGE diagnostic — UAC2 §5.2.1 RANGE attribute.
            //   bRequest=0x02 (RANGE), bmRequestType=0xA1.
            //   Returns wNumSubRanges (LE uint16), then N triples
            //   of (dMIN, dMAX, dRES) each 4-byte LE uint32.
            uint8_t rng[256] = {0};
            int gr = libusb_control_transfer(
                device_, 0xA1, /*RANGE=*/0x02,
                static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
                static_cast<uint16_t>(
                    (id << 8) | format_.controlInterfaceNum),
                rng, sizeof(rng), 1000);
            if (gc == 4 || gr >= 2) {
                LOGI("UAC2 clockId=%u: GET_CUR=%u Hz, GET_RANGE bytes=%d",
                     id, hzGot, gr);
                if (gr >= 2) {
                    int n = rng[0] | (rng[1] << 8);
                    std::lock_guard<std::mutex> elock(errorMutex_);
                    for (int i = 0; i < n && (2 + (i + 1) * 12) <= gr; ++i) {
                        const uint8_t* t = rng + 2 + i * 12;
                        uint32_t mn = uint32_t(t[0]) | (uint32_t(t[1]) << 8) |
                                      (uint32_t(t[2]) << 16) | (uint32_t(t[3]) << 24);
                        uint32_t mx = uint32_t(t[4]) | (uint32_t(t[5]) << 8) |
                                      (uint32_t(t[6]) << 16) | (uint32_t(t[7]) << 24);
                        uint32_t res = uint32_t(t[8]) | (uint32_t(t[9]) << 8) |
                                       (uint32_t(t[10]) << 16) | (uint32_t(t[11]) << 24);
                        LOGI("    range[%d]: %u..%u Hz res=%u", i, mn, mx, res);
                        // Avoid duplicating entries if a previous
                        // candidate clock already reported the same
                        // subrange — happens on devices where the
                        // SELECTOR mirrors the SOURCE's range.
                        bool dup = false;
                        for (const auto& e : supportedRates_) {
                            if (e.minHz == mn && e.maxHz == mx && e.resHz == res) {
                                dup = true; break;
                            }
                        }
                        if (!dup) supportedRates_.push_back({id, mn, mx, res});
                    }
                }
            }
            if (gc == 4 && hzGot == hz) {
                LOGI("UAC2 clockId=%u is fixed at %u Hz already — accepting", id, hz);
                format_.clockSourceId = id;
                return true;
            }
            if (gc == 4 && devHz == 0) devHz = hzGot;  // remember for error msg
        }

        // No candidate accepted SET_CUR and none reports our rate via
        // GET_CUR. Pushing audio at the wrong rate to a fixed-rate
        // clock is the chipmunk-distortion bug we used to ship.
        // Fail bypass cleanly so LibusbAudioSink falls back to the
        // delegate (Android's HAL) which resamples to the device's
        // rate properly.
        LOGE("UAC2: no clock entity accepted %u Hz (best GET_CUR=%u Hz). "
             "Refusing bypass to avoid distortion. Try a track at the "
             "device's native rate; the GET_RANGE log lines above show "
             "what rates each clock supports.",
             hz, devHz);
        return false;
    }
    // UAC1 §5.2.3.2.3.1 — set on the iso DATA endpoint, 24-bit LE.
    //   bmRequestType = 0x22 (Class | Endpoint | Host-to-Device)
    //   bRequest      = SET_CUR (0x01)
    //   wValue        = (SAMPLING_FREQ_CONTROL << 8) | 0  (0x0100)
    //   wIndex        = endpointAddress
    //   wLength       = 3
    uint8_t data[3] = {
        static_cast<uint8_t>(hz & 0xFF),
        static_cast<uint8_t>((hz >> 8) & 0xFF),
        static_cast<uint8_t>((hz >> 16) & 0xFF),
    };
    int rc = libusb_control_transfer(
        device_,
        /*bmRequestType=*/0x22,
        /*bRequest=*/REQ_SET_CUR,
        /*wValue=*/static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
        /*wIndex=*/static_cast<uint16_t>(format_.endpointAddress),
        data, 3,
        /*timeout=*/1000);
    if (rc != 3) {
        // Many UAC1 devices accept the 32-bit form on the endpoint
        // too — try as a fallback before giving up.
        uint8_t data4[4] = { data[0], data[1], data[2], 0 };
        rc = libusb_control_transfer(
            device_, 0x22, REQ_SET_CUR,
            static_cast<uint16_t>(CS_SAM_FREQ_CONTROL_SEL << 8),
            static_cast<uint16_t>(format_.endpointAddress),
            data4, 4, 1000);
        if (rc != 4) {
            LOGE("UAC1 SET_CUR sample rate %u Hz -> %d (3-byte and 4-byte both failed)", hz, rc);
            return false;
        }
    }
    return true;
}

bool LibusbUacDriver::start(int sampleRateHz, int bitsPerSample, int channels,
                            int bytesPerSample) {
    std::lock_guard<std::recursive_mutex> sessionLock(sessionMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    ErrorSink err{&lastError_, &errorMutex_, &lastErrorDetail_};
    // Optimistic clear; failure sites overwrite. Successful return
    // also clears at the bottom.
    lastError_.store(StartError::Ok, std::memory_order_release);
    {
        std::lock_guard<std::mutex> elock(errorMutex_);
        lastErrorDetail_.clear();
        // Drop the previous start's rate inventory — we'll repopulate
        // on this start (or leave empty if the device returns nothing).
        supportedRates_.clear();
    }
    if (captureWakeFd_ < 0) {
        err(StartError::IsoPumpAllocFailed,
            "eventfd creation failed: " + std::string(std::strerror(errno)));
        return false;
    }
    if (!device_) {
        err(StartError::NoDevice, "start() called before open() — "
            "no UsbDeviceConnection wrapped yet");
        return false;
    }
    if (streaming_.load(std::memory_order_acquire) ||
        !transfers_.empty() || !feedbackTransfers_.empty() ||
        !captureTransfers_.empty() || eventThread_.joinable() ||
        inflight_.load(std::memory_order_acquire) != 0 ||
        captureInflight_.load(std::memory_order_acquire) != 0 ||
        captureInterfaceClaimed_) {
        // Reconfigure both halves before mutating shared format/ring state.
        // Keep the playback/control claims, but release capture so the next
        // duplex start can select and claim its matching alternate setting.
        captureActive_.store(false, std::memory_order_release);
        const bool playbackStopped = stopIsoPump();
        const bool captureStopped = stopCapturePump();
        if (!playbackStopped || !captureStopped) {
            err(StartError::IsoPumpSubmitFailed,
                "previous USB transfers did not stop within 500 ms");
            return false;
        }
        if (captureInterfaceClaimed_) {
            if (captureFormat_.altSetting != 0)
                libusb_set_interface_alt_setting(device_, captureFormat_.interfaceNumber, 0);
            libusb_release_interface(device_, claimedCaptureIface_);
            captureInterfaceClaimed_ = false;
            claimedCaptureIface_ = 0xFF;
        }
        captureHead_.store(0, std::memory_order_relaxed);
        captureTail_.store(0, std::memory_order_relaxed);
        captureSequence_.store(0, std::memory_order_relaxed);
        captureOverruns_.store(0, std::memory_order_relaxed);
        metadataFifoOverruns_.store(0, std::memory_order_relaxed);
        captureUnderruns_.store(0, std::memory_order_relaxed);
        streaming_.store(false, std::memory_order_release);
    }

    StreamFormat fmt{};
    if (!selectAltSetting(sampleRateHz, bitsPerSample, channels,
                          bytesPerSample, &fmt)) {
        err(StartError::NoMatchingAlt,
            "no AS alt setting matches " +
            std::to_string(sampleRateHz) + " Hz / " +
            std::to_string(bitsPerSample) + "-bit / " +
            std::to_string(channels) + "ch — DAC may not support "
            "this rate at this bit depth");
        return false;
    }

    // (Re-)claim only when needed: not held yet, or a different
    // interface number on the new alt setting (vanishingly rare —
    // most DACs put all alt settings under one streaming interface).
    bool needClaim = !interfaceClaimed_ ||
                     format_.interfaceNumber != fmt.interfaceNumber;
    if (needClaim) {
        if (interfaceClaimed_) {
            libusb_release_interface(device_, format_.interfaceNumber);
            interfaceClaimed_ = false;
        }
        int rc = libusb_claim_interface(device_, fmt.interfaceNumber);
        if (rc != LIBUSB_SUCCESS) {
            const char* libErr = libusb_strerror(rc);
            LOGE("claim_interface(%u) -> %d (%s) — Developer Options "
                 "'Disable USB audio routing' must be ON, AND the "
                 "older 'USB DAC bit-perfect routing' toggle must be "
                 "OFF (it grabs the device via the framework and "
                 "fights us for the claim)",
                 fmt.interfaceNumber, rc, libErr);
            err(StartError::ClaimInterfaceFailed,
                std::string("libusb_claim_interface(") +
                std::to_string(fmt.interfaceNumber) + ") -> " +
                libErr + ". Most likely Android's audio HAL still " +
                "owns the streaming interface — turn ON Developer " +
                "Options → Disable USB audio routing, and ensure " +
                "the framework-routing toggle (above) is OFF.");
            return false;
        }
        interfaceClaimed_ = true;
    }
    // The AudioControl interface needs to be claimed too —
    // class-specific control transfers (SET_CUR / GET_CUR /
    // GET_RANGE on the clock entities) target it via wIndex's low
    // byte, and the kernel's snd-usb-audio driver silently rejects
    // them with LIBUSB_ERROR_IO if we don't own AC. Without this,
    // the descriptor walker matches the alt fine but every clock
    // request fails -> bypass refuses to engage. Discovered with
    // the Focal Bathys whose topology (2 CLOCK_SOURCEs, 4 terminals)
    // had clock entities looked correct but every SET_CUR returned
    // -1 because the kernel never let the transfer reach the device.
    if (fmt.controlInterfaceNum != 0xFF &&
        (!controlInterfaceClaimed_ ||
         claimedControlIface_ != fmt.controlInterfaceNum)) {
        if (controlInterfaceClaimed_) {
            libusb_release_interface(device_, claimedControlIface_);
            controlInterfaceClaimed_ = false;
        }
        int rc = libusb_claim_interface(device_, fmt.controlInterfaceNum);
        if (rc != LIBUSB_SUCCESS) {
            LOGW("claim AudioControl iface %u -> %d (continuing — "
                 "control transfers may fail with LIBUSB_ERROR_IO)",
                 fmt.controlInterfaceNum, rc);
        } else {
            controlInterfaceClaimed_ = true;
            claimedControlIface_ = fmt.controlInterfaceNum;
            LOGI("claimed AudioControl iface %u", fmt.controlInterfaceNum);
        }
    }
    format_ = fmt;

    int rc = libusb_set_interface_alt_setting(
        device_, format_.interfaceNumber, format_.altSetting);
    if (rc != LIBUSB_SUCCESS) {
        LOGE("set_interface_alt_setting(%u, %u) -> %d",
             format_.interfaceNumber, format_.altSetting, rc);
        err(StartError::SetAltFailed,
            std::string("libusb_set_interface_alt_setting(iface=") +
            std::to_string(format_.interfaceNumber) + ", alt=" +
            std::to_string(format_.altSetting) + ") -> " +
            libusb_strerror(rc));
        return false;
    }
    if (!setSampleRate(static_cast<uint32_t>(sampleRateHz))) {
        err(StartError::SetSampleRateFailed,
            "no clock entity accepted SET_CUR(" +
            std::to_string(sampleRateHz) +
            " Hz) and GET_CUR didn't already report this rate. "
            "Check the GET_RANGE log lines for what rates each "
            "clock supports.");
        return false;
    }

    // Reset the ring before priming the pump.
    ringHead_.store(0, std::memory_order_relaxed);
    ringTail_.store(0, std::memory_order_relaxed);
    writtenFrames_.store(0, std::memory_order_relaxed);
    playedFrames_.store(0, std::memory_order_relaxed);
    playbackOverruns_.store(0, std::memory_order_relaxed);
    playbackUnderruns_.store(0, std::memory_order_relaxed);
    playbackOverrunActive_.store(false, std::memory_order_relaxed);
    playbackUnderrunActive_.store(false, std::memory_order_relaxed);
    playbackStarted_.store(false, std::memory_order_relaxed);
    stopRequested_.store(false, std::memory_order_relaxed);
    transportFailed_.store(false, std::memory_order_relaxed);
    eventThreadUrgentAudio_.store(false, std::memory_order_relaxed);

    if (!deferOutputStart_ && !startIsoPump()) {
        if (lastError_.load(std::memory_order_relaxed) == StartError::Ok) {
            err(StartError::IsoPumpAllocFailed,
                "iso pump failed to start (see logcat for details)");
        }
        return false;
    }
    streaming_.store(!deferOutputStart_, std::memory_order_release);
    LOGI("streaming: %d Hz / %d-bit / %d ch via ep 0x%02x",
         format_.sampleRateHz, format_.bitsPerSample, format_.channels,
         format_.endpointAddress);
    return true;
}
bool LibusbUacDriver::selectCaptureAltSetting(const StreamFormat& playback,
                                              StreamFormat* out_fmt) {
    libusb_config_descriptor* config = nullptr;
    libusb_device* dev = libusb_get_device(device_);
    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) rc = libusb_get_config_descriptor(dev, 0, &config);
    if (rc != LIBUSB_SUCCESS || !config) return false;
    struct TermClock { uint8_t termId; uint8_t clockId; };
    struct ClockEntity { uint8_t id; uint8_t subtype; uint8_t baseId; };
    std::vector<TermClock> terminals;
    std::vector<ClockEntity> clocks;
    if (playback.uacVersion >= 0x0200) {
        for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
            const auto& iface = config->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a) {
                const auto& alt = iface.altsetting[a];
                if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                    alt.bInterfaceSubClass != SUBCLASS_AUDIOCONTROL) continue;
                walkExtra(alt.extra, alt.extra_length, [&](const uint8_t* p, int len) {
                    if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_SOURCE) && len >= 4)
                        clocks.push_back({p[3], AC_CLOCK_SOURCE, 0});
                    else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_SELECTOR) && len >= 6)
                        clocks.push_back({p[3], AC_CLOCK_SELECTOR, p[5]});
                    else if (isClassDescriptor(p, len, CS_INTERFACE, AC_CLOCK_MULTIPLIER) && len >= 5)
                        clocks.push_back({p[3], AC_CLOCK_MULTIPLIER, p[4]});
                    else if (isClassDescriptor(p, len, CS_INTERFACE, AC_INPUT_TERMINAL) && len >= 8)
                        terminals.push_back({p[3], p[7]});
                    else if (isClassDescriptor(p, len, CS_INTERFACE, AC_OUTPUT_TERMINAL) && len >= 9)
                        terminals.push_back({p[3], p[8]});
                    return false;
                });
            }
        }
    }
    const auto resolveClock = [&](uint8_t terminalId) {
        uint8_t clockId = 0;
        for (const auto& terminal : terminals) {
            if (terminal.termId == terminalId) {
                clockId = terminal.clockId;
                break;
            }
        }
        for (int hop = 0; hop < 4 && clockId != 0; ++hop) {
            const ClockEntity* entity = nullptr;
            for (const auto& clock : clocks) {
                if (clock.id == clockId) {
                    entity = &clock;
                    break;
                }
            }
            if (!entity || entity->subtype == AC_CLOCK_SOURCE || entity->baseId == 0) break;
            clockId = entity->baseId;
        }
        return clockId;
    };
    const libusb_interface_descriptor* chosen = nullptr;
    const libusb_endpoint_descriptor* chosenEp = nullptr;
    bool chosenImplicitFeedback = false;
    int chosenChannels = 0;
    uint8_t chosenClock = 0;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const auto& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != USB_CLASS_AUDIO ||
                alt.bInterfaceSubClass != SUBCLASS_AUDIOSTREAM ||
                alt.bAlternateSetting == 0) continue;
            int channels = 0, bits = 0, bytes = 0;
            uint8_t terminalLink = 0;
            walkExtra(alt.extra, alt.extra_length, [&](const uint8_t* p, int len) {
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_GENERAL) &&
                    playback.uacVersion >= 0x0200 && len >= 11) {
                    terminalLink = len >= 16 ? p[3] : 0;
                    channels = p[10];
                }
                if (isClassDescriptor(p, len, CS_INTERFACE, AS_FORMAT_TYPE) &&
                    len >= 6 && p[3] == FORMAT_TYPE_I) {
                    if (playback.uacVersion >= 0x0200) {
                        bytes = p[4]; bits = p[5];
                    } else if (len >= 7) {
                        channels = p[4]; bytes = p[5]; bits = p[6];
                    }
                }
                return false;
            });
            // Prefer a distinct capture terminal; sharing playback's terminal
            // is not duplex topology and risks claiming the wrong direction.
            if (playback.terminalLink != 0 && terminalLink == playback.terminalLink) continue;
            const uint8_t candidateClock = resolveClock(terminalLink);
            if (playback.clockSourceId != 0 && candidateClock != 0 &&
                candidateClock != playback.clockSourceId) continue;
            // Capture channel count is independent of playback: common
            // guitar interfaces expose one or two inputs and stereo output.
            if (channels <= 0 || bits != playback.bitsPerSample ||
                bytes != playback.bytesPerSample) continue;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ||
                    !(ep.bEndpointAddress & 0x80)) continue;
                const uint8_t usage = (ep.bmAttributes >> 4) & 3;
                if (playback.feedbackEndpointAddress == 0 &&
                    playback.syncEndpointAddress != 0 &&
                    ep.bEndpointAddress != playback.syncEndpointAddress) continue;
                if (usage != 0 && usage != 2) continue;
                if (!chosen || (usage == 2 && !chosenImplicitFeedback)) {
                    chosen = &alt;
                    chosenEp = &ep;
                    chosenChannels = channels;
                    chosenImplicitFeedback = usage == 2;
                    chosenClock = candidateClock;
                }
            }
        }
    }
    if (!chosen || !chosenEp) {
        libusb_free_config_descriptor(config);
        return false;
    }
    *out_fmt = playback;
    out_fmt->channels = chosenChannels;
    out_fmt->interfaceNumber = chosen->bInterfaceNumber;
    out_fmt->altSetting = chosen->bAlternateSetting;
    out_fmt->endpointAddress = chosenEp->bEndpointAddress;
    out_fmt->maxPacketSize = chosenEp->wMaxPacketSize;
    out_fmt->clockSourceId = chosenClock;
    out_fmt->bInterval = chosenEp->bInterval;
    out_fmt->feedbackEndpointAddress = 0;
    out_fmt->implicitFeedback = chosenImplicitFeedback;
    libusb_free_config_descriptor(config);
    return true;
}

bool LibusbUacDriver::startDuplex(int sampleRateHz, int bitsPerSample,
                                  int channels, int bytesPerSample) {
    std::lock_guard<std::recursive_mutex> sessionLock(sessionMutex_);
    ErrorSink err{&lastError_, &errorMutex_, &lastErrorDetail_};
    deferOutputStart_.store(true, std::memory_order_release);
    const bool prepared = start(sampleRateHz, bitsPerSample, channels, bytesPerSample);
    deferOutputStart_.store(false, std::memory_order_release);
    if (!prepared) return false;

    bool captureStarted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        StreamFormat capture{};
        if (!selectCaptureAltSetting(format_, &capture)) {
            LOGW("no compatible PCM capture alt setting");
            err(StartError::NoMatchingAlt,
                "no capture alternate setting matches the selected playback format and clock");
        } else if (capture.interfaceNumber == format_.interfaceNumber) {
            LOGW("capture and playback share interface; refusing unsafe duplex claim");
            err(StartError::ClaimInterfaceFailed,
                "capture and playback resolve to the same streaming interface");
        } else {
            const int claim = libusb_claim_interface(device_, capture.interfaceNumber);
            if (claim != LIBUSB_SUCCESS) {
                LOGE("claim capture interface %u -> %d", capture.interfaceNumber, claim);
                err(StartError::ClaimInterfaceFailed,
                    std::string("libusb_claim_interface(capture=") +
                    std::to_string(capture.interfaceNumber) + ") -> " +
                    libusb_strerror(claim));
            } else {
                captureInterfaceClaimed_ = true;
                claimedCaptureIface_ = capture.interfaceNumber;
                captureFormat_ = capture;
                const int alt = libusb_set_interface_alt_setting(
                    device_, capture.interfaceNumber, capture.altSetting);
                if (alt != LIBUSB_SUCCESS) {
                    LOGE("set capture interface %u alt %u -> %d",
                         capture.interfaceNumber, capture.altSetting, alt);
                    err(StartError::SetAltFailed,
                        std::string("set capture alternate setting failed: ") +
                        libusb_strerror(alt));
                } else {
                    captureHead_.store(0, std::memory_order_relaxed);
                    captureTail_.store(0, std::memory_order_relaxed);
                    captureOverruns_.store(0, std::memory_order_relaxed);
                    captureUnderruns_.store(0, std::memory_order_relaxed);
                    captureSequence_.store(0, std::memory_order_relaxed);
                    implicitRead_.store(0, std::memory_order_relaxed);
                    implicitWrite_.store(0, std::memory_order_relaxed);
                    implicitFallbackPackets_.store(0, std::memory_order_relaxed);
                    metadataFifoOverruns_.store(0, std::memory_order_relaxed);
                    captureTransferErrors_.store(0, std::memory_order_relaxed);
                    playbackTransferErrors_.store(0, std::memory_order_relaxed);
                    lifecycleFailures_.store(0, std::memory_order_relaxed);
                    captureStarted = startCapturePump();
                }
            }
        }
    }
    if (captureStarted) {
        if (captureActive_.load(std::memory_order_acquire) &&
            captureFormat_.implicitFeedback &&
            format_.feedbackEndpointAddress == 0) {
            // Capture completions provide the packet clock. Drive them before
            // submitting OUT transfers, then require enough metadata to size
            // every initially queued packet without a nominal fallback.
            eventThread_ = std::thread([this]() {
                eventThreadUrgentAudio_.store(
                    guitarrackcraft::setCurrentThreadUrgentAudio("UsbIsoEvents"),
                    std::memory_order_release);
                while (!stopRequested_.load(std::memory_order_acquire)) {
                    timeval tv{0, 100000};
                    libusb_handle_events_timeout(ctx_, &tv);
                }
            });
            const int startupPacketsPerSecond = packetsPerSecondForInterval(
                format_.isHighSpeed, format_.bInterval);
            const int startupPacketsPerTransfer =
                lowLatencyProfile_ && startupPacketsPerSecond >= 8000
                    ? kId4PacketsPerTransfer
                    : packetsPerTransferForRate(startupPacketsPerSecond);
            const size_t required = static_cast<size_t>(transferCount_) *
                                    static_cast<size_t>(startupPacketsPerTransfer);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (implicitWrite_.load(std::memory_order_acquire) -
                       implicitRead_.load(std::memory_order_acquire) < required &&
                   !stopRequested_.load(std::memory_order_acquire)) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0 || !pollWakeFd(captureWakeFd_, static_cast<int>(remaining))) break;
                drainWakeFd(captureWakeFd_);
            }
            if (implicitWrite_.load(std::memory_order_acquire) -
                    implicitRead_.load(std::memory_order_acquire) < required) {
                LOGE("implicit-feedback capture did not prime output packet metadata");
                err(StartError::IsoPumpSubmitFailed,
                    "implicit-feedback capture did not prime output within 50 ms");
                stop();
                return false;
            }
        }
        // Capture is live; now prepare OUT descriptors. No playback transfer
        // is submitted until startPlayback() has primed the ring.
        if (!startIsoPump(false)) {
            stop();
            return false;
        }
        streaming_.store(true, std::memory_order_release);
        return true;
    }
    // start() has already activated output. A partial duplex startup must
    // fully unwind its claims and transfers; leaving output live would leak
    // the Android-owned interface and make the next attempt permanently BUSY.
    stop();
    return false;
}

bool LibusbUacDriver::startCapturePump() {
    ErrorSink err{&lastError_, &errorMutex_, &lastErrorDetail_};
    const int mps = libusb_get_max_iso_packet_size(
        libusb_get_device(device_), captureFormat_.endpointAddress);
    if (mps <= 0) {
        err(StartError::IsoPumpAllocFailed,
            "capture endpoint has no usable isochronous packet size");
        return false;
    }
    const int captureStride =
        captureFormat_.channels * captureFormat_.bytesPerSample;
    if (captureStride <= 0) {
        err(StartError::IsoPumpAllocFailed,
            "capture format has no usable PCM frame stride");
        return false;
    }
    const int capturePacketsPerSecond = packetsPerSecondForInterval(
        captureFormat_.isHighSpeed, captureFormat_.bInterval);
    capturePacketsPerTransfer_ =
        lowLatencyProfile_ && capturePacketsPerSecond >= 8000
            ? kId4PacketsPerTransfer
            : packetsPerTransferForRate(capturePacketsPerSecond);
    const int packets = capturePacketsPerTransfer_;
    const int captureNominalMax =
        (captureFormat_.sampleRateHz + capturePacketsPerSecond - 1) /
        capturePacketsPerSecond;
    const int capturePhysicalMax = mps / captureStride;
    if (capturePhysicalMax < captureNominalMax) {
        err(StartError::IsoPumpAllocFailed,
            "capture endpoint max packet cannot carry nominal frame cadence");
        return false;
    }
    captureMaxFramesPerPacket_ =
        std::min(capturePhysicalMax, captureNominalMax + 1);
    captureTransferFrames_.store(captureMaxFramesPerPacket_ * packets,
                                  std::memory_order_release);
    captureTransfers_.reserve(transferCount_);
    captureTransferBuffers_.reserve(transferCount_);
    for (int i = 0; i < transferCount_; ++i) {
        libusb_transfer* xfr = libusb_alloc_transfer(packets);
        if (!xfr) {
            err(StartError::IsoPumpAllocFailed,
                "libusb_alloc_transfer returned null for capture");
            stopCapturePump();
            return false;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(mps) * packets);
        libusb_fill_iso_transfer(xfr, device_, captureFormat_.endpointAddress,
                                 buf.data(), buf.size(), packets,
                                 &LibusbUacDriver::onCaptureTrampoline, this, 0);
        libusb_set_iso_packet_lengths(xfr, mps);
        captureTransferBuffers_.emplace_back(std::move(buf));
        captureTransfers_.push_back(xfr);
    }
    captureActive_.store(true, std::memory_order_release);
    for (auto* xfr : captureTransfers_) {
        captureInflight_.fetch_add(1, std::memory_order_acq_rel);
        const int rc = libusb_submit_transfer(xfr);
        if (rc != LIBUSB_SUCCESS) {
            captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
            captureTransferErrors_.fetch_add(1, std::memory_order_relaxed);
            LOGE("capture transfer submit failed: %s endpoint=0x%02x packets=%d packetBytes=%d totalBytes=%zu",
                 libusb_strerror(rc), captureFormat_.endpointAddress, packets, mps,
                 captureTransferBuffers_.empty() ? 0U : captureTransferBuffers_.back().size());
            err(StartError::IsoPumpSubmitFailed,
                std::string("capture transfer submit failed: ") +
                libusb_strerror(rc));
            captureActive_.store(false, std::memory_order_release);
            stopRequested_.store(true, std::memory_order_release);
            stopCapturePump();
            return false;
        }
    }
    return true;
}
bool LibusbUacDriver::stopCapturePump() {
    for (auto* xfr : captureTransfers_) if (xfr) libusb_cancel_transfer(xfr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (captureInflight_.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 5000};
        libusb_handle_events_timeout(ctx_, &tv);
    }
    if (captureInflight_.load(std::memory_order_acquire) != 0) {
        lifecycleFailures_.fetch_add(1, std::memory_order_relaxed);
        LOGW("capture teardown deferred: %d transfers still live",
             captureInflight_.load(std::memory_order_acquire));
        return false;
    }
    for (auto* xfr : captureTransfers_) if (xfr) libusb_free_transfer(xfr);
    captureTransfers_.clear();
    captureTransferBuffers_.clear();
    captureMaxFramesPerPacket_ = 0;
    captureTransferFrames_.store(0, std::memory_order_release);
    return true;
}

void LibusbUacDriver::onCaptureTrampoline(libusb_transfer* xfr) {
    static_cast<LibusbUacDriver*>(xfr->user_data)->onCapture(xfr);
}

void LibusbUacDriver::markTransportFailed() noexcept {
    transportFailed_.store(true, std::memory_order_release);
    captureActive_.store(false, std::memory_order_release);
    streaming_.store(false, std::memory_order_release);
    stopRequested_.store(true, std::memory_order_release);
    signalWakeFd(captureWakeFd_);
}

void LibusbUacDriver::onCapture(libusb_transfer* xfr) {
    if (!captureActive_.load(std::memory_order_acquire)) {
        captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED ||
        stopRequested_.load(std::memory_order_acquire)) {
        captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE) {
        captureTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
        markTransportFailed();
        return;
    }
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
        captureTransferErrors_.fetch_add(1, std::memory_order_relaxed);

    size_t head = captureHead_.load(std::memory_order_relaxed);
    const size_t tail = captureTail_.load(std::memory_order_acquire);
    const size_t capacity = captureRing_.size();
    int totalFrames = 0;
    const int stride = captureFormat_.channels * captureFormat_.bytesPerSample;
    uint8_t* cursor = xfr->buffer;
    for (int i = 0; i < xfr->num_iso_packets; ++i) {
        auto& pkt = xfr->iso_packet_desc[i];
        const int n = pkt.actual_length;
        const bool packetOk = xfr->status == LIBUSB_TRANSFER_COMPLETED &&
                              pkt.status == LIBUSB_TRANSFER_COMPLETED &&
                              n > 0 && stride > 0;
        if (!packetOk)
            captureTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        const bool implicit = captureFormat_.implicitFeedback &&
                              format_.feedbackEndpointAddress == 0;
        if (implicit) {
            const uint16_t packetFrames =
                packetOk ? static_cast<uint16_t>(n / stride) : 0;
            const size_t write =
                implicitWrite_.load(std::memory_order_relaxed);
            size_t read = implicitRead_.load(std::memory_order_acquire);
            while (write - read >= kImplicitFifoCapacity) {
                // Startup priming briefly consumes this FIFO from the
                // control thread. Advance its cursor only if it has not
                // already moved, so an overflow can never rewind the reader.
                if (implicitRead_.compare_exchange_weak(
                        read, write, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    metadataFifoOverruns_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            implicitFrames_[write & (kImplicitFifoCapacity - 1)].store(
                packetFrames, std::memory_order_relaxed);
            implicitWrite_.store(write + 1, std::memory_order_release);
        }
        if (packetOk) {
            const size_t free = capacity - (head - tail);
            const size_t keep = std::min<size_t>(n, free - (free % stride));
            if (keep < static_cast<size_t>(n))
                captureOverruns_.fetch_add(1, std::memory_order_relaxed);
            const size_t off = head & captureRingMask_;
            const size_t first = std::min(keep, capacity - off);
            if (first) std::memcpy(captureRing_.data() + off, cursor, first);
            if (first < keep)
                std::memcpy(captureRing_.data(), cursor + first, keep - first);
            head += keep;
            totalFrames += static_cast<int>(keep / stride);
        }
        cursor += xfr->iso_packet_desc[i].length;
    }
    captureHead_.store(head, std::memory_order_release);
    if (totalFrames > 0)
        captureSequence_.fetch_add(static_cast<uint64_t>(totalFrames),
                                   std::memory_order_release);

    submitPendingImplicitTransfers();
    signalWakeFd(captureWakeFd_);
    if (transportFailed_.load(std::memory_order_acquire)) {
        captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    const int rc = libusb_submit_transfer(xfr);
    if (rc != LIBUSB_SUCCESS) {
        captureInflight_.fetch_sub(1, std::memory_order_acq_rel);
        captureTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        markTransportFailed();
    }
}

int LibusbUacDriver::captureFrameLimit() const noexcept {
    const int stride =
        captureFormat_.channels * captureFormat_.bytesPerSample;
    if (stride <= 0 || captureRing_.empty()) return 0;
    const int graph = graphQuantum_.load(std::memory_order_acquire);
    // A full completion wave plus two newly resubmitted transfers can arrive
    // before the render thread wakes. Size this from the active device profile.
    const int captureTransferFrames =
        std::max(1, capturePacketsPerTransfer_) *
        std::max(0, captureMaxFramesPerPacket_);
    const int queuedCaptureFrames =
        (transferCount_ + 2) * captureTransferFrames;
    // Capture must retain at least the complete playback runway (userspace
    // watermark plus already-submitted USB frames), otherwise a scheduler
    // stall drops input while the DAC still has enough audio to keep playing.
    const int capturePacketsPerSecond = packetsPerSecondForInterval(
        captureFormat_.isHighSpeed, captureFormat_.bInterval);
    const int submittedPlaybackFrames = transferCount_ * nominalTransferFrames(
        captureFormat_.sampleRateHz, capturePacketsPerTransfer_,
        capturePacketsPerSecond);
    const int playbackBudget =
        playbackTargetFrames_.load(std::memory_order_acquire) + graph +
        submittedPlaybackFrames;
    const int latencyLimit =
        std::max(graph * 2, std::max(queuedCaptureFrames + graph,
                                     playbackBudget));
    const int physicalLimit =
        static_cast<int>(captureRing_.size() / static_cast<size_t>(stride));
    return std::min(physicalLimit, latencyLimit);
}

int LibusbUacDriver::readCapturePcm(uint8_t* dst, int frames) {
    if (!captureActive_.load(std::memory_order_acquire)) return 0;
    if (!dst || frames <= 0 || captureFormat_.channels <= 0) return 0;
    const int stride = captureFormat_.channels * captureFormat_.bytesPerSample;
    const size_t want = static_cast<size_t>(frames) * stride;
    const size_t head = captureHead_.load(std::memory_order_acquire);
    size_t tail = captureTail_.load(std::memory_order_relaxed);

    const size_t limitBytes =
        static_cast<size_t>(captureFrameLimit()) * stride;
    if (head - tail > limitBytes) {
        // The consumer owns tail, so it can discard stale capture without
        // violating the SPSC producer's ownership of head. Preserve the
        // newest bounded window after a render stall.
        tail = head - limitBytes;
        captureOverruns_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t n = std::min(want, head - tail);
    n -= n % stride;
    const size_t off = tail & captureRingMask_;
    const size_t first = std::min(n, captureRing_.size() - off);
    if (first) std::memcpy(dst, captureRing_.data() + off, first);
    if (first < n) std::memcpy(dst + first, captureRing_.data(), n - first);
    captureTail_.store(tail + n, std::memory_order_release);
    const int got = static_cast<int>(n / stride);
    if (got < frames)
        captureUnderruns_.fetch_add(1, std::memory_order_relaxed);
    return got;
}

int LibusbUacDriver::discardCaptureFrames(int maxFrames) noexcept {
    if (!captureActive_.load(std::memory_order_acquire) || maxFrames <= 0)
        return 0;
    const int stride = captureFormat_.channels * captureFormat_.bytesPerSample;
    if (stride <= 0) return 0;
    const size_t head = captureHead_.load(std::memory_order_acquire);
    const size_t tail = captureTail_.load(std::memory_order_relaxed);
    const size_t availableFrames =
        (head - tail) / static_cast<size_t>(stride);
    const size_t discarded = std::min(
        availableFrames, static_cast<size_t>(maxFrames));
    captureTail_.store(
        tail + discarded * static_cast<size_t>(stride),
        std::memory_order_release);
    return static_cast<int>(discarded);
}

int LibusbUacDriver::captureAvailableFrames() const {
    if (!captureActive_.load(std::memory_order_acquire)) return 0;
    const int stride = captureFormat_.channels * captureFormat_.bytesPerSample;
    if (stride <= 0) return 0;
    const size_t available =
        (captureHead_.load(std::memory_order_acquire) -
         captureTail_.load(std::memory_order_relaxed)) /
        static_cast<size_t>(stride);
    return static_cast<int>(
        std::min(available, static_cast<size_t>(captureFrameLimit())));
}
bool LibusbUacDriver::waitForCaptureFrames(int frames, int timeoutMs) const {
    if (frames <= 0) return true;
    const auto deadline = timeoutMs > 0
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs)
        : std::chrono::steady_clock::time_point::max();
    while (captureAvailableFrames() < frames &&
           streaming_.load(std::memory_order_acquire)) {
        int waitMs = -1;
        if (timeoutMs > 0) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            waitMs = static_cast<int>(remaining);
        }
        if (!pollWakeFd(captureWakeFd_, waitMs)) break;
        drainWakeFd(captureWakeFd_);
    }
    return captureAvailableFrames() >= frames;
}


void LibusbUacDriver::flushRing() {
    // Lockless reset of the SPSC ring. Producer + consumer both
    // observe head==tail meaning empty on the very next access.
    // Ordering: store tail first, then head — between the two
    // moments, drainRing sees fewer bytes than actually present
    // (worst case: it pads with silence, which is what we want
    // anyway). Other order (head, then tail) could briefly let
    // drainRing think there are huge ring contents that are
    // actually stale.
    ringTail_.store(0, std::memory_order_release);
    ringHead_.store(0, std::memory_order_release);
    // Position counters reset too — flushRing is called between
    // tracks (Media3 flush()), and stale frame counts would let
    // getCurrentPositionUs report frames from the previous track.
    writtenFrames_.store(0, std::memory_order_release);
    playedFrames_.store(0, std::memory_order_release);
}

bool LibusbUacDriver::isStreamingFormat(int sampleRate, int bitsPerSample, int channels) const {
    if (!streaming_.load(std::memory_order_acquire)) return false;
    return format_.sampleRateHz == sampleRate
        && format_.bitsPerSample == bitsPerSample
        && format_.channels == channels;
}

void LibusbUacDriver::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    captureActive_.store(false, std::memory_order_release);
    streaming_.store(false, std::memory_order_release);
    signalWakeFd(captureWakeFd_);
}
void LibusbUacDriver::stop() {
    std::lock_guard<std::recursive_mutex> sessionLock(sessionMutex_);
    requestStop();
    if (transfers_.empty() && captureTransfers_.empty() &&
        !interfaceClaimed_ && !controlInterfaceClaimed_ &&
        !captureInterfaceClaimed_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool playbackStopped = stopIsoPump();
    const bool captureStopped = stopCapturePump();
    if (!playbackStopped || !captureStopped) {
        LOGW("USB teardown deferred until live transfers complete");
        return;
    }
    if (device_ && interfaceClaimed_) {
        if (format_.altSetting != 0)
            libusb_set_interface_alt_setting(device_, format_.interfaceNumber, 0);
        libusb_release_interface(device_, format_.interfaceNumber);
        interfaceClaimed_ = false;
    }
    if (device_ && controlInterfaceClaimed_) {
        libusb_release_interface(device_, claimedControlIface_);
        controlInterfaceClaimed_ = false;
    }
    if (device_ && captureInterfaceClaimed_) {
        if (captureFormat_.altSetting != 0)
            libusb_set_interface_alt_setting(device_, captureFormat_.interfaceNumber, 0);
        libusb_release_interface(device_, claimedCaptureIface_);
        captureInterfaceClaimed_ = false;
        claimedCaptureIface_ = 0xFF;
    }
    captureHead_.store(0, std::memory_order_relaxed);
    captureTail_.store(0, std::memory_order_relaxed);
    captureSequence_.store(0, std::memory_order_relaxed);
    metadataFifoOverruns_.store(0, std::memory_order_relaxed);
    captureUnderruns_.store(0, std::memory_order_relaxed);
    LOGI("stopped streaming (full teardown)");
}

// --- Iso pump ---------------------------------------------------------

bool LibusbUacDriver::startIsoPump(bool submit) {
    auto setErr = [this](StartError c, const std::string& d) {
        lastError_.store(c, std::memory_order_release);
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastErrorDetail_ = d;
    };
    // Compute the actual *packet* rate from bInterval. Naively
    // assuming 1 packet per microframe was the source of distortion
    // when a device's alt uses bInterval > 1 (Bathys' alt 1 is
    // bInterval=4 = 1 packet per ms = 1000 packets/sec, NOT the 8000
    // microframes/sec we'd been pumping at).
    //   HS: packet interval = 2^(bInterval-1) microframes
    //   FS: packet interval = bInterval frames (1ms each)
    const int hostPeriodHz = format_.isHighSpeed ? 8000 : 1000;
    const int intervalExponent = std::min<int>(
        15, format_.bInterval > 0 ? format_.bInterval - 1 : 0);
    packetIntervalUframes_ = format_.isHighSpeed
        ? (1 << intervalExponent)
        : std::max<int>(1, format_.bInterval);
    microframesPerSec_ =
        std::max(1, hostPeriodHz / packetIntervalUframes_);
    playbackPacketsPerTransfer_ =
        lowLatencyProfile_ && microframesPerSec_ >= 8000
            ? kId4PacketsPerTransfer
            : packetsPerTransferForRate(microframesPerSec_);
    int baseFrames = format_.sampleRateHz / microframesPerSec_;
    int rateRemainder = format_.sampleRateHz % microframesPerSec_;
    LOGI("iso pump: %d packets/sec (HS=%d, bInterval=%u), "
         "%d frames/packet base + %d/%d frac",
         microframesPerSec_, format_.isHighSpeed, format_.bInterval,
         baseFrames, rateRemainder, microframesPerSec_);
    uint32_t seed_q16 =
        (static_cast<uint32_t>(baseFrames) << 16) +
        static_cast<uint32_t>(
            (static_cast<uint64_t>(rateRemainder) << 16) /
            static_cast<uint32_t>(microframesPerSec_));
    nominalScheduler_.reset(static_cast<uint32_t>(format_.sampleRateHz),
                            static_cast<uint32_t>(microframesPerSec_));
    framesPerUframe_q16_.store(seed_q16, std::memory_order_relaxed);
    fracAccumulator_q16_ = 0;
    pendingImplicitCount_ = 0;
    const int nominalMaxFrames = baseFrames + (rateRemainder > 0 ? 1 : 0);
    const int frameStride = format_.channels * format_.bytesPerSample;
    const int maxPacket = libusb_get_max_iso_packet_size(
        libusb_get_device(device_), format_.endpointAddress);
    if (frameStride <= 0 || maxPacket <= 0) {
        setErr(StartError::IsoPumpAllocFailed,
               "playback endpoint has no usable isochronous packet size");
        return false;
    }
    const int physicalMaxFrames = maxPacket / frameStride;
    if (physicalMaxFrames < nominalMaxFrames) {
        setErr(StartError::IsoPumpAllocFailed,
               "playback endpoint max packet cannot carry nominal frame cadence");
        return false;
    }
    // Permit at most one feedback-driven frame above nominal while honoring
    // exact-capacity synchronous endpoints. This rejects glitchy feedback and
    // keeps transfer-burst latency independent of an overprovisioned wMaxPacket.
    maxFramesPerPacket_ =
        std::min(physicalMaxFrames, nominalMaxFrames + 1);
    const int maxBytesPerPacket = maxFramesPerPacket_ * frameStride;

    transfers_.reserve(transferCount_);
    transferBuffers_.reserve(transferCount_);

    for (int t = 0; t < transferCount_; ++t) {
        libusb_transfer* xfr = libusb_alloc_transfer(playbackPacketsPerTransfer_);
        if (!xfr) {
            LOGE("libusb_alloc_transfer failed at #%d", t);
            setErr(StartError::IsoPumpAllocFailed,
                   "libusb_alloc_transfer returned null at transfer #" +
                   std::to_string(t) + " — likely OOM");
            stopIsoPump();
            return false;
        }
        // Worst-case sized buffer — every packet is the +1 size.
        // Per-packet `length` (set in onIso) trims to actual.
        std::vector<uint8_t> buf(maxBytesPerPacket * playbackPacketsPerTransfer_, 0);
        libusb_fill_iso_transfer(
            xfr, device_, format_.endpointAddress,
            buf.data(), buf.size(), playbackPacketsPerTransfer_,
            &LibusbUacDriver::onIsoTrampoline, this, /*timeout=*/0);
        // Prime every packet with the same exact rational cadence used after
        // the first completion. At 44.1 kHz, filling all initial packets with
        // the five-frame floor would undersend 16 frames in the first 4 ms.
        for (int packet = 0; packet < playbackPacketsPerTransfer_; ++packet) {
            xfr->iso_packet_desc[packet].length =
                static_cast<unsigned int>(nominalScheduler_.next()) * frameStride;
        }

        transferBuffers_.emplace_back(std::move(buf));
        transfers_.push_back(xfr);
    }

    // The capture endpoint is the clock for an implicit-feedback sink.
    // Initial OUT transfers must use capture-derived packet sizes too.
    if (captureActive_.load(std::memory_order_acquire) &&
        captureFormat_.implicitFeedback &&
        format_.feedbackEndpointAddress == 0) {
        for (libusb_transfer* xfr : transfers_) {
            for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
                size_t read = implicitRead_.load(std::memory_order_acquire);
                for (;;) {
                    const size_t write =
                        implicitWrite_.load(std::memory_order_acquire);
                    if (read == write) {
                        setErr(StartError::IsoPumpSubmitFailed,
                               "implicit-feedback metadata exhausted while priming output");
                        stopIsoPump();
                        return false;
                    }
                    const int frames = std::min<int>(
                        implicitFrames_[read & (kImplicitFifoCapacity - 1)]
                            .load(std::memory_order_relaxed),
                        maxFramesPerPacket_);
                    if (implicitRead_.compare_exchange_weak(
                            read, read + 1, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        xfr->iso_packet_desc[packet].length =
                            frames * frameStride;
                        break;
                    }
                }
            }
        }
    }

    exactInitialPacketFrames_ = 0;
    for (const libusb_transfer* xfr : transfers_) {
        for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
            exactInitialPacketFrames_ +=
                static_cast<int>(xfr->iso_packet_desc[packet].length) / frameStride;
        }
    }
    const int strideFrames = std::max(1, frameStride);
    const int physicalFrames = static_cast<int>(kRingBytes / strideFrames);
    const int maxPrime = std::max(0, physicalFrames -
                                      graphQuantum_.load(std::memory_order_acquire));
    startupPrimeFrames_.store(
        startupPlaybackPrimeFrames(maxPrime, exactInitialPacketFrames_,
        playbackTargetFrames_.load(std::memory_order_acquire),
        graphQuantum_.load(std::memory_order_acquire)),
        std::memory_order_release);

    // Optional feedback EP. UAC2 §5.2.2.4.1: feedback IN, 4 bytes
    // (16.16 fixed) on high-speed, 3 bytes (10.14 fixed) on full-speed.
    constexpr int kFeedbackTransfers = 2;
    if (format_.feedbackEndpointAddress != 0) {
        for (int t = 0; t < kFeedbackTransfers; ++t) {
            libusb_transfer* fxfr = libusb_alloc_transfer(1);
            if (!fxfr) {
                setErr(StartError::IsoPumpAllocFailed,
                       "libusb_alloc_transfer (feedback) returned null");
                stopIsoPump();
                return false;
            }
            const int fbBufSize = format_.feedbackMaxPacketSize > 0
                ? format_.feedbackMaxPacketSize : 4;
            std::vector<uint8_t> fbBuf(fbBufSize, 0);
            libusb_fill_iso_transfer(fxfr, device_, format_.feedbackEndpointAddress,
                                     fbBuf.data(), fbBuf.size(), 1,
                                     &LibusbUacDriver::onFeedbackTrampoline, this, 0);
            libusb_set_iso_packet_lengths(fxfr, fbBufSize);
            feedbackBuffers_.emplace_back(std::move(fbBuf));
            feedbackTransfers_.push_back(fxfr);
        }
    }
    if (!eventThread_.joinable()) {
        eventThread_ = std::thread([this]() {
            eventThreadUrgentAudio_.store(
                guitarrackcraft::setCurrentThreadUrgentAudio("UsbIsoEvents"),
                std::memory_order_release);
            while (!stopRequested_.load(std::memory_order_acquire)) {
                timeval tv{0, 100000};
                libusb_handle_events_timeout(ctx_, &tv);
            }
        });
    }
    if (!submit) return true;
    return submitIsoPump();
}

bool LibusbUacDriver::startPlayback() noexcept {
    bool expected = false;
    if (!playbackStarted_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return false;
    if (!submitIsoPump()) {
        playbackStarted_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}
bool LibusbUacDriver::submitIsoPump() {
    if (stopRequested_.load(std::memory_order_acquire)) {
        LOGE("playback submit rejected: stop already requested");
        return false;
    }
    auto setErr = [this](StartError c, const std::string& d) {
        lastError_.store(c, std::memory_order_release);
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastErrorDetail_ = d;
    };
    // All initial packet payloads are filled before the first submit.
    const int stride = format_.channels * format_.bytesPerSample;
    if (stride <= 0 || startupPrimeFrames() > bufferedFrames()) {
        LOGE("playback submit rejected: stride=%d prime=%d buffered=%d",
             stride, startupPrimeFrames(), bufferedFrames());
        setErr(StartError::IsoPumpSubmitFailed,
               "insufficient ring frames for initial playback prime");
        return false;
    }
    for (libusb_transfer* xfr : transfers_) {
        size_t offset = 0;
        for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
            const int bytes = static_cast<int>(xfr->iso_packet_desc[packet].length);
            drainRing(xfr->buffer + offset, bytes);
            offset += static_cast<size_t>(bytes);
        }
    }
    for (libusb_transfer* fxfr : feedbackTransfers_) {
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        const int rc = libusb_submit_transfer(fxfr);
        if (rc != LIBUSB_SUCCESS) {
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            LOGE("feedback transfer submit failed: %s", libusb_strerror(rc));
            setErr(StartError::IsoPumpSubmitFailed,
                   std::string("feedback transfer submit failed: ") + libusb_strerror(rc));
            stopIsoPump();
            return false;
        }
    }
    for (libusb_transfer* xfr : transfers_) {
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        const int rc = libusb_submit_transfer(xfr);
        if (rc != LIBUSB_SUCCESS) {
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            LOGE("playback transfer submit failed: %s", libusb_strerror(rc));
            setErr(StartError::IsoPumpSubmitFailed,
                   std::string("playback transfer submit failed: ") + libusb_strerror(rc));
            stopIsoPump();
            return false;
        }
        uint64_t frames = 0;
        for (int p = 0; p < xfr->num_iso_packets; ++p)
            frames += static_cast<uint64_t>(xfr->iso_packet_desc[p].length / stride);
        queuedOutFrames_.fetch_add(frames, std::memory_order_acq_rel);
    }
    playbackStarted_.store(true, std::memory_order_release);
    streaming_.store(true, std::memory_order_release);
    return true;
}
bool LibusbUacDriver::stopIsoPump() {
    stopRequested_.store(true, std::memory_order_release);
    for (libusb_transfer* xfr : transfers_) if (xfr) libusb_cancel_transfer(xfr);
    for (libusb_transfer* fxfr : feedbackTransfers_) if (fxfr) libusb_cancel_transfer(fxfr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (inflight_.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 5000};
        libusb_handle_events_timeout(ctx_, &tv);
    }
    if (inflight_.load(std::memory_order_acquire) != 0) {
        lifecycleFailures_.fetch_add(1, std::memory_order_relaxed);
        LOGW("playback teardown deferred: %d transfers still live",
             inflight_.load(std::memory_order_acquire));
        return false;
    }
    if (eventThread_.joinable()) eventThread_.join();
    pendingImplicitCount_ = 0;
    pendingImplicitTransfers_.fill(nullptr);
    for (libusb_transfer* xfr : transfers_) if (xfr) libusb_free_transfer(xfr);
    for (libusb_transfer* fxfr : feedbackTransfers_) if (fxfr) libusb_free_transfer(fxfr);
    transfers_.clear();
    transferBuffers_.clear();
    feedbackTransfers_.clear();
    feedbackBuffers_.clear();
    queuedOutFrames_.store(0, std::memory_order_release);
    playbackStarted_.store(false, std::memory_order_release);
    return true;
}

void LibusbUacDriver::onIsoTrampoline(libusb_transfer* xfr) {
    static_cast<LibusbUacDriver*>(xfr->user_data)->onIso(xfr);
}

void LibusbUacDriver::onFeedbackTrampoline(libusb_transfer* xfr) {
    static_cast<LibusbUacDriver*>(xfr->user_data)->onFeedback(xfr);
}
// Decodes a UAC feedback packet and updates the atomic rate the
// data-EP completion callback reads on its next pass.
//
// High-speed (USB 2.0): 4 bytes, little-endian, 16.16 fixed-point;
// the value is samples per microframe.
//
// Full-speed (USB 1.1): 3 bytes, little-endian, 10.14 fixed-point;
// normalize it to Q16 by shifting left two bits.
//
// onFeedback then scales either normalized value by the data endpoint's
// service interval. Values outside its physical packet capacity are ignored.
void LibusbUacDriver::onFeedback(libusb_transfer* xfr) {
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED ||
        stopRequested_.load(std::memory_order_acquire)) {
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE) {
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        markTransportFailed();
        return;
    }
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED ||
        xfr->num_iso_packets <= 0 ||
        xfr->iso_packet_desc[0].status != LIBUSB_TRANSFER_COMPLETED) {
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
    } else {
        const int actual = xfr->iso_packet_desc[0].actual_length;
        const uint8_t* p = xfr->buffer;
        uint32_t v_q16 = 0;
        if (actual >= 4) {
            v_q16 = static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) |
                    (static_cast<uint32_t>(p[3]) << 24);
        } else if (actual >= 3) {
            v_q16 = (static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16)) << 2;
        }
        const uint64_t packetRateQ16 =
            static_cast<uint64_t>(v_q16) *
            static_cast<uint64_t>(packetIntervalUframes_);
        if (packetRateQ16 > 0 &&
            packetRateQ16 <=
                (static_cast<uint64_t>(maxFramesPerPacket_) << 16)) {
            framesPerUframe_q16_.store(
                static_cast<uint32_t>(packetRateQ16),
                std::memory_order_release);
        }
    }
    const int rc = libusb_submit_transfer(xfr);
    if (rc != LIBUSB_SUCCESS) {
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        markTransportFailed();
    }
}

bool LibusbUacDriver::prepareImplicitTransfer(libusb_transfer* xfr) {
    const size_t count = static_cast<size_t>(xfr->num_iso_packets);
    if (count == 0 || count > kMaxPacketsPerTransfer) return false;
    std::array<int, kMaxPacketsPerTransfer> frameCounts{};
    size_t read = implicitRead_.load(std::memory_order_acquire);
    for (;;) {
        const size_t write = implicitWrite_.load(std::memory_order_acquire);
        if (write - read < count) return false;
        for (size_t packet = 0; packet < count; ++packet) {
            frameCounts[packet] = std::min<int>(
                implicitFrames_[(read + packet) &
                                (kImplicitFifoCapacity - 1)]
                    .load(std::memory_order_relaxed),
                maxFramesPerPacket_);
        }
        if (implicitRead_.compare_exchange_weak(
                read, read + count, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    const int stride = format_.channels * format_.bytesPerSample;
    uint8_t* cursor = xfr->buffer;
    for (size_t packet = 0; packet < count; ++packet) {
        const int bytes = frameCounts[packet] * stride;
        xfr->iso_packet_desc[packet].length = bytes;
        if (bytes > 0) drainRing(cursor, bytes);
        cursor += bytes;
    }
    return true;
}

void LibusbUacDriver::submitPendingImplicitTransfers() {
    size_t submitted = 0;
    while (submitted < pendingImplicitCount_) {
        libusb_transfer* xfr = pendingImplicitTransfers_[submitted];
        if (!prepareImplicitTransfer(xfr)) break;
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        const int rc = libusb_submit_transfer(xfr);
        if (rc != LIBUSB_SUCCESS) {
            playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            markTransportFailed();
            break;
        }
        ++submitted;
        uint64_t frames = 0;
        const int stride = format_.channels * format_.bytesPerSample;
        if (stride > 0)
            for (int p = 0; p < xfr->num_iso_packets; ++p)
                frames += static_cast<uint64_t>(xfr->iso_packet_desc[p].length / stride);
        queuedOutFrames_.fetch_add(frames, std::memory_order_acq_rel);
    }
    if (submitted > 0) {
        for (size_t i = submitted; i < pendingImplicitCount_; ++i)
            pendingImplicitTransfers_[i - submitted] =
                pendingImplicitTransfers_[i];
        pendingImplicitCount_ -= submitted;
    }
}

void LibusbUacDriver::onIso(libusb_transfer* xfr) {
    const int queuedStride = format_.channels * format_.bytesPerSample;
    uint64_t completedFrames = 0;
    if (queuedStride > 0)
        for (int p = 0; p < xfr->num_iso_packets; ++p)
            completedFrames += static_cast<uint64_t>(
                xfr->iso_packet_desc[p].length / queuedStride);
    subtractQueued(queuedOutFrames_, completedFrames);
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED ||
        stopRequested_.load(std::memory_order_acquire)) {
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE) {
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        markTransportFailed();
        return;
    }
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);

    const bool implicit = captureActive_.load(std::memory_order_acquire) &&
                          captureFormat_.implicitFeedback &&
                          format_.feedbackEndpointAddress == 0;
    for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
        const bool packetOk = xfr->status == LIBUSB_TRANSFER_COMPLETED &&
                              xfr->iso_packet_desc[packet].status ==
                                  LIBUSB_TRANSFER_COMPLETED;
        if (!packetOk)
            playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
    }
    if (implicit) {
        if (!prepareImplicitTransfer(xfr)) {
            if (pendingImplicitCount_ < pendingImplicitTransfers_.size()) {
                inflight_.fetch_sub(1, std::memory_order_acq_rel);
                pendingImplicitTransfers_[pendingImplicitCount_++] = xfr;
                return;
            }
            playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            markTransportFailed();
            return;
        }
    } else {
        const int stride = format_.channels * format_.bytesPerSample;
        const uint32_t rate =
            framesPerUframe_q16_.load(std::memory_order_acquire);
        uint8_t* cursor = xfr->buffer;
        for (int packet = 0; packet < xfr->num_iso_packets; ++packet) {
            int frames = format_.feedbackEndpointAddress == 0
                ? static_cast<int>(nominalScheduler_.next())
                : static_cast<int>((fracAccumulator_q16_ += rate) >> 16);
            if (format_.feedbackEndpointAddress != 0)
                fracAccumulator_q16_ &= 0xFFFF;
            if (frames <= 0) frames = static_cast<int>(rate >> 16);
            frames = std::min(frames, maxFramesPerPacket_);
            const int bytes = frames * stride;
            xfr->iso_packet_desc[packet].length = bytes;
            if (bytes > 0) drainRing(cursor, bytes);
            cursor += bytes;
        }
    }
    signalWakeFd(captureWakeFd_);
    const int rc = libusb_submit_transfer(xfr);
    if (rc != LIBUSB_SUCCESS) {
        playbackTransferErrors_.fetch_add(1, std::memory_order_relaxed);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        markTransportFailed();
    }
    else {
        uint64_t frames = 0;
        const int stride = format_.channels * format_.bytesPerSample;
        if (stride > 0)
            for (int p = 0; p < xfr->num_iso_packets; ++p)
                frames += static_cast<uint64_t>(xfr->iso_packet_desc[p].length / stride);
        queuedOutFrames_.fetch_add(frames, std::memory_order_acq_rel);
    }
}

// --- Ring buffer ------------------------------------------------------

int LibusbUacDriver::drainRing(uint8_t* dst, int bytes) {
    size_t head = ringHead_.load(std::memory_order_acquire);
    size_t tail = ringTail_.load(std::memory_order_relaxed);
    size_t available = head - tail;
    int n = static_cast<int>(std::min<size_t>(available, static_cast<size_t>(bytes)));
    if (n > 0) {
        size_t off = tail & ringMask_;
        size_t first = std::min<size_t>(n, kRingBytes - off);
        std::memcpy(dst, ring_.data() + off, first);
        if (first < static_cast<size_t>(n)) {
            std::memcpy(dst + first, ring_.data(), n - first);
        }
        ringTail_.store(tail + n, std::memory_order_release);
    }
    if (n < bytes) {
        // Underrun — pad with silence so the iso packet still ships.
        // The DAC hears a click rather than dropping the entire URB.
        std::memset(dst + n, 0, bytes - n);
        if (playbackStarted_.load(std::memory_order_acquire) &&
            !playbackUnderrunActive_.exchange(true, std::memory_order_acq_rel)) {
            playbackUnderruns_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        playbackUnderrunActive_.store(false, std::memory_order_release);
    }
    // Frames "played" = frames the pump has dispatched, including the
    // silence padding (since the device hears those samples too). Used
    // for accurate position reporting back to ExoPlayer.
    int frameStride = format_.channels * format_.bytesPerSample;
    if (frameStride > 0) {
        playedFrames_.fetch_add(bytes / frameStride, std::memory_order_acq_rel);
    }
    return n;
}

int LibusbUacDriver::writePcm(const uint8_t* data, int frames) {
    if (frames <= 0 || format_.channels == 0) return 0;
    int bytes = frames * format_.channels * format_.bytesPerSample;
    size_t head = ringHead_.load(std::memory_order_relaxed);
    size_t tail = ringTail_.load(std::memory_order_acquire);
    size_t free = kRingBytes - (head - tail);
    int frameStride = format_.channels * format_.bytesPerSample;
    const size_t queuedFrames = frameStride > 0 ? (head - tail) / static_cast<size_t>(frameStride) : 0;
    const bool started = playbackStarted_.load(std::memory_order_acquire);
    const size_t frameLimit = static_cast<size_t>(
        (started ? playbackTargetFrames_.load(std::memory_order_acquire)
                 : startupPrimeFrames_.load(std::memory_order_acquire)) +
        graphQuantum_.load(std::memory_order_acquire));
    if (queuedFrames >= frameLimit) free = 0;
    else free = std::min(free, (frameLimit - queuedFrames) * static_cast<size_t>(frameStride));
    int writable = static_cast<int>(std::min<size_t>(free, static_cast<size_t>(bytes)));
    // Round down to whole frames to avoid splitting a frame across calls.
    if (frameStride > 0) writable -= writable % frameStride;
    if (writable < bytes) {
        if (!playbackOverrunActive_.exchange(true, std::memory_order_acq_rel)) {
            playbackOverruns_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        playbackOverrunActive_.store(false, std::memory_order_release);
    }
    if (writable <= 0) return 0;

    size_t off = head & ringMask_;
    size_t first = std::min<size_t>(writable, kRingBytes - off);
    std::memcpy(ring_.data() + off, data, first);
    if (first < static_cast<size_t>(writable)) {
        std::memcpy(ring_.data(), data + first, writable - first);
    }
    ringHead_.store(head + writable, std::memory_order_release);
    int framesPushed = writable / frameStride;
    writtenFrames_.fetch_add(framesPushed, std::memory_order_acq_rel);
    return framesPushed;
}

int LibusbUacDriver::startupPrimeFrames() const noexcept {
    return startupPrimeFrames_.load(std::memory_order_acquire);
}

int LibusbUacDriver::bufferedFrames() const {
    const int stride = format_.channels * format_.bytesPerSample;
    if (stride <= 0) return 0;
    return static_cast<int>((ringHead_.load(std::memory_order_acquire) -
                             ringTail_.load(std::memory_order_relaxed)) /
                            static_cast<size_t>(stride));
}

void LibusbUacDriver::setGraphQuantum(int frames, int periodMultiplier) {
    const auto config = playbackWatermarkConfig(frames, periodMultiplier);
    const int stride = format_.channels * format_.bytesPerSample;
    const int physicalFrames = stride > 0
        ? static_cast<int>(kRingBytes / static_cast<size_t>(stride))
        : config.frameLimit;
    const int maxTarget = std::max(0, physicalFrames - config.graphQuantum);
    // Generic devices retain their conservative userspace floor. The
    // calibrated iD4 watermark counts only completed-transfer reserve here:
    // its in-flight USB frames are already queued independently at the DAC.
    const int physicalTransferFrames =
        std::max(1, playbackPacketsPerTransfer_) *
        std::max(0, maxFramesPerPacket_);
    const int transferFrames = lowLatencyProfile_
        ? nominalTransferFrames(format_.sampleRateHz,
                                playbackPacketsPerTransfer_,
                                microframesPerSec_)
        : physicalTransferFrames;
    // Six exact 0.5 ms reserve transfers at the default multiplier keep the
    // steady 48 kHz host queue below 9 ms without sacrificing kernel runway.
    const int reserveTransfers =
        clampPeriodMultiplier(periodMultiplier) +
        (lowLatencyProfile_ ? 3 : 1);
    const int watermarkTransfers = playbackWatermarkTransferCount(
        transferCount_, reserveTransfers, lowLatencyProfile_);
    const int queuedTransferFrames = watermarkTransfers * transferFrames;
    const int target = std::min(
        effectivePlaybackTargetFrames(config.targetFrames,
                                      queuedTransferFrames),
        maxTarget);
    const int prime = startupPlaybackPrimeFrames(maxTarget, exactInitialPacketFrames_, target, config.graphQuantum);
    graphQuantum_.store(config.graphQuantum, std::memory_order_release);
    playbackTargetFrames_.store(target, std::memory_order_release);
    startupPrimeFrames_.store(prime, std::memory_order_release);
}

int LibusbUacDriver::writableFrames() const {
    const int frameStride = format_.channels * format_.bytesPerSample;
    if (frameStride <= 0) return 0;
    const size_t head = ringHead_.load(std::memory_order_relaxed);
    const size_t tail = ringTail_.load(std::memory_order_acquire);
    const size_t queuedFrames = (head - tail) / static_cast<size_t>(frameStride);
    const bool started = playbackStarted_.load(std::memory_order_acquire);
    const size_t frameLimit = static_cast<size_t>(
        (started ? playbackTargetFrames_.load(std::memory_order_acquire)
                 : startupPrimeFrames_.load(std::memory_order_acquire)) +
        graphQuantum_.load(std::memory_order_acquire));
    const size_t physicalFree = (kRingBytes - (head - tail)) /
                                static_cast<size_t>(frameStride);
    const size_t logicalFree = queuedFrames < frameLimit ? frameLimit - queuedFrames : 0;
    return static_cast<int>(std::min(physicalFree, logicalFree));
}

bool LibusbUacDriver::waitForWritableFrames(int frames, int timeoutMs) const {
    if (frames <= 0) return true;
    const auto deadline = timeoutMs > 0
        ? std::chrono::steady_clock::now() +
              std::chrono::milliseconds(timeoutMs)
        : std::chrono::steady_clock::time_point::max();
    while (writableFrames() < frames &&
           streaming_.load(std::memory_order_acquire) &&
           !transportFailed_.load(std::memory_order_acquire)) {
        int waitMs = -1;
        if (timeoutMs > 0) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            waitMs = static_cast<int>(remaining);
        }
        if (!pollWakeFd(captureWakeFd_, waitMs)) break;
        drainWakeFd(captureWakeFd_);
    }
    return writableFrames() >= frames;
}

} // namespace monotrypt::usb
