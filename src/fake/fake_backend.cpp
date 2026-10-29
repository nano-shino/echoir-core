#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "fake/fake.h"

namespace echoir::fake {
namespace {

// One word of the fake text per block of this many samples (100 ms at 16 kHz).
constexpr std::size_t kBlockSamples = 1600;

class FakeBackend final : public IAsrBackend {
public:
    DecodeResult decode(const DecodeRequest& request) override {
        if (!request.prefix.empty()) {
            throw std::invalid_argument("the fake backend takes no prefix");
        }
        if (request.pcm.empty()) {
            return DecodeResult{};
        }
        const std::size_t blocks = std::max<std::size_t>(1, request.pcm.size() / kBlockSamples);
        const std::string text = "fake " + std::to_string(blocks);
        return DecodeResult{text, text};
    }

    std::string name() const override { return "fake"; }
};

}  // namespace

std::unique_ptr<IAsrBackend> make_backend() { return std::make_unique<FakeBackend>(); }

}  // namespace echoir::fake
