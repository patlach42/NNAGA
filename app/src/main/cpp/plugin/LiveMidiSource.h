#pragma once

#include "IPlugin.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace guitarrackcraft {

struct UsbMidiPortIdentity {
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    std::string serialNumber;
    uint16_t portNumber = 0;
    bool operator==(const UsbMidiPortIdentity& other) const noexcept {
        return vendorId == other.vendorId && productId == other.productId &&
               portNumber == other.portNumber && serialNumber == other.serialNumber;
    }
};

class LiveMidiSource final {
public:
    static constexpr uint32_t kDescriptorCapacity = 256;
    static constexpr uint32_t kPayloadCapacity = 256u * 1024u;

    explicit LiveMidiSource(uint64_t handle, UsbMidiPortIdentity identity) noexcept;
    const UsbMidiPortIdentity& identity() const noexcept { return identity_; }
    uint64_t handle() const noexcept { return handle_; }
    uint32_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
    uint32_t lastDrainedEpoch() const noexcept { return lastDrainedEpoch_; }
    uint32_t resetEpoch() noexcept;
    void flush() noexcept { resetEpoch(); }

    uint32_t enqueue(const uint64_t* timestamps, const uint32_t* offsets,
                     const uint32_t* lengths, const uint8_t* payload,
                     uint32_t count) noexcept;
    template <typename DropFn>
    uint32_t drain(uint32_t frameCount, uint64_t nowNanos, double sampleRate,
                   MidiBuffer& destination, DropFn&& onDrop) noexcept {
        const uint32_t drainEpoch = epoch_.load(std::memory_order_acquire);
        lastDrainedEpoch_ = drainEpoch;
        if (frameCount == 0 || sampleRate <= 0.0) {
            Descriptor descriptor{};
            while (peekDescriptor(descriptor)) consumeDescriptor(descriptor);
            return 0;
        }
        const uint64_t window = static_cast<uint64_t>(
            (static_cast<double>(frameCount) * 1'000'000'000.0) / sampleRate);
        const uint64_t start = nowNanos > window ? nowNanos - window : 0;
        uint32_t drained = 0;
        Descriptor descriptor{};
        while (peekDescriptor(descriptor)) {
            if (descriptor.epoch != drainEpoch) {
                consumeDescriptor(descriptor);
                ++drained;
                continue;
            }
            if (descriptor.timestamp >= nowNanos) break;

            uint32_t frame = 0;
            if (descriptor.timestamp < start) {
                onDrop(true);
            } else {
                const uint64_t delta = descriptor.timestamp - start;
                frame = static_cast<uint32_t>(std::min<uint64_t>(
                    frameCount - 1,
                    static_cast<uint64_t>(
                        static_cast<long double>(delta) * sampleRate /
                        1'000'000'000.0L)));
            }
            if (!destination.append(
                    frame, payload_.data() + descriptor.offset, descriptor.length)) {
                onDrop(false);
            }
            consumeDescriptor(descriptor);
            ++drained;
        }
        return drained;
    }
    uint64_t dropped() const noexcept { return ingressDrops_.load(std::memory_order_relaxed); }
    uint64_t takeDropped() noexcept { return ingressDrops_.exchange(0, std::memory_order_acq_rel); }

private:
    struct Descriptor { uint64_t timestamp; uint32_t offset; uint32_t length; uint32_t epoch; uint64_t end; };
    bool peekDescriptor(Descriptor& out) noexcept;
    void consumeDescriptor(const Descriptor& descriptor) noexcept;
    const uint64_t handle_;
    const UsbMidiPortIdentity identity_;
    alignas(64) std::array<Descriptor, kDescriptorCapacity> descriptors_{};
    alignas(64) std::array<uint8_t, kPayloadCapacity> payload_{};
    alignas(64) std::atomic<uint64_t> descriptorWrite_{0};
    alignas(64) std::atomic<uint64_t> descriptorRead_{0};
    alignas(64) std::atomic<uint64_t> payloadWrite_{0};
    alignas(64) std::atomic<uint64_t> payloadRead_{0};
    alignas(64) std::atomic<uint32_t> epoch_{1};
    uint32_t lastDrainedEpoch_ = 0;
    std::atomic<uint64_t> ingressDrops_{0};
};
}
