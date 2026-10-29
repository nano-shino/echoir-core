#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fake/fake.h"
#include "text/env.h"
#include "vad/range.h"

namespace echoir::fake {
namespace {

constexpr std::int64_t kDefaultSegmentMs = 1000;
constexpr std::int64_t kMinSegmentMs = 100;
constexpr std::int64_t kMaxSegmentMs = 60000;

// The segment length from ECHOIR_FAKE_SEGMENT_MS. A value that is missing or not a number gives
// the default.
std::int64_t segment_ms_from_env() {
    const std::optional<std::string> text = text::get_env("ECHOIR_FAKE_SEGMENT_MS");
    if (!text) {
        return kDefaultSegmentMs;
    }
    try {
        return std::clamp<std::int64_t>(std::stoll(*text), kMinSegmentMs, kMaxSegmentMs);
    } catch (const std::exception&) {
        return kDefaultSegmentMs;
    }
}

class FakeVad final : public vad::IVad {
public:
    explicit FakeVad(std::int64_t segment_ms) : segment_ms_(segment_ms) {}

    // The whole input is speech. Ranges are segment_ms long, and the last one ends with the input.
    std::vector<vad::Range> detect(std::span<const float> pcm16k_mono) override {
        std::vector<vad::Range> out;
        if (pcm16k_mono.empty()) {
            return out;
        }
        // Rounded up, so the last range reaches the end of a partial millisecond.
        const auto total_ms = (static_cast<std::int64_t>(pcm16k_mono.size()) + 15) / 16;
        for (std::int64_t start = 0; start < total_ms; start += segment_ms_) {
            out.push_back(vad::Range{start, std::min(start + segment_ms_, total_ms)});
        }
        return out;
    }

    std::string name() const override { return "fake"; }

private:
    std::int64_t segment_ms_;
};

}  // namespace

std::unique_ptr<vad::IVad> make_vad() {
    return std::make_unique<FakeVad>(segment_ms_from_env());
}

}  // namespace echoir::fake
