// Long-session regression on real models. long_mix is about 10 minutes of speech, silence and
// steady tones. Over that run no FINAL may come from a repetitive decode, silence and tone must
// yield almost no words, resident memory must stay flat, and the run must keep up with real time.
// silence_30s is run through the session and then decoded directly in 10 s windows. The session
// decodes only the ranges the VAD reports, and digital silence has none, so the direct decode is
// what reaches the backend's own silence handling. Each case skips when its model or fixture is
// missing, and ECHOIR_REQUIRE_MODELS=1 turns a skip into a failure.
//
// Run with: ctest -R long_session
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "stream/session.h"
#include "stream_support.h"
#include "test_support.h"
#include "text/clean.h"
#include "text/repetition.h"
#include "text/score.h"
#include "text/utf.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace ea = echoir::audio;
namespace et = echoir::test;

namespace {

// A run of silence or tone shorter than this is not a block. long_mix has blocks of 10 s and 15 s
// or more, and the speech clips are never this quiet for this long.
constexpr std::size_t kBlockFrames = 50;  // 100 ms frames, so 5 s
// Each block may hold fewer words than this.
constexpr std::size_t kMaxWordsPerBlock = 3;
// The tone is a 440 Hz sine of amplitude 0.3, so its RMS is 0.212 and each 100 ms frame has about
// 88 zero crossings.
constexpr double kToneRmsMin = 0.18;
constexpr double kToneRmsMax = 0.24;
constexpr std::size_t kToneCrossingsMin = 82;
constexpr std::size_t kToneCrossingsMax = 94;
// Digital silence has an RMS of exactly zero, so anything quieter than this counts as quiet.
constexpr double kQuietRms = 1e-4;
// Silence is decoded directly in windows of this many samples (10 s at 16 kHz).
constexpr std::size_t kSilenceWindowSamples = 10 * 16000;
// Resident memory is sampled every 10 s of audio.
constexpr std::int64_t kSampleEveryMs = 10000;
// The memory level is taken after the first minute, when models and buffers are allocated.
constexpr std::int64_t kWarmupMs = 60000;
constexpr double kMaxMemoryGrowth = 0.10;
constexpr double kMaxRtf = 1.0;

// Passes every call to the real backend and keeps the raw text of each decode, in order.
class RecordingBackend final : public echoir::IAsrBackend {
public:
    explicit RecordingBackend(echoir::IAsrBackend& inner) : inner_(inner) {}

    echoir::DecodeResult decode(const echoir::DecodeRequest& request) override {
        echoir::DecodeResult result = inner_.decode(request);
        raw_.push_back(result.text);
        return result;
    }
    std::string name() const override { return inner_.name(); }

    const std::vector<std::string>& raw() const { return raw_; }

private:
    echoir::IAsrBackend& inner_;
    std::vector<std::string> raw_;
};

// Resident memory of this process in bytes, or nothing where it is not measured.
std::optional<std::uint64_t> resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.resident_size);
#else
    return std::nullopt;
#endif
}

struct MemorySample {
    std::int64_t audio_ms = 0;
    std::uint64_t resident = 0;
};

// Each FINAL must come from a decode that is not repetitive. A FINAL's text is the cleaned text of
// its decode, so the decode is found by that text. Every decode with the same cleaned text counts,
// so a repetitive decode cannot pass by sharing its text with a clean one.
void check_no_repetitive_finals(const std::vector<echoir::SessionEvent>& events,
                                const std::vector<std::string>& raw_decodes) {
    // The raw text is tested, because cleaning collapses the runs that give a loop away.
    std::map<std::u32string, bool> repetitive_by_clean;
    for (const std::string& raw : raw_decodes) {
        const std::u32string raw32 = echoir::text::utf8_to_utf32(raw);
        bool& flag = repetitive_by_clean[echoir::text::clean_segment(raw32)];
        flag = flag || echoir::text::is_repetitive(raw32);
    }
    for (const echoir::SessionEvent& ev : events) {
        if (ev.type != ECHOIR_EVENT_FINAL || ev.text.empty()) continue;
        INFO("FINAL " << ev.segment_id << " [" << ev.start_ms << ", " << ev.end_ms
                      << ") text: " << ev.text);
        const auto it = repetitive_by_clean.find(echoir::text::utf8_to_utf32(ev.text));
        // Every FINAL text comes from a decode of this run. A miss means the mapping broke, so the
        // FINAL is reported and the remaining events are still checked.
        CHECK(it != repetitive_by_clean.end());
        if (it == repetitive_by_clean.end()) continue;
        CHECK_FALSE(it->second);
    }
}

// Counts the words of a text as the score counts them. Punctuation and symbols are dropped first,
// so a bracketed tag such as "[ Music ]" counts as one word. Each CJK, kana or Hangul character
// counts as one unit.
std::size_t count_words(const std::string& text) {
    return echoir::text::error_rate("", text).ref_len;
}

// A run of silence or of tone, in ms from the start of the input.
struct Block {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    bool tone = false;
};

// Finds the silence and tone blocks from the audio alone, so the check does not depend on the
// VAD. Each 100 ms frame is quiet, tone or other, and a run of at least kBlockFrames frames of one
// kind is a block. The input must be 16 kHz mono.
std::vector<Block> find_blocks(const ea::PcmBuffer& wav) {
    constexpr std::size_t kFrame = 1600;  // 100 ms at 16 kHz
    const std::size_t frames = wav.samples.size() / kFrame;
    enum class Kind { other, quiet, tone };
    std::vector<Kind> kinds(frames, Kind::other);
    for (std::size_t f = 0; f < frames; ++f) {
        const auto begin = wav.samples.begin() + static_cast<std::ptrdiff_t>(f * kFrame);
        const auto end = begin + static_cast<std::ptrdiff_t>(kFrame);
        double sum_sq = 0.0;
        std::size_t crossings = 0;
        for (auto it = begin; it != end; ++it) {
            sum_sq += static_cast<double>(*it) * static_cast<double>(*it);
            if (it != begin && ((*it < 0.0f) != (*(it - 1) < 0.0f))) ++crossings;
        }
        const double rms = std::sqrt(sum_sq / static_cast<double>(kFrame));
        if (rms < kQuietRms) {
            kinds[f] = Kind::quiet;
        } else if (rms >= kToneRmsMin && rms <= kToneRmsMax && crossings >= kToneCrossingsMin &&
                   crossings <= kToneCrossingsMax) {
            kinds[f] = Kind::tone;
        }
    }

    std::vector<Block> blocks;
    std::size_t f = 0;
    while (f < frames) {
        std::size_t g = f;
        while (g < frames && kinds[g] == kinds[f]) ++g;
        if (kinds[f] != Kind::other && g - f >= kBlockFrames) {
            blocks.push_back(Block{static_cast<std::int64_t>(f) * 100,
                                   static_cast<std::int64_t>(g) * 100,
                                   kinds[f] == Kind::tone});
        }
        f = g;
    }
    return blocks;
}

// Silence and tone blocks produce no text, or under kMaxWordsPerBlock words per block. An event
// belongs to the block that holds its midpoint. Each kind has its own minimum count, so the blocks
// of one kind cannot satisfy the floor alone and leave the other kind unchecked.
void check_quiet_and_tone(const std::vector<echoir::SessionEvent>& events,
                          const ea::PcmBuffer& wav, std::size_t min_silence,
                          std::size_t min_tone) {
    REQUIRE(wav.channels == 1);
    REQUIRE(wav.rate == 16000);
    const std::vector<Block> blocks = find_blocks(wav);
    const auto tones = static_cast<std::size_t>(
        std::count_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.tone; }));
    // Without blocks of a kind, its word limit would pass without checking anything.
    REQUIRE(tones >= min_tone);
    REQUIRE(blocks.size() - tones >= min_silence);

    std::vector<std::size_t> words(blocks.size(), 0);
    for (const echoir::SessionEvent& ev : events) {
        if (ev.type != ECHOIR_EVENT_FINAL) continue;
        const std::int64_t mid = (ev.start_ms + ev.end_ms) / 2;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            if (mid >= blocks[b].start_ms && mid < blocks[b].end_ms) {
                words[b] += count_words(ev.text);
                break;
            }
        }
    }
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        INFO((blocks[b].tone ? "tone" : "silence") << " block [" << blocks[b].start_ms << ", "
                                                   << blocks[b].end_ms << ") ms");
        CHECK(words[b] < kMaxWordsPerBlock);
    }
}

// Decodes silence directly through the backend, in windows of kSilenceWindowSamples. The session
// never does this for digital silence, because it decodes only VAD ranges, so this check is what
// reaches the backend's silence handling. Each window must not be repetitive and must hold fewer
// than kMaxWordsPerBlock words once cleaned.
void check_silence_decodes(echoir::IAsrBackend& asr, const ea::PcmBuffer& wav) {
    const std::vector<std::string> no_history;
    for (std::size_t begin = 0; begin < wav.samples.size(); begin += kSilenceWindowSamples) {
        const std::size_t count = std::min(kSilenceWindowSamples, wav.samples.size() - begin);
        const echoir::DecodeRequest request{
            std::span<const float>(wav.samples).subspan(begin, count), no_history, "auto"};
        const std::string raw = asr.decode(request).text;
        INFO("silence window at " << begin / 16000 << " s, raw text: " << raw);
        const std::u32string raw32 = echoir::text::utf8_to_utf32(raw);
        CHECK_FALSE(echoir::text::is_repetitive(raw32));
        CHECK(count_words(echoir::text::utf32_to_utf8(echoir::text::clean_segment(raw32))) <
              kMaxWordsPerBlock);
    }
}

// Resident memory after the first minute must stay within kMaxMemoryGrowth of its level then.
void check_memory_flat(const std::vector<MemorySample>& samples) {
    std::optional<std::uint64_t> baseline;
    std::uint64_t peak = 0;
    for (const MemorySample& s : samples) {
        if (s.audio_ms < kWarmupMs) continue;
        if (!baseline) baseline = s.resident;
        peak = std::max(peak, s.resident);
    }
    REQUIRE(baseline.has_value());
    const double growth = static_cast<double>(peak) / static_cast<double>(*baseline) - 1.0;
    MESSAGE("resident memory: " << *baseline / (1024 * 1024) << " MiB after 1 min, peak "
                                << peak / (1024 * 1024) << " MiB (" << growth * 100.0
                                << "% growth)");
    CHECK(growth <= kMaxMemoryGrowth);
}

// Wall time over audio time for the whole run, model loading excluded.
void check_real_time(double wall_s, std::int64_t duration_ms) {
    const double rtf = wall_s / (static_cast<double>(duration_ms) / 1000.0);
    MESSAGE("RTF " << rtf << " over " << duration_ms / 1000 << " s of audio");
    CHECK(rtf < kMaxRtf);
}

// Runs one backend over long_mix with the memory and time checks.
void run_long_mix(const et::BackendCase& backend) {
    const auto fixture = et::fixture("long_mix");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "long_mix.wav fixture (tools/make-fixtures.sh)");
    auto engine = et::load_engine(backend, /*use_gpu=*/true);
    ECHOIR_REQUIRE_OR_SKIP(engine, "speech model and VAD for the long session test");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);
    const std::int64_t duration_ms = et::duration_ms_of(wav);

    RecordingBackend recorder(*engine->asr);
    std::vector<MemorySample> samples;
    std::int64_t next_sample_ms = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto events =
        et::stream_all(recorder, *engine->vad, wav, [&](std::int64_t audio_ms) {
            if (audio_ms < next_sample_ms) return;
            next_sample_ms = audio_ms + kSampleEveryMs;
            if (const auto resident = resident_bytes()) {
                samples.push_back(MemorySample{audio_ms, *resident});
            }
        });
    const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - started;

    et::check_invariants(events, duration_ms);
    check_no_repetitive_finals(events, recorder.raw());
    check_quiet_and_tone(events, wav, /*min_silence=*/1, /*min_tone=*/2);
    check_real_time(wall.count(), duration_ms);
    if (samples.empty()) {
        // Resident memory is read on macOS only. A required run fails here rather than pass
        // without the check. Any other run prints the gap on stderr, as the model skips do.
        if (et::models_required()) {
            FAIL("resident memory is not measured on this platform (ECHOIR_REQUIRE_MODELS is set)");
        }
        std::fprintf(stderr, "SKIP: resident memory is measured on macOS only\n");
    } else {
        check_memory_flat(samples);
    }
}

// Runs one backend over 30 s of digital silence, where any word is a hallucination. The audio goes
// through the session and then straight to the backend.
void run_silence(const et::BackendCase& backend) {
    const auto fixture = et::fixture("silence_30s");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "silence_30s.wav fixture (tools/make-fixtures.sh)");
    auto engine = et::load_engine(backend, /*use_gpu=*/true);
    ECHOIR_REQUIRE_OR_SKIP(engine, "speech model and VAD for the long session test");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);

    RecordingBackend recorder(*engine->asr);
    const auto events = et::stream_all(recorder, *engine->vad, wav);
    et::check_invariants(events, et::duration_ms_of(wav), /*expect_text=*/false);
    check_no_repetitive_finals(events, recorder.raw());
    check_quiet_and_tone(events, wav, /*min_silence=*/1, /*min_tone=*/0);
    check_silence_decodes(*engine->asr, wav);
}

}  // namespace

TEST_CASE("long session: whisper on long_mix") { run_long_mix(et::kWhisper); }

TEST_CASE("long session: qwen on long_mix") { run_long_mix(et::kQwen); }

TEST_CASE("long session: whisper on silence_30s") { run_silence(et::kWhisper); }

TEST_CASE("long session: qwen on silence_30s") { run_silence(et::kQwen); }
