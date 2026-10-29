// Lock-free single-producer, single-consumer ring for handing audio from a capture thread to a
// session.
// One thread calls push(), one other thread calls pop(). Neither side blocks or allocates.
#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace echoir::audio {

#ifdef _MSC_VER
// The alignas(64) indices pad the class on purpose, so each sits on its own cache line.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename T>
class SpscRing {
public:
    // Capacity is rounded up to a power of two, so indexing is a mask, not a modulo.
    // Throws std::length_error when that power would not fit in std::size_t.
    explicit SpscRing(std::size_t min_capacity) {
        constexpr std::size_t kMaxCapacity = std::size_t{1}
                                             << (std::numeric_limits<std::size_t>::digits - 1);
        if (min_capacity > kMaxCapacity) throw std::length_error("SpscRing: capacity too large");
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    std::size_t capacity() const noexcept { return mask_ + 1; }

    // Producer thread only. Writes as many items as fit and returns that count.
    std::size_t push(std::span<const T> items) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire pairs with the consumer's release in pop(): the slots it freed are readable.
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t room = capacity() - (tail - head);
        const std::size_t n = items.size() < room ? items.size() : room;

        const std::size_t first = copy_in(tail, items.first(n));
        copy_in(tail + first, items.subspan(first, n - first));

        // Release publishes the slot writes before the new tail becomes visible.
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer thread only. Reads up to out.size() items and returns the count read.
    std::size_t pop(std::span<T> out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire pairs with the producer's release in push(): the slot data is visible.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t avail = tail - head;
        const std::size_t n = out.size() < avail ? out.size() : avail;

        const std::size_t first = copy_out(head, out.first(n));
        copy_out(head + first, out.subspan(first, n - first));

        // Release hands the slots back to the producer only after the reads above.
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Either thread. Exact when read from the thread that is not moving the index.
    std::size_t size() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    // Copies `src` to slots starting at logical index `at`, up to the end of the buffer.
    // Returns how many items it copied, so the caller can continue after the wrap.
    std::size_t copy_in(std::size_t at, std::span<const T> src) noexcept {
        const std::size_t idx = at & mask_;
        const std::size_t run = capacity() - idx;
        const std::size_t n = src.size() < run ? src.size() : run;
        for (std::size_t i = 0; i < n; ++i) buf_[idx + i] = src[i];
        return n;
    }

    std::size_t copy_out(std::size_t at, std::span<T> dst) noexcept {
        const std::size_t idx = at & mask_;
        const std::size_t run = capacity() - idx;
        const std::size_t n = dst.size() < run ? dst.size() : run;
        for (std::size_t i = 0; i < n; ++i) dst[i] = buf_[idx + i];
        return n;
    }

    std::vector<T> buf_;
    std::size_t mask_ = 0;
    // Each index sits on its own cache line so the two threads do not false-share.
    alignas(64) std::atomic<std::size_t> head_{0};  // written by the consumer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by the producer
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace echoir::audio
