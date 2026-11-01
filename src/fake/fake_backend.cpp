#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "fake/fake.h"
#include "text/env.h"

namespace echoir::fake {
namespace {

// One word of the fake text per block of this many samples (100 ms at 16 kHz).
constexpr std::size_t kBlockSamples = 1600;
constexpr double kMaxDecodeRtf = 10.0;

// The decode delay per second of audio from ECHOIR_FAKE_DECODE_RTF. Missing, not a whole finite
// number (such as "2x", "nan" or "inf"): none.
double decode_rtf_from_env() {
    const std::optional<std::string> text = text::get_env("ECHOIR_FAKE_DECODE_RTF");
    if (!text) {
        return 0.0;
    }
    try {
        std::size_t used = 0;
        const double rtf = std::stod(*text, &used);
        if (used != text->size() || !std::isfinite(rtf)) {
            return 0.0;
        }
        return std::clamp(rtf, 0.0, kMaxDecodeRtf);
    } catch (const std::exception&) {
        return 0.0;
    }
}

class FakeBackend final : public IAsrBackend {
public:
    FakeBackend() : decode_rtf_(decode_rtf_from_env()) {}

    DecodeResult decode(const DecodeRequest& request) override {
        if (!request.prefix.empty()) {
            throw std::invalid_argument("the fake backend takes no prefix");
        }
        if (request.pcm.empty()) {
            return DecodeResult{};
        }
        if (decode_rtf_ > 0.0) {
            // 16 samples per ms.
            const double audio_ms = static_cast<double>(request.pcm.size()) / 16.0;
            std::this_thread::sleep_for(
                std::chrono::duration<double, std::milli>(audio_ms * decode_rtf_));
        }
        const std::size_t blocks = std::max<std::size_t>(1, request.pcm.size() / kBlockSamples);
        const std::string text = "fake " + std::to_string(blocks);
        return DecodeResult{text, text};
    }

    std::string name() const override { return "fake"; }

private:
    double decode_rtf_;
};

}  // namespace

std::unique_ptr<IAsrBackend> make_backend() { return std::make_unique<FakeBackend>(); }

}  // namespace echoir::fake
