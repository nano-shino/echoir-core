// The session tape: memory and Opus pages, reads by range across both, seams between sessions
// and inside one, several tracks, the envelope, the joined files of checkpoint() and close(), and
// the C ABI. Sessions run on the
// scripted backend and VAD from tests/support/live_fakes.h, pulled, so a push stores at once.
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../support/live_fakes.h"
#include "doctest.h"
#include "echoir/echoir.h"
#include "errors.h"
#include "json.hpp"
#include "stream/envelope.h"
#include "stream/session.h"
#include "stream/tape.h"

namespace {

namespace ts = echoir::test_support;
using echoir::Session;
using echoir::SessionConfig;
using echoir::SessionEvent;
using echoir::stream::Tape;

constexpr std::int64_t kAll = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kSpm = 16;  // samples per ms
constexpr std::int64_t kMinute = 60000;

// A folder under the system temp folder, removed with its files at the end of the test.
struct TempFolder {
    TempFolder() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("echoir-tape-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    }
    ~TempFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

// A voiced, speech-like signal: harmonics of a gliding pitch under a syllable-rate envelope, so
// Opus codes it the way it codes voice. seed shifts the pitch and the envelope, so tracks differ.
// offset is the first sample, so a long signal can be made in pieces.
std::vector<float> voice(std::int64_t samples, int seed, std::int64_t offset = 0) {
    constexpr double kTwoPi = 6.283185307179586;
    std::vector<float> out(static_cast<std::size_t>(samples));
    for (std::int64_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(offset + i) / 16000.0;
        // The phase of a gliding pitch, integrated in closed form.
        const double phase =
            kTwoPi * (130.0 + 20.0 * seed) * t - 50.0 / 0.4 * std::cos(kTwoPi * 0.4 * t + seed);
        double v = 0.0;
        for (int k = 1; k <= 10; ++k) {
            v += std::sin(k * phase) / k;
        }
        const double env = 0.55 + 0.45 * std::sin(kTwoPi * 3.1 * t + 0.7 * seed);
        out[static_cast<std::size_t>(i)] = static_cast<float>(0.2 * env * v);
    }
    return out;
}

std::vector<std::int16_t> to_s16(const std::vector<float>& pcm) {
    std::vector<std::int16_t> out(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        out[i] = echoir::stream::to_s16(pcm[i]);
    }
    return out;
}

std::vector<std::int16_t> slice(const std::vector<std::int16_t>& v, std::int64_t from_ms,
                                std::int64_t to_ms) {
    return {v.begin() + from_ms * kSpm, v.begin() + to_ms * kSpm};
}

std::vector<std::int16_t> read(const Tape& tape, const std::string& track, std::int64_t start_ms,
                               std::int64_t end_ms) {
    std::vector<std::int16_t> out(tape.read_audio(track, start_ms, end_ms, nullptr, 0));
    REQUIRE(tape.read_audio(track, start_ms, end_ms, out.data(), out.size()) == out.size());
    return out;
}

std::vector<std::int16_t> read(const Session& session, std::int64_t start_ms, std::int64_t end_ms) {
    std::vector<std::int16_t> out(session.read_audio(start_ms, end_ms, nullptr, 0));
    REQUIRE(session.read_audio(start_ms, end_ms, out.data(), out.size()) == out.size());
    return out;
}

// Normalized correlation of a and b shifted by lag samples (b[i + lag] against a[i]).
double correlation(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b,
                   int lag) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto j = static_cast<std::int64_t>(i) + lag;
        if (j < 0 || j >= static_cast<std::int64_t>(b.size())) continue;
        const double x = a[i];
        const double y = b[static_cast<std::size_t>(j)];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
}

// Decoded Opus is close to its input and in time with it: equal length, a high correlation, and
// no better match at a lag beyond max_lag (a shift would mean the encoder delay is handled
// wrongly). The tape decodes at 16 kHz and is exact in time (max_lag 0). A player decodes at
// 48 kHz, whose resampler is about one 16 kHz sample apart, so the file check allows 1.
void check_close(const std::vector<std::int16_t>& decoded,
                 const std::vector<std::int16_t>& original, int max_lag = 0) {
    REQUIRE(decoded.size() == original.size());
    double best = -1.0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        best = std::max(best, correlation(original, decoded, lag));
    }
    INFO("best correlation within the allowed lag: " << best);
    CHECK(best > 0.95);
    for (const int lag : {-40, -16, -8, -3, -2, -1, 1, 2, 3, 8, 16, 40}) {
        if (std::abs(lag) <= max_lag) continue;
        CHECK(correlation(original, decoded, lag) < best);
    }
}

std::int64_t max_abs(const std::vector<std::int16_t>& v) {
    std::int64_t m = 0;
    for (const std::int16_t s : v) m = std::max<std::int64_t>(m, std::abs(static_cast<int>(s)));
    return m;
}

bool on_disk(const std::filesystem::path& p) { return std::filesystem::exists(p); }

// A pulled session on the scripted backend and VAD, writing a track of a tape.
struct Rig {
    Rig(std::shared_ptr<Tape> tape, const std::string& track) {
        SessionConfig config;
        config.tape = std::move(tape);
        config.tape_track = track;
        session = std::make_unique<Session>(backend, vad, config, [](const SessionEvent&) {});
    }
    // Pushes in 100 ms pieces, as a capture would.
    void push(const std::vector<float>& pcm) {
        for (std::size_t at = 0; at < pcm.size(); at += 1600) {
            const std::size_t n = std::min<std::size_t>(1600, pcm.size() - at);
            session->push(std::span<const float>(pcm.data() + at, n), 16000, 1);
        }
    }
    ts::LiveBackend backend;
    ts::LiveVad vad;
    std::unique_ptr<Session> session;  // last, so it goes before the fakes
};

nlohmann::json tracks(const Tape& tape) { return nlohmann::json::parse(tape.tracks_json()); }

// --- An Ogg Opus reader written for the tests, independent of src/audio/ogg_opus.cpp ---------

std::uint32_t crc_bitwise(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0;
    for (const std::uint8_t b : bytes) {
        crc ^= static_cast<std::uint32_t>(b) << 24;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04c11db7u : crc << 1;
        }
    }
    return crc;
}

struct OggFile {
    std::vector<std::vector<std::uint8_t>> packets;  // every packet, headers first
    std::vector<std::int64_t> granules;              // the granule position of every page
    std::int64_t last_granule = -1;
    bool bos_first = false;
    bool eos_last = false;
    bool sequence_ok = true;
    bool crc_ok = true;
    bool single_serial = true;
};

OggFile parse_ogg(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    OggFile f;
    std::size_t at = 0;
    std::uint32_t expected_seq = 0;
    std::uint32_t serial = 0;
    std::vector<std::uint8_t> packet;
    std::uint8_t flags = 0;
    while (at < data.size()) {
        REQUIRE(at + 27 <= data.size());
        REQUIRE(std::memcmp(&data[at], "OggS", 4) == 0);
        flags = data[at + 5];
        std::int64_t granule = 0;
        for (int i = 7; i >= 0; --i) granule = (granule << 8) | data[at + 6 + i];
        std::uint32_t page_serial = 0, seq = 0, crc = 0;
        for (int i = 3; i >= 0; --i) page_serial = (page_serial << 8) | data[at + 14 + i];
        for (int i = 3; i >= 0; --i) seq = (seq << 8) | data[at + 18 + i];
        for (int i = 3; i >= 0; --i) crc = (crc << 8) | data[at + 22 + i];
        if (at == 0) {
            serial = page_serial;
            f.bos_first = (flags & 2) != 0;
        }
        f.single_serial = f.single_serial && page_serial == serial;
        f.sequence_ok = f.sequence_ok && seq == expected_seq++;
        const std::size_t segments = data[at + 26];
        std::size_t body = 0;
        for (std::size_t i = 0; i < segments; ++i) body += data[at + 27 + i];
        const std::size_t page_size = 27 + segments + body;
        REQUIRE(at + page_size <= data.size());
        std::vector<std::uint8_t> page(data.begin() + static_cast<std::ptrdiff_t>(at),
                                       data.begin() + static_cast<std::ptrdiff_t>(at + page_size));
        std::fill(page.begin() + 22, page.begin() + 26, std::uint8_t{0});
        f.crc_ok = f.crc_ok && crc_bitwise(page) == crc;
        std::size_t p = at + 27 + segments;
        for (std::size_t i = 0; i < segments; ++i) {
            const std::uint8_t lace = data[at + 27 + i];
            packet.insert(packet.end(), data.begin() + static_cast<std::ptrdiff_t>(p),
                          data.begin() + static_cast<std::ptrdiff_t>(p + lace));
            p += lace;
            if (lace < 255) {
                f.packets.push_back(std::move(packet));
                packet.clear();
            }
        }
        f.granules.push_back(granule);
        f.last_granule = granule;
        at += page_size;
    }
    f.eos_last = (flags & 4) != 0;
    return f;
}

// Decodes a whole Ogg Opus file the way a player does: pre-skip from the head, and the length
// from the last granule position. Returns 16 kHz samples.
std::vector<std::int16_t> play(const std::filesystem::path& path) {
    const OggFile f = parse_ogg(path);
    CHECK(f.bos_first);
    CHECK(f.eos_last);
    CHECK(f.sequence_ok);
    CHECK(f.crc_ok);
    CHECK(f.single_serial);
    REQUIRE(f.packets.size() >= 3);
    const auto& head = f.packets[0];
    REQUIRE(head.size() == 19);
    CHECK(std::memcmp(head.data(), "OpusHead", 8) == 0);
    CHECK(head[8] == 1);  // version
    CHECK(head[9] == 1);  // channels
    const int pre_skip = head[10] | (head[11] << 8);
    const std::uint32_t rate = head[12] | (head[13] << 8) | (head[14] << 16) |
                               (static_cast<std::uint32_t>(head[15]) << 24);
    CHECK(rate == 16000);
    CHECK(std::memcmp(f.packets[1].data(), "OpusTags", 8) == 0);

    int error = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &error);
    REQUIRE(error == OPUS_OK);
    std::vector<std::int16_t> out48;
    std::vector<std::int16_t> frame(5760);
    for (std::size_t i = 2; i < f.packets.size(); ++i) {
        const int n =
            opus_decode(dec, f.packets[i].data(), static_cast<opus_int32>(f.packets[i].size()),
                        frame.data(), 5760, 0);
        REQUIRE(n > 0);
        out48.insert(out48.end(), frame.begin(), frame.begin() + n);
    }
    opus_decoder_destroy(dec);
    const std::int64_t length48 = f.last_granule - pre_skip;
    REQUIRE(length48 >= 0);
    REQUIRE(static_cast<std::int64_t>(out48.size()) >= f.last_granule);
    // 48 kHz to 16 kHz by taking every third sample: the content is band-limited to 8 kHz by the
    // 16 kHz input, so this is close enough to compare.
    std::vector<std::int16_t> out;
    for (std::int64_t i = pre_skip; i < f.last_granule; i += 3) {
        out.push_back(out48[static_cast<std::size_t>(i)]);
    }
    return out;
}

}  // namespace

TEST_CASE("tape: memory-to-page handoff at the 2 min boundary") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    Rig rig(tape, "mic");
    const std::vector<float> pcm = voice(3 * kMinute * kSpm + 10 * kSpm, 1);
    const std::vector<std::int16_t> s16 = to_s16(pcm);

    // Exactly 2 minutes: all of it in memory, nothing encoded yet.
    rig.push(std::vector<float>(pcm.begin(), pcm.begin() + 2 * kMinute * kSpm));
    CHECK_FALSE(on_disk(dir.path / "mic.00001.opus"));
    CHECK(read(*tape, "mic", 0, kAll) == slice(s16, 0, 2 * kMinute));

    // One more minute: the oldest minute fills the first page, which is written at once.
    rig.push(std::vector<float>(pcm.begin() + 2 * kMinute * kSpm, pcm.end()));
    REQUIRE(on_disk(dir.path / "mic.00001.opus"));
    CHECK_FALSE(on_disk(dir.path / "mic.00002.opus"));
    CHECK(tape->duration_ms() == 3 * kMinute + 10);

    // The newest 2 minutes are still exact; the first minute comes back through Opus.
    const auto newest = read(*tape, "mic", kMinute + 10, kAll);
    CHECK(newest == slice(s16, kMinute + 10, 3 * kMinute + 10));
    check_close(read(*tape, "mic", 0, kMinute), slice(s16, 0, kMinute));
    // One read across the handoff: decoded, then exact, in one piece.
    const auto across = read(*tape, "mic", kMinute - 1000, kMinute + 2000);
    check_close(across, slice(s16, kMinute - 1000, kMinute + 2000));
    CHECK(std::equal(across.end() - 1000 * kSpm, across.end(),
                     s16.begin() + (kMinute + 1000) * kSpm));
}

TEST_CASE("tape: each page file starts its own timeline, and the joined file runs on") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    const std::vector<float> pcm = voice(3 * kMinute * kSpm + 10 * kSpm, 3);
    const std::vector<std::int16_t> s16 = to_s16(pcm);
    auto rig = std::make_unique<Rig>(tape, "mic");
    rig->push(pcm);
    // Granules count 48 kHz samples from the page file's own first packet. A page holds 50
    // packets of 20 ms, so a full page file ends at 60 s of audio, not at its place on the tape.
    constexpr std::int64_t kPageGranules = 60 * 48000;
    REQUIRE(on_disk(dir.path / "mic.00001.opus"));
    CHECK(parse_ogg(dir.path / "mic.00001.opus").last_granule == kPageGranules);
    // A player drops the pre-skip (about 6 ms), so the page plays for a minute less that much.
    const auto length = static_cast<std::int64_t>(play(dir.path / "mic.00001.opus").size());
    CHECK(std::abs(length - kMinute * kSpm) <= 10 * kSpm);
    // The reads still place the audio at its time on the tape.
    check_close(read(*tape, "mic", kMinute + 5000, kMinute + 6000),
                slice(s16, kMinute + 5000, kMinute + 6000));

    rig.reset();
    tape->close();
    // The joined file's granules rise page by page: each page holds 1 s of packets, and only the
    // last page is trimmed to the track's end.
    const OggFile joined = parse_ogg(dir.path / "mic.opus");
    REQUIRE(joined.granules.size() > 3);
    for (std::size_t i = 2; i + 1 < joined.granules.size(); ++i) {
        CHECK(joined.granules[i] == static_cast<std::int64_t>(i - 1) * 48000);
    }
    for (std::size_t i = 3; i < joined.granules.size(); ++i) {
        CHECK(joined.granules[i] > joined.granules[i - 1]);
    }
    CHECK(play(dir.path / "mic.opus").size() == s16.size());
}

TEST_CASE("tape: a read across a page edge decodes both pages in time") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    Rig rig(tape, "mic");
    const std::vector<float> pcm = voice(5 * kMinute * kSpm, 2);
    rig.push(pcm);
    REQUIRE(on_disk(dir.path / "mic.00003.opus"));
    const std::vector<std::int16_t> s16 = to_s16(pcm);
    for (const std::int64_t edge : {kMinute, 2 * kMinute}) {
        INFO("page edge at " << edge << " ms");
        check_close(read(*tape, "mic", edge - 700, edge + 700), slice(s16, edge - 700, edge + 700));
    }
    // Short reads that start just after an edge, and one of a single ms.
    check_close(read(*tape, "mic", kMinute + 3, kMinute + 503),
                slice(s16, kMinute + 3, kMinute + 503));
    CHECK(read(*tape, "mic", 2 * kMinute, 2 * kMinute + 1).size() == 16);
    // A count without a buffer decodes nothing and still gives the size.
    CHECK(tape->read_audio("mic", 0, 2 * kMinute, nullptr, 0) == 2 * kMinute * kSpm);
}

TEST_CASE("tape: paged audio reads back after its session ends, and after close") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    const std::vector<float> pcm = voice(4 * kMinute * kSpm, 3);
    const std::vector<std::int16_t> s16 = to_s16(pcm);
    {
        Rig rig(tape, "mic");
        rig.push(pcm);
    }
    // The session is gone; the page files are opened again for each read.
    const auto first = read(*tape, "mic", 30000, 90000);
    check_close(first, slice(s16, 30000, 90000));
    CHECK(read(*tape, "mic", 30000, 90000) == first);  // the same bytes decode the same way

    // A new session on the same track reads the old run through the tape as well.
    Rig again(tape, "mic");
    check_close(read(*tape, "mic", 30000, 90000), slice(s16, 30000, 90000));

    // After close the pages are joined into mic.opus and deleted, and reads come from that file,
    // also for the audio that was in memory.
    again.session.reset();
    tape->close();
    CHECK(on_disk(dir.path / "mic.opus"));
    CHECK_FALSE(on_disk(dir.path / "mic.00001.opus"));
    CHECK(read(*tape, "mic", 30000, 90000) == first);
    check_close(read(*tape, "mic", 3 * kMinute, 4 * kMinute), slice(s16, 3 * kMinute, 4 * kMinute));
    CHECK(tape->read_audio("mic", 0, kAll, nullptr, 0) == 4 * kMinute * kSpm);
}

TEST_CASE("tape: a later session starts at the end of the tape, after a seam") {
    auto tape = Tape::create({});
    const std::vector<float> a = voice(1005 * kSpm, 1);
    const std::vector<float> b = voice(2000 * kSpm, 2);
    {
        Rig rig(tape, "mic");
        CHECK(rig.session->tape_offset_ms() == 0);
        rig.push(a);
        CHECK(read(*rig.session, 0, kAll) == to_s16(a));
    }
    Rig rig(tape, "mic");
    // The run starts on the next whole 10 ms after the end of the tape.
    CHECK(rig.session->tape_offset_ms() == 1010);
    rig.push(b);
    // The session reads its own timeline; the tape reads both runs and the 5 ms between them.
    CHECK(read(*rig.session, 0, kAll) == to_s16(b));
    CHECK(read(*rig.session, 500, 700) == slice(to_s16(b), 500, 700));
    const auto all = read(*tape, "mic", 0, kAll);
    REQUIRE(all.size() == static_cast<std::size_t>(3010 * kSpm));
    CHECK(std::equal(all.begin(), all.begin() + 1005 * kSpm, to_s16(a).begin()));
    CHECK(std::all_of(all.begin() + 1005 * kSpm, all.begin() + 1010 * kSpm,
                      [](std::int16_t s) { return s == 0; }));
    CHECK(std::equal(all.begin() + 1010 * kSpm, all.end(), to_s16(b).begin()));

    const auto runs = tracks(*tape)["tracks"][0]["runs"];
    REQUIRE(runs.size() == 2);
    CHECK(runs[0]["start_ms"] == 0);
    CHECK(runs[0]["end_ms"] == 1005);
    CHECK(runs[1]["start_ms"] == 1010);
    CHECK(runs[1]["end_ms"] == 3010);
    CHECK(tracks(*tape)["tracks"][0]["writing"] == true);

    // reset() ends the run and starts another one at the end of the tape.
    rig.session->reset();
    CHECK(rig.session->tape_offset_ms() == 3010);
    CHECK(rig.session->read_audio(0, kAll, nullptr, 0) == 0);
    rig.push(a);
    CHECK(read(*rig.session, 0, kAll) == to_s16(a));
    CHECK(tracks(*tape)["tracks"][0]["runs"].size() == 3);
}

TEST_CASE("tape: three sessions write three tracks at once") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    tape->add_track("system", "System audio");
    const std::vector<std::string> names = {"mic", "system", "app:chat"};
    std::vector<std::unique_ptr<Rig>> rigs;
    for (const auto& name : names) {
        rigs.push_back(std::make_unique<Rig>(tape, name));
        CHECK(rigs.back()->session->tape_offset_ms() == 0);
    }
    // A track takes one writer at a time.
    CHECK_THROWS_AS(tape->attach("mic"), echoir::StateError);

    const std::int64_t length = 3 * kMinute * kSpm + 500 * kSpm;
    std::vector<std::vector<float>> pcm;
    for (int i = 0; i < 3; ++i) pcm.push_back(voice(length, i + 1));
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back(
            [&, i] { rigs[static_cast<std::size_t>(i)]->push(pcm[static_cast<std::size_t>(i)]); });
    }
    for (auto& t : threads) t.join();

    // In the order they were added: system first, by add_track.
    const auto json = tracks(*tape);
    REQUIRE(json["tracks"].size() == 3);
    CHECK(json["tracks"][0]["name"] == "system");
    CHECK(json["tracks"][0]["label"] == "System audio");
    CHECK(json["tracks"][1]["name"] == "mic");
    CHECK(json["tracks"][2]["label"] == "app:chat");
    CHECK(json["tracks"][2]["file"] == "app%3Achat.opus");
    for (int i = 0; i < 3; ++i) {
        const auto& name = names[static_cast<std::size_t>(i)];
        INFO("track " << name);
        const auto s16 = to_s16(pcm[static_cast<std::size_t>(i)]);
        CHECK(json["duration_ms"] == length / kSpm);
        CHECK(read(*tape, name, 2 * kMinute, kAll) == slice(s16, 2 * kMinute, length / kSpm));
        check_close(read(*tape, name, 10000, 40000), slice(s16, 10000, 40000));
    }
    CHECK(on_disk(dir.path / "app%3Achat.00001.opus"));
    rigs.clear();
    tape->close();
    for (const char* file : {"mic.opus", "system.opus", "app%3Achat.opus"}) {
        CHECK(on_disk(dir.path / file));
    }
}

TEST_CASE("tape: a track added mid-session joins the timeline with silence before it") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    Rig mic(tape, "mic");
    const std::vector<float> m = voice(4 * kMinute * kSpm, 1);
    mic.push(std::vector<float>(m.begin(), m.begin() + 70000 * kSpm));

    tape->add_track("system", "System");
    Rig system(tape, "system");
    CHECK(system.session->tape_offset_ms() == 70000);
    const std::vector<float> s = voice(170000 * kSpm, 2);
    // Both go on together.
    for (std::int64_t at = 0; at < 170000 * kSpm; at += 16000) {
        mic.push(std::vector<float>(m.begin() + 70000 * kSpm + at,
                                    m.begin() + 70000 * kSpm + at + 16000));
        system.push(std::vector<float>(s.begin() + at, s.begin() + at + 16000));
    }
    CHECK(tape->duration_ms() == 240000);
    // The silence before the system run: the first minute and a bit is paged, and reads back as
    // silence; the session's own timeline starts at its run.
    const auto before = read(*tape, "system", 0, 70000);
    REQUIRE(before.size() == static_cast<std::size_t>(70000 * kSpm));
    // Opus spreads a little of the onset into the ms before it (pre-echo), so the last second is
    // left out.
    CHECK(max_abs(std::vector<std::int16_t>(before.begin(), before.end() - 1000 * kSpm)) < 100);
    const auto s16 = to_s16(s);
    check_close(read(*tape, "system", 75000, 100000), slice(s16, 5000, 30000));
    check_close(read(*system.session, 5000, 30000), slice(s16, 5000, 30000));
    CHECK(read(*system.session, 120000, kAll) == slice(s16, 120000, 170000));
    const auto runs = tracks(*tape)["tracks"][1]["runs"];
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]["start_ms"] == 70000);
}

TEST_CASE("tape: the envelope covers the whole tape, across pages, seams and padding") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    const std::vector<float> a = voice(2 * kMinute * kSpm + 30005 * kSpm, 1);
    const std::vector<float> b = voice(kMinute * kSpm, 2);
    {
        Rig rig(tape, "mic");
        rig.push(a);
    }
    Rig rig(tape, "mic");
    rig.push(b);

    // The expected cells: run a, 5 ms of padding, run b.
    echoir::stream::Envelope expected;
    expected.append(to_s16(a));
    expected.append_silence(5 * kSpm);
    expected.append(to_s16(b));
    const std::size_t cells = expected.read(0, kAll, 10, nullptr, 0);
    REQUIRE(cells == static_cast<std::size_t>(tape->duration_ms() / 10));
    REQUIRE(tape->read_peaks("mic", 0, kAll, 10, nullptr, 0) == cells);
    std::vector<float> want(cells * 2), got(cells * 2);
    expected.read(0, kAll, 10, want.data(), cells);
    tape->read_peaks("mic", 0, kAll, 10, got.data(), cells);
    CHECK(want == got);

    // Through the session: its own cells, from its run on.
    const std::int64_t offset = rig.session->tape_offset_ms();
    const std::size_t own = rig.session->read_peaks(0, kAll, 10, nullptr, 0);
    CHECK(own == static_cast<std::size_t>(kMinute / 10));
    std::vector<float> session_cells(own * 2);
    rig.session->read_peaks(0, kAll, 10, session_cells.data(), own);
    CHECK(std::equal(session_cells.begin(), session_cells.end(), got.begin() + offset / 10 * 2));
    // Buckets of 1 s line up with the tape's because the run starts on a whole cell.
    std::vector<float> bucket(2), tape_bucket(2);
    rig.session->read_peaks(1000, 2000, 1000, bucket.data(), 1);
    tape->read_peaks("mic", offset + 1000, offset + 2000, 1000, tape_bucket.data(), 1);
    CHECK(bucket == tape_bucket);
}

TEST_CASE("tape: close writes a playable <track>.opus for each track") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    const std::vector<float> mic = voice(3 * kMinute * kSpm + 1234 * kSpm + 7, 1);
    const std::vector<float> sys = voice(45000 * kSpm, 2);
    {
        Rig m(tape, "mic");
        Rig s(tape, "system");
        m.push(mic);
        s.push(sys);
    }
    tape->add_track("empty", "");
    tape->close();
    CHECK(tape->closed());
    CHECK_NOTHROW(tape->close());

    std::vector<std::string> left;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
        left.push_back(entry.path().filename().string());
    }
    std::sort(left.begin(), left.end());
    CHECK(left == std::vector<std::string>{"mic.opus", "system.opus"});

    const auto mic_s16 = to_s16(mic);
    const auto played = play(dir.path / "mic.opus");
    // The file plays exactly the track's length, in time with what was written.
    REQUIRE(played.size() == mic_s16.size());
    check_close(
        std::vector<std::int16_t>(played.begin() + 1000 * kSpm, played.begin() + 31000 * kSpm),
        slice(mic_s16, 1000, 31000), 1);
    check_close(std::vector<std::int16_t>(played.end() - 20000 * kSpm, played.end()),
                std::vector<std::int16_t>(mic_s16.end() - 20000 * kSpm, mic_s16.end()), 1);
    // About 24 kbit/s.
    const auto bytes = std::filesystem::file_size(dir.path / "mic.opus");
    const double seconds = static_cast<double>(mic.size()) / 16000.0;
    CHECK(static_cast<double>(bytes) * 8.0 / seconds < 30000.0);

    const auto system_played = play(dir.path / "system.opus");
    CHECK(system_played.size() == sys.size());

    // A closed tape takes no new tracks or writers.
    CHECK_THROWS_AS(tape->add_track("late", ""), echoir::StateError);
    CHECK_THROWS_AS(tape->attach("mic"), echoir::StateError);
    CHECK(tracks(*tape)["closed"] == true);
}

TEST_CASE("tape: a checkpoint writes a playable file mid-session, and recording goes on") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    Rig rig(tape, "mic");
    // Three minutes and a bit: one page file, the rest in memory.
    const std::int64_t first = 3 * kMinute * kSpm + 1234 * kSpm + 7;
    const std::int64_t second = 130000 * kSpm + 3;
    const std::vector<float> a = voice(first, 1);
    const std::vector<float> b = voice(second, 1, first);
    rig.push(a);
    const auto before = read(*tape, "mic", 0, kAll);

    tape->checkpoint();
    CHECK(on_disk(dir.path / "mic.opus"));
    CHECK(on_disk(dir.path / "mic.00001.opus"));
    CHECK_FALSE(on_disk(dir.path / "mic.opus.part"));
    const auto a_s16 = to_s16(a);
    {
        const auto played = play(dir.path / "mic.opus");
        // The whole track so far, the paged part and the part in memory, in time.
        REQUIRE(played.size() == a_s16.size());
        check_close(
            std::vector<std::int16_t>(played.begin() + 1000 * kSpm, played.begin() + 31000 * kSpm),
            slice(a_s16, 1000, 31000), 1);
        check_close(std::vector<std::int16_t>(played.end() - 20000 * kSpm, played.end()),
                    std::vector<std::int16_t>(a_s16.end() - 20000 * kSpm, a_s16.end()), 1);
    }
    // The tape itself did not change: the same reads, the newest 2 minutes still exact.
    CHECK(read(*tape, "mic", 0, kAll) == before);
    CHECK(read(*tape, "mic", 3 * kMinute, 3 * kMinute + 1234) ==
          slice(a_s16, 3 * kMinute, 3 * kMinute + 1234));
    CHECK(tracks(*tape)["closed"] == false);

    // Recording goes on, and a later checkpoint replaces the file with the longer track.
    rig.push(b);
    tape->checkpoint();
    std::vector<std::int16_t> all_s16 = a_s16;
    const auto b_s16 = to_s16(b);
    all_s16.insert(all_s16.end(), b_s16.begin(), b_s16.end());
    CHECK(play(dir.path / "mic.opus").size() == all_s16.size());

    // The tape stays gapless across the first checkpoint: the reads there, now from the pages,
    // follow the signal with no shift.
    const std::int64_t at_ms = first / kSpm;
    check_close(read(*tape, "mic", at_ms - 2000, at_ms + 2000),
                slice(all_s16, at_ms - 2000, at_ms + 2000));

    // The close writes the final file over the checkpoint, gapless across both checkpoints.
    rig.session.reset();
    tape->close();
    const auto played = play(dir.path / "mic.opus");
    REQUIRE(played.size() == all_s16.size());
    check_close(std::vector<std::int16_t>(played.begin() + (at_ms - 2000) * kSpm,
                                          played.begin() + (at_ms + 2000) * kSpm),
                slice(all_s16, at_ms - 2000, at_ms + 2000), 1);
    std::vector<std::string> left;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
        left.push_back(entry.path().filename().string());
    }
    CHECK(left == std::vector<std::string>{"mic.opus"});

    // After the close, and on a tape without a folder, a checkpoint does nothing.
    CHECK_NOTHROW(tape->checkpoint());
    auto memory = Tape::create({});
    Rig m(memory, "mic");
    m.push(voice(1000 * kSpm, 2));
    CHECK_NOTHROW(memory->checkpoint());
}

TEST_CASE("tape: a checkpoint of short tracks, padding and an empty track") {
    TempFolder dir;
    auto tape = Tape::create(dir.path);
    tape->add_track("empty", "");
    const std::vector<float> mic = voice(2500 * kSpm + 5, 1);
    const std::vector<float> sys = voice(4000 * kSpm, 2);
    {
        Rig m(tape, "mic");
        m.push(mic);
    }
    Rig s(tape, "system");
    s.push(sys);
    tape->checkpoint();
    // No page file yet: the whole file comes from memory. The empty track gets no file.
    CHECK_FALSE(on_disk(dir.path / "empty.opus"));
    const auto mic_s16 = to_s16(mic);
    const auto sys_s16 = to_s16(sys);
    const auto mic_played = play(dir.path / "mic.opus");
    REQUIRE(mic_played.size() == mic.size());
    check_close(std::vector<std::int16_t>(mic_played.begin() + 100 * kSpm, mic_played.end()),
                std::vector<std::int16_t>(mic_s16.begin() + 100 * kSpm, mic_s16.end()), 1);
    // The system track starts after the mic's run, padded with silence before it.
    const std::int64_t offset = s.session->tape_offset_ms();
    CHECK(offset == 2510);
    const auto sys_played = play(dir.path / "system.opus");
    REQUIRE(sys_played.size() == static_cast<std::size_t>(offset * kSpm) + sys.size());
    CHECK(max_abs(std::vector<std::int16_t>(sys_played.begin(),
                                            sys_played.begin() + (offset - 20) * kSpm)) < 64);
    check_close(
        std::vector<std::int16_t>(sys_played.begin() + (offset + 100) * kSpm, sys_played.end()),
        std::vector<std::int16_t>(sys_s16.begin() + 100 * kSpm, sys_s16.end()), 1);
}

TEST_CASE("tape: a seam keeps the writer's place and starts a new run") {
    auto tape = Tape::create({});
    const std::vector<float> a = voice(1005 * kSpm, 1);
    const std::vector<float> b = voice(2000 * kSpm, 2);
    Rig other(tape, "system");
    other.push(voice(5000 * kSpm, 3));
    Rig rig(tape, "mic");
    const std::int64_t offset = rig.session->tape_offset_ms();
    CHECK(offset == 5000);
    // A seam before any audio is not a run of its own.
    rig.session->mark_seam();
    rig.push(a);
    rig.session->mark_seam();
    rig.session->mark_seam();
    rig.push(b);
    // No padding: the offset stays, and the session reads its audio back to back.
    CHECK(rig.session->tape_offset_ms() == offset);
    auto both = to_s16(a);
    const auto b_s16 = to_s16(b);
    both.insert(both.end(), b_s16.begin(), b_s16.end());
    CHECK(read(*rig.session, 0, kAll) == both);
    CHECK(read(*tape, "mic", offset, kAll) == both);
    const auto runs = tracks(*tape)["tracks"][1]["runs"];
    REQUIRE(runs.size() == 2);
    CHECK(runs[0]["start_ms"] == 5000);
    CHECK(runs[0]["end_ms"] == 6005);
    CHECK(runs[1]["start_ms"] == 6005);
    CHECK(runs[1]["end_ms"] == 8005);

    // Without a tape, a seam does nothing.
    ts::LiveBackend backend;
    ts::LiveVad vad;
    SessionConfig config;
    config.retain_audio_ms = 10000;
    Session plain(backend, vad, config, [](const SessionEvent&) {});
    plain.push(std::span<const float>(a.data(), a.size()), 16000, 1);
    plain.mark_seam();
    plain.push(std::span<const float>(b.data(), b.size()), 16000, 1);
    CHECK(read(plain, 0, kAll) == both);
}

TEST_CASE("tape: without a folder only the newest 2 minutes are kept") {
    auto tape = Tape::create({});
    Rig rig(tape, "mic");
    const std::vector<float> pcm = voice(150000 * kSpm, 1);
    rig.push(pcm);
    const auto s16 = to_s16(pcm);
    CHECK(read(*tape, "mic", 30000, kAll) == slice(s16, 30000, 150000));
    CHECK_THROWS_AS(tape->read_audio("mic", 29999, 31000, nullptr, 0), echoir::StateError);
    CHECK_THROWS_AS(rig.session->read_audio(0, 1000, nullptr, 0), echoir::StateError);
    // The envelope covers everything.
    CHECK(tape->read_peaks("mic", 0, kAll, 10, nullptr, 0) == 15000);
    // Closing drops the audio and keeps the envelope.
    tape->close();
    CHECK_THROWS_AS(tape->read_audio("mic", 100000, 101000, nullptr, 0), echoir::StateError);
    CHECK(tape->read_peaks("mic", 0, kAll, 10, nullptr, 0) == 15000);
    // The session goes on after close; its audio is no longer kept.
    rig.push(voice(1000 * kSpm, 1));
}

TEST_CASE("tape: track names and argument errors") {
    auto tape = Tape::create({});
    tape->add_track("mic", "Microphone");
    CHECK_THROWS_AS(tape->add_track("MIC", ""), std::invalid_argument);
    CHECK_THROWS_AS(tape->add_track("", ""), std::invalid_argument);
    CHECK_THROWS_AS(tape->add_track(std::string(65, 'x'), ""), std::invalid_argument);
    CHECK_NOTHROW(tape->add_track(std::string(64, 'x'), ""));
    CHECK_THROWS_AS(tape->read_audio("none", 0, 10, nullptr, 0), std::invalid_argument);
    CHECK_THROWS_AS(tape->read_audio("mic", -1, 10, nullptr, 0), std::invalid_argument);
    CHECK_THROWS_AS(tape->read_audio("mic", 20, 10, nullptr, 0), std::invalid_argument);
    CHECK_THROWS_AS(tape->read_peaks("mic", 0, 10, 15, nullptr, 0), std::invalid_argument);
    CHECK(tape->read_audio("mic", 0, kAll, nullptr, 0) == 0);
    CHECK(tape->duration_ms() == 0);
    // An attached track that is not on the tape is added, labelled with its name.
    {
        auto writer = tape->attach("zoom:Ann");
    }
    const auto json = tracks(*tape);
    CHECK(json["tracks"][2]["name"] == "zoom:Ann");
    CHECK(json["tracks"][2]["label"] == "zoom:Ann");
    CHECK(json["tracks"][2]["writing"] == false);

    // A session with a tape needs a track name.
    ts::LiveBackend backend;
    ts::LiveVad vad;
    SessionConfig config;
    config.tape = tape;
    CHECK_THROWS_AS(Session(backend, vad, config, [](const SessionEvent&) {}),
                    std::invalid_argument);
}

TEST_CASE("tape: the C ABI") {
    TempFolder dir;
    const std::string folder = (dir.path / "sub").string();
    echoir_tape* tape = echoir_tape_create(folder.c_str());
    REQUIRE(tape != nullptr);
    CHECK(std::filesystem::is_directory(folder));
    CHECK(echoir_tape_add_track(tape, "mic", "Microphone") == ECHOIR_OK);
    CHECK(echoir_tape_add_track(tape, "Mic", nullptr) == ECHOIR_ERR_INVALID_ARG);
    CHECK(std::string(echoir_last_error()).find("echoir_tape_add_track") == 0);
    CHECK(echoir_tape_add_track(tape, nullptr, nullptr) == ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_tape_add_track(nullptr, "x", nullptr) == ECHOIR_ERR_INVALID_ARG);

    const char* json = echoir_tape_tracks_json(tape);
    REQUIRE(json != nullptr);
    const auto parsed = nlohmann::json::parse(json);
    CHECK(parsed["tracks"][0]["label"] == "Microphone");
    CHECK(parsed["duration_ms"] == 0);

    int64_t duration = -1;
    CHECK(echoir_tape_duration_ms(tape, &duration) == ECHOIR_OK);
    CHECK(duration == 0);
    CHECK(echoir_tape_duration_ms(tape, nullptr) == ECHOIR_ERR_INVALID_ARG);

    size_t n = 99;
    CHECK(echoir_tape_read_audio(tape, "mic", 0, kAll, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 0);
    CHECK(echoir_tape_read_audio(tape, "none", 0, 10, nullptr, 0, &n) == ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_tape_read_audio(tape, "mic", 0, 10, nullptr, 0, nullptr) ==
          ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_tape_read_peaks(tape, "mic", 0, 10, 15, nullptr, 0, &n) == ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_tape_read_peaks(tape, "mic", 0, kAll, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 0);

    CHECK(echoir_tape_checkpoint(tape) == ECHOIR_OK);  // no audio: no file
    CHECK_FALSE(std::filesystem::exists(dir.path / "sub" / "mic.opus"));
    CHECK(echoir_tape_checkpoint(nullptr) == ECHOIR_ERR_INVALID_ARG);

    CHECK(echoir_tape_close(tape) == ECHOIR_OK);
    CHECK(echoir_tape_checkpoint(tape) == ECHOIR_OK);
    CHECK(echoir_tape_add_track(tape, "late", nullptr) == ECHOIR_ERR_STATE);
    echoir_tape_free(tape);
    echoir_tape_free(nullptr);

    echoir_tape* memory = echoir_tape_create(nullptr);
    REQUIRE(memory != nullptr);
    echoir_tape_free(memory);  // free closes
}
