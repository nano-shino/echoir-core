// Parity with upstream FireRedVAD: the post-processor applied to recorded frame probabilities must
// reproduce upstream's speech ranges. A missing golden file skips its case and passes.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "doctest.h"
#include "json.hpp"
#include "stream/vad_postprocess.h"

using echoir::stream::VadPostprocessor;
using echoir::stream::VadPostprocessParams;

namespace {

std::filesystem::path golden_dir() {
    return std::filesystem::path(ECHOIR_SOURCE_DIR) / "tests" / "fixtures" / "firered";
}

// Little-endian float32 values, as tools/dev/firered_reference.py writes them.
std::vector<float> read_f32le(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) {
        throw std::runtime_error("not a float32 file: " + path.string());
    }
    std::vector<float> values(bytes.size() / 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint32_t bits = static_cast<std::uint32_t>(bytes[4 * i]) |
                                   static_cast<std::uint32_t>(bytes[4 * i + 1]) << 8 |
                                   static_cast<std::uint32_t>(bytes[4 * i + 2]) << 16 |
                                   static_cast<std::uint32_t>(bytes[4 * i + 3]) << 24;
        values[i] = std::bit_cast<float>(bits);
    }
    return values;
}

VadPostprocessParams params_from(const nlohmann::json& p) {
    VadPostprocessParams params;
    params.smooth_window_size = p.at("smooth_window_size").get<int>();
    params.prob_threshold = p.at("prob_threshold").get<double>();
    params.min_speech_frame = p.at("min_speech_frame").get<int>();
    params.max_speech_frame = p.at("max_speech_frame").get<int>();
    params.min_silence_frame = p.at("min_silence_frame").get<int>();
    params.merge_silence_frame = p.at("merge_silence_frame").get<int>();
    params.extend_speech_frame = p.at("extend_speech_frame").get<int>();
    return params;
}

}  // namespace

TEST_CASE("post-processor reproduces the upstream ranges for the golden clips") {
    for (const char* name : {"jfk.vad", "jfk.stream", "tone_noise.vad"}) {
        const auto probs_path = golden_dir() / (std::string(name) + ".probs.f32");
        const auto ranges_path = golden_dir() / (std::string(name) + ".ranges.json");
        if (!std::filesystem::exists(probs_path) || !std::filesystem::exists(ranges_path)) {
            // doctest's MESSAGE takes one operand, so the text is built before the call.
            MESSAGE(("SKIP: missing golden " + std::string(name)));
            std::fprintf(stderr, "SKIP: missing golden %s\n", name);
            continue;
        }
        std::ifstream in(ranges_path);
        const auto golden = nlohmann::json::parse(in);
        const std::vector<float> probs = read_f32le(probs_path);
        CHECK(probs.size() == golden.at("frames").get<std::size_t>());

        const VadPostprocessor pp(params_from(golden.at("params")));
        const auto got = pp.ranges(probs, golden.at("duration_s").get<double>());
        const auto& expected = golden.at("ranges");
        REQUIRE(got.size() == expected.size());
        for (std::size_t k = 0; k < got.size(); ++k) {
            const auto start_ms =
                static_cast<std::int64_t>(std::llround(expected[k][0].get<double>() * 1000.0));
            const auto end_ms =
                static_cast<std::int64_t>(std::llround(expected[k][1].get<double>() * 1000.0));
            CHECK(std::llabs(got[k].start_ms - start_ms) <= 1);
            CHECK(std::llabs(got[k].end_ms - end_ms) <= 1);
        }
    }
}
