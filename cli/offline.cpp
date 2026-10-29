#include "offline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio/resampler.h"
#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "input.h"
#include "models.h"
#include "stream/signal.h"
#include "text/clean.h"
#include "text/utf.h"
#include "vad/vad.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// The longest piece decoded in one call, 20 s. VAD ranges can run much longer, because whisper.cpp
// merges speech across short gaps with no cap, and Qwen rejects windows over 30 s.
constexpr std::size_t kMaxPieceSamples = 320000;

}  // namespace

OfflineResult transcribe_offline(const TranscribeOptions& o,
                                 const std::vector<fs::path>& model_dirs,
                                 const SegmentSink& on_segment) {
    using Clock = std::chrono::steady_clock;

    // The input is read first, so a bad input is reported before any model loads.
    const audio::PcmBuffer wav = read_input(o.input, o.raw_pcm);
    // The load time covers the VAD and the model.
    const auto started = Clock::now();
    LoadedModels models = load_models(o, model_dirs, vad::VadUse::offline);
    const auto loaded = Clock::now();
    // A bad language or context fails here, so it is reported even when the input has no speech.
    check_request(*models.asr, o);

    const std::vector<float> pcm = audio::to_mono_16k(wav.samples, wav.rate, wav.channels);
    const auto decoding = Clock::now();
    const std::vector<vad::Range> ranges = models.vad->detect(pcm);

    OfflineResult result;
    result.backend = models.backend;

    // Each piece is decoded alone, with no history, so one piece cannot influence the next. The
    // user context goes with every piece.
    const std::vector<std::string> no_history;
    const auto decode_piece = [&](std::size_t from, std::size_t to) {
        const DecodeRequest request{std::span<const float>(pcm).subspan(from, to - from),
                                    no_history, o.run.language, o.context};
        std::string line = text::utf32_to_utf8(
            text::clean_segment(text::utf8_to_utf32(models.asr->decode(request).text)));
        if (line.empty()) {
            return;
        }
        const auto ms_of = [](std::size_t sample) {
            return static_cast<std::int64_t>(sample) / stream::kSamplesPerMs;
        };
        Segment segment{ms_of(from), ms_of(to), std::move(line)};
        if (on_segment) {
            on_segment(segment);
        }
        result.segments.push_back(std::move(segment));
    };

    for (const vad::Range& range : ranges) {
        const std::size_t first =
            std::min(static_cast<std::size_t>(range.start_ms * stream::kSamplesPerMs), pcm.size());
        const std::size_t last =
            std::min(static_cast<std::size_t>(range.end_ms * stream::kSamplesPerMs), pcm.size());
        if (last <= first) {
            continue;
        }
        // A long range is cut at the quietest point in the 8 s before each 20 s cap, which is where
        // a pause is most likely. The search ends a quiet block before the range end, so no cut
        // leaves a sliver behind.
        std::size_t piece = first;
        while (last - piece > kMaxPieceSamples) {
            const std::size_t cap = piece + kMaxPieceSamples;
            const std::size_t cut = stream::find_cut(pcm, piece, cap, last, stream::kCutSearch);
            decode_piece(piece, cut);
            piece = cut;
        }
        decode_piece(piece, last);
    }

    const auto finished = Clock::now();
    const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    result.stats.audio_s = static_cast<double>(pcm.size()) / (stream::kSamplesPerMs * 1000.0);
    result.stats.load_s = seconds(loaded - started);
    result.stats.wall_s = seconds(finished - decoding);
    return result;
}

}  // namespace echoir::cli
