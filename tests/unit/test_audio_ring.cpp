#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "audio/spsc_ring.h"
#include "doctest.h"

namespace ea = echoir::audio;

TEST_CASE("SpscRing: capacity rounds up to a power of two") {
    ea::SpscRing<float> ring(1000);
    CHECK(ring.capacity() == 1024);
    ea::SpscRing<float> exact(64);
    CHECK(exact.capacity() == 64);
}

TEST_CASE("SpscRing: push stops when full, pop frees room") {
    ea::SpscRing<int> ring(8);
    int in[12];
    for (int i = 0; i < 12; ++i) in[i] = i;

    CHECK(ring.push(std::span<const int>(in, 12)) == 8);  // only 8 fit
    CHECK(ring.push(std::span<const int>(in, 1)) == 0);   // full
    CHECK(ring.size() == 8);

    int out[5] = {};
    CHECK(ring.pop(out) == 5);
    for (int i = 0; i < 5; ++i) CHECK(out[i] == i);
    CHECK(ring.size() == 3);
}

TEST_CASE("SpscRing: a push that wraps the buffer end keeps order") {
    ea::SpscRing<int> ring(8);
    int seq[16];
    for (int i = 0; i < 16; ++i) seq[i] = i;

    // Move the indices to 6 so the next 4-item push splits across slot 7 and slot 0.
    REQUIRE(ring.push(std::span<const int>(seq, 6)) == 6);
    int drop[6];
    REQUIRE(ring.pop(drop) == 6);
    REQUIRE(ring.push(std::span<const int>(seq + 6, 4)) == 4);

    int got[4] = {};
    REQUIRE(ring.pop(got) == 4);
    for (int i = 0; i < 4; ++i) CHECK(got[i] == 6 + i);
}

TEST_CASE("SpscRing: two threads move 2M items in order") {
    ea::SpscRing<float> ring(1024);
    // Every index is an integer below 2^24, so float holds it exactly.
    constexpr std::size_t kTotal = 2'000'000;

    std::thread producer([&ring] {
        std::vector<float> chunk;
        std::size_t next = 0;
        std::size_t step = 1;
        while (next < kTotal) {
            const std::size_t n = std::min(step, kTotal - next);
            chunk.resize(n);
            for (std::size_t i = 0; i < n; ++i) chunk[i] = static_cast<float>(next + i);
            std::size_t sent = 0;
            while (sent < n) {
                const std::size_t k = ring.push(std::span<const float>(chunk).subspan(sent));
                sent += k;
                if (k == 0) std::this_thread::yield();
            }
            next += n;
            step = step % 97 + 1;  // vary the chunk size so the wrap point moves
        }
    });

    // The consumer runs on the test thread. Checks stay here, not in the producer thread.
    std::vector<float> got(64);
    std::size_t received = 0;
    bool in_order = true;
    while (received < kTotal) {
        const std::size_t k = ring.pop(got);
        for (std::size_t i = 0; i < k; ++i) {
            if (got[i] != static_cast<float>(received + i)) in_order = false;
        }
        received += k;
        if (k == 0) std::this_thread::yield();
    }
    producer.join();

    CHECK(in_order);
    CHECK(received == kTotal);
    CHECK(ring.size() == 0);
}

TEST_CASE("SpscRing: a capacity above the largest power of two throws instead of hanging") {
    // Rounding SIZE_MAX up would overflow to zero. The constructor must reject it.
    const std::size_t too_big = std::numeric_limits<std::size_t>::max();
    CHECK_THROWS_AS(ea::SpscRing<float>{too_big}, std::length_error);
}
