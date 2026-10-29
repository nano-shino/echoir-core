#include <array>
#include <cstddef>
#include <span>

#include "doctest.h"

// Checks the toolchain and the test runner.
// The core is 16 kHz mono float32 throughout, so f32 has to be 32 bits.
static_assert(sizeof(float) == 4, "PCM is float32");

TEST_CASE("C++20 std::span works over float32 PCM") {
    std::array<float, 4> samples{0.5f, 1.0f, -1.0f, 0.0f};
    std::span<const float> view(samples);

    CHECK(view.size() == std::size_t{4});
    CHECK(view[1] == 1.0f);
    CHECK(view[2] == -1.0f);
}
