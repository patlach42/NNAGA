#include "LiveMidiSource.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace guitarrackcraft {
LiveMidiSource::LiveMidiSource(uint64_t handle, UsbMidiPortIdentity identity) noexcept
    : handle_(handle), identity_(std::move(identity)) {}

uint32_t LiveMidiSource::resetEpoch() noexcept {
    uint32_t next = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (next == 0) { epoch_.store(1, std::memory_order_release); next = 1; }
    return next;
}

uint32_t LiveMidiSource::enqueue(const uint64_t* timestamps, const uint32_t* offsets,
                                 const uint32_t* lengths, const uint8_t* payload,
                                 uint32_t count) noexcept {
    if (!timestamps || !offsets || !lengths || !payload || count > kDescriptorCapacity) return 0;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t length = lengths[i], offset = offsets[i];
        if (length == 0 || length > kMaxMidiPayloadBytes ||
            offset > kMaxMidiPayloadBytes || length > kMaxMidiPayloadBytes - offset) {
            ingressDrops_.fetch_add(1, std::memory_order_relaxed); continue;
        }
        const uint64_t readDesc = descriptorRead_.load(std::memory_order_acquire);
        const uint64_t writeDesc = descriptorWrite_.load(std::memory_order_relaxed);
        if (writeDesc - readDesc >= kDescriptorCapacity) {
            ingressDrops_.fetch_add(1, std::memory_order_relaxed); continue;
        }
        uint64_t writeBytes = payloadWrite_.load(std::memory_order_relaxed);
        uint64_t readBytes = payloadRead_.load(std::memory_order_acquire);
        const uint32_t inRing = static_cast<uint32_t>(writeBytes % kPayloadCapacity);
        uint32_t padding =
            inRing + length > kPayloadCapacity ? kPayloadCapacity - inRing : 0;
        if (writeDesc == readDesc && writeBytes == readBytes && padding != 0) {
            writeBytes += padding;
            readBytes = writeBytes;
            payloadRead_.store(readBytes, std::memory_order_release);
            padding = 0;
        }
        const uint64_t required = static_cast<uint64_t>(padding) + length;
        if (writeBytes - readBytes + required > kPayloadCapacity) {
            ingressDrops_.fetch_add(1, std::memory_order_relaxed); continue;
        }
        writeBytes += padding;
        const uint32_t ringOffset = static_cast<uint32_t>(writeBytes % kPayloadCapacity);
        std::memcpy(payload_.data() + ringOffset, payload + offset, length);
        const uint64_t end = writeBytes + length;
        payloadWrite_.store(end, std::memory_order_release);
        descriptors_[writeDesc % kDescriptorCapacity] =
            Descriptor{timestamps[i], ringOffset, length, epoch_.load(std::memory_order_acquire), end};
        descriptorWrite_.store(writeDesc + 1, std::memory_order_release);
        ++accepted;
    }
    return accepted;
}

bool LiveMidiSource::peekDescriptor(Descriptor& out) noexcept {
    const uint64_t read = descriptorRead_.load(std::memory_order_relaxed);
    if (read == descriptorWrite_.load(std::memory_order_acquire)) return false;
    out = descriptors_[read % kDescriptorCapacity];
    return true;
}

void LiveMidiSource::consumeDescriptor(const Descriptor& descriptor) noexcept {
    const uint64_t read = descriptorRead_.load(std::memory_order_relaxed);
    payloadRead_.store(descriptor.end, std::memory_order_release);
    descriptorRead_.store(read + 1, std::memory_order_release);
}
} // namespace guitarrackcraft
