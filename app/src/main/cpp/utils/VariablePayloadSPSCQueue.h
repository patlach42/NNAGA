#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarrackcraft {

/** SPSC queue with a bounded descriptor ring and preallocated byte storage. */
class VariablePayloadSPSCQueue {
public:
    VariablePayloadSPSCQueue() = default;
    VariablePayloadSPSCQueue(std::size_t capacity, std::size_t payloadSize) { reset(capacity, payloadSize); }

    void reset(std::size_t capacity, std::size_t payloadSize) {
        capacity_ = capacity;
        payloadSize_ = payloadSize;
        // Keep enough space for a full-size atom and pack smaller records into it.
        byteCapacity_ = (std::max(payloadSize, kDefaultByteCapacity) + (kAlignment - 1)) & ~(kAlignment - 1);
        records_.assign(capacity_, Record{});
        payloads_.assign(byteCapacity_, 0);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        byteRead_.store(0, std::memory_order_relaxed);
        byteWrite_.store(0, std::memory_order_relaxed);
    }

    std::size_t payloadSize() const noexcept { return payloadSize_; }

    template <typename Writer>
    bool tryEmplace(std::size_t size, Writer&& writer) noexcept {
        if (size > payloadSize_ || capacity_ < 2 || byteCapacity_ == 0) return false;
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) return false;

        const std::size_t read = byteRead_.load(std::memory_order_acquire);
        const std::size_t write = byteWrite_.load(std::memory_order_relaxed);
        if (write - read > byteCapacity_) return false;
        std::size_t start = (write + (kAlignment - 1)) & ~(kAlignment - 1);
        const std::size_t offset = start % byteCapacity_;
        const bool empty = (write == read);
        if (size > byteCapacity_ - offset) {
            start += byteCapacity_ - offset; // implicit padding to the next byte-ring lap
        }
        if (empty && start != write) {
            // No unread record exists, so discard alignment/wrap padding from
            // the logical byte history before this new record is published.
            byteRead_.store(start, std::memory_order_release);
        }
        const std::size_t usedBefore = empty ? 0 : (start - read);
        if (usedBefore > byteCapacity_ || size > byteCapacity_ - usedBefore) return false;
        writer(payloads_.data() + (start % byteCapacity_), size);
        records_[tail % capacity_] = Record{start, size};
        byteWrite_.store(start + size, std::memory_order_release);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    template <typename Reader>
    bool consume(Reader&& reader) noexcept {
        if (capacity_ == 0) return false;
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        const Record record = records_[head % capacity_];
        reader(payloads_.data() + (record.start % byteCapacity_), record.size);
        byteRead_.store(record.start + record.size, std::memory_order_release);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    void clear() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t write = byteWrite_.load(std::memory_order_acquire);
        byteRead_.store(write, std::memory_order_release);
        head_.store(tail, std::memory_order_release);
    }

private:
    struct Record {
        std::size_t start = 0;
        std::size_t size = 0;
    };
    static constexpr std::size_t kAlignment = 8;
    static constexpr std::size_t kDefaultByteCapacity = 4u * 1024u * 1024u;
    std::size_t capacity_ = 0;
    std::size_t payloadSize_ = 0;
    std::size_t byteCapacity_ = 0;
    std::vector<Record> records_;
    std::vector<std::uint8_t> payloads_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::size_t> byteRead_{0};
    alignas(64) std::atomic<std::size_t> byteWrite_{0};
};

} // namespace guitarrackcraft
