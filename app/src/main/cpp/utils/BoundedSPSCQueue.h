#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace guitarrackcraft {

// Single-producer/single-consumer bounded mailbox. push/pop never block or allocate.
template <typename T, std::size_t Capacity>
class BoundedSPSCQueue {
    static_assert(Capacity > 1, "queue capacity must exceed one");
    static_assert(std::is_trivially_copyable<T>::value, "queue payload must be trivially copyable");
public:
    bool push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) % Capacity;
        if (next == head_.load(std::memory_order_acquire)) return false;
        slots_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        value = slots_[head];
        head_.store((head + 1) % Capacity, std::memory_order_release);
        return true;
    }

    void clear() noexcept {
        T ignored{};
        while (pop(ignored)) {}
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace guitarrackcraft
