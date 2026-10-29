#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "doctest.h"
#include "stream/signal.h"

using echoir::stream::find_cut;
using echoir::stream::find_quietest_point;
using echoir::stream::kCutSearch;
using echoir::stream::kCutSearchStream;
using echoir::stream::kQuietBlock;
using echoir::stream::rms;

namespace {

// Loud background: squares are 0.25 and 0.0625, so every 1600-sample block has energy well above
// the quiet and silent test gaps.
std::vector<float> background(std::size_t n) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = (i % 3 == 0) ? 0.5f : -0.25f;
    }
    return x;
}

void fill(std::vector<float>& x, std::size_t begin, std::size_t len, float value) {
    for (std::size_t i = begin; i < begin + len; ++i) {
        x[i] = value;
    }
}

}  // namespace

TEST_CASE("quietest point is the centre of a silent gap, exactly") {
    CHECK(kQuietBlock == 1600);
    for (std::size_t g : {std::size_t{1000}, std::size_t{7321}, std::size_t{20000}}) {
        std::vector<float> x = background(32000);
        fill(x, g, 3200, 0.0f);
        // Only blocks that start in [g, g + 1600] are silent. The earliest one is g.
        CHECK(find_quietest_point(x, 0, x.size()) == g + 800);
    }
    // A gap of exactly one block has a single silent start.
    std::vector<float> x = background(10000);
    fill(x, 4000, 1600, 0.0f);
    CHECK(find_quietest_point(x, 0, x.size()) == 4800);
}

TEST_CASE("quietest point finds a silence buried in quiet hiss") {
    std::vector<float> x = background(20000);
    fill(x, 5000, 4000, 1.0f / 64.0f);  // quiet, but not silent
    fill(x, 7000, 1600, 0.0f);          // silence inside the hiss
    CHECK(find_quietest_point(x, 0, x.size()) == 7800);
}

TEST_CASE("quietest point only considers blocks inside [lo, hi)") {
    std::vector<float> x = background(30000);
    fill(x, 2000, 3200, 0.0f);
    fill(x, 12000, 3200, 0.0f);
    // The gap at 2000 is outside [8000, 20000), so the gap at 12000 wins.
    CHECK(find_quietest_point(x, 8000, 20000) == 12800);
    // Both gaps are silent. The earlier one wins the tie.
    CHECK(find_quietest_point(x, 0, x.size()) == 2800);
}

TEST_CASE("quietest point breaks ties toward the earliest block") {
    std::vector<float> x = background(20000);
    fill(x, 4000, 3200, 0.0f);
    fill(x, 9000, 3200, 0.0f);
    CHECK(find_quietest_point(x, 0, x.size()) == 4800);
}

TEST_CASE("a block must fit entirely before hi") {
    std::vector<float> x = background(20000);
    fill(x, 17000, 3000, 0.0f);
    // With hi = 18000 the last block starts at 16400, so no block is fully silent. Any result
    // must come from a block that fits, which means block start <= 16400 and centre <= 17200.
    const std::size_t c18 = find_quietest_point(x, 0, 18000);
    CHECK(c18 <= 16400 + 800);
    // With hi = 19000 the silent block at 17000 fits.
    CHECK(find_quietest_point(x, 0, 19000) == 17800);
}

TEST_CASE("a range of exactly one block returns its centre") {
    std::vector<float> x = background(5000);
    CHECK(find_quietest_point(x, 100, 100 + kQuietBlock) == 100 + 800);
}

TEST_CASE("quietest point rejects ranges that break its preconditions") {
    std::vector<float> x = background(5000);
    CHECK_THROWS_AS(find_quietest_point(x, 0, 1599), std::invalid_argument);  // shorter than block
    CHECK_THROWS_AS(find_quietest_point(x, 10, 5), std::invalid_argument);    // lo > hi
    CHECK_THROWS_AS(find_quietest_point(x, 0, 5001), std::invalid_argument);  // hi past the end
}

TEST_CASE("the cut search covers only the window the caller passes before the cap") {
    CHECK(kCutSearch == 8 * 16000);
    CHECK(kCutSearchStream == 3 * 16000);
    // Silent gaps at 2000 and 60000. The cap is 100000, so the 3 s window starts at 52000 and
    // holds only the second gap, while the 8 s window starts at 0 and holds both.
    std::vector<float> x = background(200000);
    fill(x, 2000, 3200, 0.0f);
    fill(x, 60000, 3200, 0.0f);
    CHECK(find_cut(x, 0, 100000, 150000, kCutSearchStream) == 60800);
    CHECK(find_cut(x, 0, 100000, 150000, kCutSearch) == 2800);  // both silent, so the earlier wins
}

TEST_CASE("sliding search agrees with a brute-force reference") {
    // Values are multiples of 1/8, so squares and sums are exact in double. Energies therefore
    // tie often, and the tie-break is exercised for real.
    for (std::uint32_t seed : {1u, 7u, 12345u}) {
        std::uint32_t state = seed;
        std::vector<float> x(20000);
        for (float& v : x) {
            state = state * 1103515245u + 12345u;
            v = static_cast<float>(static_cast<int>((state >> 16) % 5) - 2) * 0.125f;
        }
        for (auto [lo, hi] : {std::pair<std::size_t, std::size_t>{0, 20000},
                              std::pair<std::size_t, std::size_t>{123, 9876},
                              std::pair<std::size_t, std::size_t>{5000, 6600}}) {
            double best = -1.0;
            std::size_t best_start = lo;
            for (std::size_t s = lo; s + kQuietBlock <= hi; ++s) {
                double energy = 0.0;
                for (std::size_t k = s; k < s + kQuietBlock; ++k) {
                    const double v = x[k];
                    energy += v * v;
                }
                if (best < 0.0 || energy < best) {
                    best = energy;
                    best_start = s;
                }
            }
            CHECK(find_quietest_point(x, lo, hi) == best_start + 800);
        }
    }
}

TEST_CASE("rms of silence, constants, and small known signals") {
    std::vector<float> empty;
    CHECK(rms(empty) == 0.0);

    std::vector<float> silence(100, 0.0f);
    CHECK(rms(silence) == 0.0);

    std::vector<float> constant(64, 0.5f);
    CHECK(rms(constant) == doctest::Approx(0.5));

    std::vector<float> alternating{1.0f, -1.0f, 1.0f, -1.0f};
    CHECK(rms(alternating) == doctest::Approx(1.0));

    std::vector<float> half_loud{0.5f, 0.5f, 0.0f, 0.0f};
    CHECK(rms(half_loud) == doctest::Approx(std::sqrt(0.125)));
}
