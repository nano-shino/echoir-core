#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "stream/vad_postprocess.h"

using echoir::stream::VadPostprocessor;
using echoir::stream::VadPostprocessParams;
using echoir::vad::Range;

namespace {

using Spans = std::vector<std::pair<std::int64_t, std::int64_t>>;

void set_frames(std::vector<float>& probs, std::size_t begin, std::size_t len, float value) {
    for (std::size_t i = begin; i < begin + len; ++i) {
        probs[i] = value;
    }
}

// Decisions as text, one character per frame, so expectations read like the frames.
std::string text(const std::vector<std::uint8_t>& decisions) {
    std::string out;
    for (const std::uint8_t d : decisions) {
        out.push_back(d ? '1' : '0');
    }
    return out;
}

Spans spans(const std::vector<Range>& ranges) {
    Spans out;
    for (const Range& r : ranges) {
        out.emplace_back(r.start_ms, r.end_ms);
    }
    return out;
}

}  // namespace

TEST_CASE("a speech blip shorter than min_speech is dropped") {
    std::vector<float> probs(100, 0.0f);
    set_frames(probs, 40, 3, 0.9f);
    const VadPostprocessor pp({.smooth_window_size = 1});
    CHECK(text(pp.process(probs)) == std::string(100, '0'));
    CHECK(pp.ranges(probs, 1.0).empty());
}

TEST_CASE("a speech blip is kept when min_speech allows it") {
    std::vector<float> probs(100, 0.0f);
    set_frames(probs, 40, 3, 0.9f);
    const VadPostprocessor pp(
        {.smooth_window_size = 1, .min_speech_frame = 2, .min_silence_frame = 1});
    // With a window of 1 the rising edge moves back one frame, as in upstream.
    CHECK(spans(pp.ranges(probs, 1.0)) == Spans{{390, 440}});
}

TEST_CASE("a silence dip shorter than min_silence is bridged") {
    std::vector<float> probs(200, 0.0f);
    set_frames(probs, 0, 100, 0.9f);
    set_frames(probs, 40, 10, 0.0f);
    const VadPostprocessor pp({.smooth_window_size = 1});
    CHECK(spans(pp.ranges(probs, 2.0)) == Spans{{0, 1200}});
}

TEST_CASE("a silence dip as long as min_silence splits the speech") {
    std::vector<float> probs(200, 0.0f);
    set_frames(probs, 0, 40, 0.9f);
    set_frames(probs, 65, 35, 0.9f);
    const VadPostprocessor pp({.smooth_window_size = 1});
    CHECK(spans(pp.ranges(probs, 2.0)) == Spans{{0, 600}, {640, 1200}});
}

TEST_CASE("speech longer than max_speech is split once at its quietest frame") {
    std::vector<float> probs(2500, 0.9f);
    probs[1500] = 0.5f;
    const VadPostprocessor pp({.smooth_window_size = 1});
    const auto decisions = pp.process(probs);
    CHECK(decisions[1499] == 1);
    CHECK(decisions[1500] == 0);
    CHECK(decisions[1501] == 1);
    CHECK(spans(pp.ranges(probs, 25.0)) == Spans{{0, 15000}, {15010, 25000}});
}

TEST_CASE("a split that leaves speech over max_speech is split again") {
    std::vector<float> probs(6000, 0.9f);
    probs[1500] = 0.5f;
    probs[3000] = 0.5f;
    probs[4500] = 0.5f;
    const VadPostprocessor pp({.smooth_window_size = 1});
    CHECK(spans(pp.ranges(probs, 60.0)) ==
          Spans{{0, 15000}, {15010, 30000}, {30010, 45000}, {45010, 60000}});
}

TEST_CASE("an open segment at the end ends at the clip end") {
    std::vector<float> probs(100, 0.0f);
    set_frames(probs, 60, 40, 0.9f);
    const VadPostprocessor pp({.smooth_window_size = 1});
    // The frames end at 1.025 s. The clip is 1.2 s, so the end keeps the frame end.
    CHECK(spans(pp.ranges(probs, 1.2)) == Spans{{590, 1025}});
    // A 1.0 s clip caps the end at the clip end.
    CHECK(spans(pp.ranges(probs, 1.0)) == Spans{{590, 1000}});
}

TEST_CASE("a clip shorter than a range's start drops the range") {
    std::vector<float> probs(100, 0.0f);
    set_frames(probs, 60, 40, 0.9f);
    const VadPostprocessor pp({.smooth_window_size = 1});
    CHECK(pp.ranges(probs, 0.5).empty());
    // A duration that isn't a whole ms never lets an end pass it.
    for (const auto& r : pp.ranges(probs, 1.0004)) {
        CHECK(r.end_ms <= 1000);
    }
}

TEST_CASE("a probability exactly at the threshold counts as speech") {
    const std::vector<float> at(60, 0.5f);
    const std::vector<float> below(60, 0.4999f);
    const VadPostprocessor pp({.smooth_window_size = 1, .prob_threshold = 0.5});
    CHECK(text(pp.process(at)) == std::string(60, '1'));
    CHECK(text(pp.process(below)) == std::string(60, '0'));
}

TEST_CASE("smoothing over 5 frames keeps a tie at the threshold as speech") {
    const VadPostprocessor half({.smooth_window_size = 5, .prob_threshold = 0.5});
    CHECK(spans(half.ranges(std::vector<float>(60, 0.5f), 0.6)) == Spans{{0, 600}});
    const VadPostprocessor default_threshold({.smooth_window_size = 5});
    CHECK(spans(default_threshold.ranges(std::vector<float>(60, 0.4f), 0.6)) == Spans{{0, 600}});
}

TEST_CASE("a rising edge moves back over the smoothing window") {
    std::vector<float> probs(120, 0.0f);
    set_frames(probs, 30, 90, 0.9f);
    const VadPostprocessor pp(
        {.smooth_window_size = 5, .min_speech_frame = 1, .min_silence_frame = 1});
    // Smoothing delays the edge to frame 32. The window fix moves it back five frames to 27.
    CHECK(spans(pp.ranges(probs, 1.2)) == Spans{{270, 1200}});
}

TEST_CASE("extension grows speech by extend_speech_frame on each side") {
    std::vector<float> probs(50, 0.0f);
    set_frames(probs, 20, 10, 0.9f);
    const VadPostprocessor plain(
        {.smooth_window_size = 1, .min_speech_frame = 1, .min_silence_frame = 1});
    const VadPostprocessor extended({.smooth_window_size = 1,
                                     .min_speech_frame = 1,
                                     .min_silence_frame = 1,
                                     .extend_speech_frame = 2});
    CHECK(spans(plain.ranges(probs, 0.5)) == Spans{{190, 310}});
    CHECK(spans(extended.ranges(probs, 0.5)) == Spans{{170, 330}});
}

TEST_CASE("merge fills a silence gap shorter than merge_silence_frame") {
    std::vector<float> probs(50, 0.0f);
    set_frames(probs, 0, 20, 0.9f);
    set_frames(probs, 23, 17, 0.9f);
    const VadPostprocessor plain(
        {.smooth_window_size = 1, .min_speech_frame = 1, .min_silence_frame = 1});
    const VadPostprocessor merged({.smooth_window_size = 1,
                                   .min_speech_frame = 1,
                                   .min_silence_frame = 1,
                                   .merge_silence_frame = 5});
    CHECK(spans(plain.ranges(probs, 0.5)) == Spans{{0, 210}, {220, 410}});
    CHECK(spans(merged.ranges(probs, 0.5)) == Spans{{0, 410}});
}

TEST_CASE("a window longer than the input keeps upstream's length") {
    std::vector<float> probs(40, 0.0f);
    set_frames(probs, 0, 15, 0.9f);
    const VadPostprocessor pp({.smooth_window_size = 1,
                               .min_speech_frame = 1,
                               .min_silence_frame = 1,
                               .extend_speech_frame = 50});
    const auto decisions = pp.process(probs);
    // Window 101 is longer than the 40 frames, so upstream returns 101 decisions.
    CHECK(decisions.size() == 101);
    // The extended speech would end at 0.97 s, past the 0.4 s clip. The end is capped there.
    CHECK(spans(pp.ranges(probs, 0.4)) == Spans{{0, 400}});
}

TEST_CASE("empty input gives no decisions and no ranges") {
    const VadPostprocessor pp({});
    CHECK(pp.process({}).empty());
    CHECK(pp.ranges({}, 1.0).empty());
}

TEST_CASE("invalid parameters are rejected") {
    const VadPostprocessParams no_window = {.max_speech_frame = 0};
    CHECK_THROWS_AS(VadPostprocessor{no_window}, std::invalid_argument);
    const VadPostprocessor pp({});
    CHECK_THROWS_AS(pp.ranges(std::vector<float>(10, 0.9f), -1.0), std::invalid_argument);
}
