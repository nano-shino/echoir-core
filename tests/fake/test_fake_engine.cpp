// C ABI sessions on the scripted backend (ECHOIR_FAKE_BACKEND builds only). No model files are
// read: the engine is created from a models directory that does not exist.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "echoir/echoir.h"

namespace {

constexpr int kSampleRate = 16000;
constexpr std::size_t kBlock = 1600;  // 100 ms

struct Seen {
    echoir_event_type type;
    std::int64_t id;
    std::string text;
};

// Copies every queued event. The text pointers are valid only until the next poll, so they are
// copied here.
std::vector<Seen> drain(echoir_session* session) {
    std::vector<Seen> out;
    echoir_event ev{};
    while (echoir_session_poll(session, &ev) == ECHOIR_OK) {
        out.push_back(Seen{ev.type, ev.segment_id, ev.text != nullptr ? ev.text : ""});
    }
    return out;
}

echoir_engine* create_engine() {
    echoir_model_params params{};
    echoir_model_params_init(&params);
    params.model_id = "whisper-tiny-en";
    params.models_dir = "no-such-models-dir";
    return echoir_engine_create_from_model(&params);
}

// A tone is speech to the scripted VAD.
std::vector<float> speech(std::size_t samples) {
    std::vector<float> pcm(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        pcm[i] = 0.3f * static_cast<float>((i % 40) < 20 ? 1 : -1);
    }
    return pcm;
}

}  // namespace

TEST_CASE("fake build creates an engine from any params") {
    echoir_engine_params params{};
    echoir_engine_params_init(&params);
    echoir_engine* engine = echoir_engine_create(&params);
    CHECK(engine != nullptr);
    echoir_engine_free(engine);

    echoir_engine* from_model = create_engine();
    CHECK(from_model != nullptr);
    echoir_engine_free(from_model);
}

TEST_CASE("fake session emits a PARTIAL and a FINAL with the same id") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    // 2.5 s of speech in 100 ms pushes, decoded as it arrives, then flushed.
    const std::vector<float> pcm = speech(25 * kBlock);
    std::vector<Seen> events;
    for (std::size_t i = 0; i < 25; ++i) {
        REQUIRE(echoir_session_push_f32(session, pcm.data() + i * kBlock, kBlock, kSampleRate, 1) ==
                ECHOIR_OK);
        REQUIRE(echoir_session_process(session) == ECHOIR_OK);
        for (const Seen& e : drain(session)) events.push_back(e);
    }
    REQUIRE(echoir_session_flush(session) == ECHOIR_OK);
    for (const Seen& e : drain(session)) events.push_back(e);

    std::vector<std::int64_t> partial_ids;
    std::vector<std::int64_t> final_ids;
    for (const Seen& e : events) {
        if (e.type == ECHOIR_EVENT_PARTIAL) partial_ids.push_back(e.id);
        if (e.type == ECHOIR_EVENT_FINAL) final_ids.push_back(e.id);
    }
    REQUIRE_FALSE(partial_ids.empty());
    CHECK(final_ids.size() >= 2);
    for (std::int64_t id : partial_ids) {
        CHECK(std::find(final_ids.begin(), final_ids.end(), id) != final_ids.end());
    }
    for (const Seen& e : events) {
        if (e.type == ECHOIR_EVENT_FINAL) CHECK(e.text.rfind("fake ", 0) == 0);
    }

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("fake session live mode delivers FINALs on the worker") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    REQUIRE(echoir_session_start(session) == ECHOIR_OK);
    const std::vector<float> pcm = speech(3 * kSampleRate);
    REQUIRE(echoir_session_push_f32(session, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);

    // The worker decodes on its own thread. Poll for up to 10 s.
    int finals = 0;
    for (int tick = 0; tick < 1000 && finals < 2; ++tick) {
        for (const Seen& e : drain(session)) {
            if (e.type == ECHOIR_EVENT_FINAL) ++finals;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(finals >= 2);
    REQUIRE(echoir_session_stop(session) == ECHOIR_OK);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("fake session clear emits CLEARED") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    REQUIRE(echoir_session_clear(session) == ECHOIR_OK);
    bool cleared = false;
    for (const Seen& e : drain(session)) {
        if (e.type == ECHOIR_EVENT_CLEARED) cleared = true;
    }
    CHECK(cleared);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("fake backend refuses the rollback policy, as Whisper does") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    params.stream_policy = ECHOIR_POLICY_ROLLBACK;
    CHECK(echoir_session_create(engine, &params, nullptr, nullptr) == nullptr);
    echoir_engine_free(engine);
}

TEST_CASE("fake session keeps its audio, its envelope and the mute through the C ABI") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    params.retain_audio_ms = 1000;
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    // 1.5 s at 16 kHz, the last 500 ms muted.
    const std::vector<float> pcm = speech(15 * kBlock);
    REQUIRE(echoir_session_push_f32(session, pcm.data(), 10 * kBlock, kSampleRate, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_set_input_muted(session, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_push_f32(session, pcm.data() + 10 * kBlock, 5 * kBlock, kSampleRate,
                                    1) == ECHOIR_OK);
    REQUIRE(echoir_session_set_input_muted(session, 0) == ECHOIR_OK);

    size_t n = 0;
    REQUIRE(echoir_session_read_audio(session, 500, 2000, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 1000 * 16);
    std::vector<std::int16_t> audio(n);
    REQUIRE(echoir_session_read_audio(session, 500, 2000, audio.data(), audio.size(), &n) ==
            ECHOIR_OK);
    CHECK(audio[0] == static_cast<std::int16_t>(std::lround(pcm[5 * kBlock] * 32768.0f)));
    CHECK(
        std::all_of(audio.begin() + 500 * 16, audio.end(), [](std::int16_t s) { return s == 0; }));
    // The first 500 ms no longer fit in the 1 s kept.
    CHECK(echoir_session_read_audio(session, 0, 100, nullptr, 0, &n) == ECHOIR_ERR_STATE);
    CHECK(echoir_session_read_audio(session, 500, 2000, audio.data(), 10, &n) ==
          ECHOIR_ERR_INVALID_ARG);

    REQUIRE(echoir_session_read_peaks(session, 0, INT64_MAX, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 150);
    std::vector<float> peaks(2 * 3);
    REQUIRE(echoir_session_read_peaks(session, 0, 1500, 500, peaks.data(), 3, &n) == ECHOIR_OK);
    CHECK(n == 3);
    CHECK(peaks[1] > 0.29f);
    CHECK(peaks[4] == 0.0f);
    CHECK(peaks[5] == 0.0f);
    CHECK(echoir_session_read_peaks(session, 0, 100, 15, nullptr, 0, &n) == ECHOIR_ERR_INVALID_ARG);

    REQUIRE(echoir_session_reset(session) == ECHOIR_OK);
    REQUIRE(echoir_session_read_peaks(session, 0, INT64_MAX, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 0);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("fake session without retain_audio_ms keeps no audio") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);
    const std::vector<float> pcm = speech(kBlock);
    REQUIRE(echoir_session_push_f32(session, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);
    size_t n = 0;
    CHECK(echoir_session_read_audio(session, 0, 100, nullptr, 0, &n) == ECHOIR_ERR_STATE);
    CHECK(echoir_session_read_peaks(session, 0, 100, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 10);

    params.retain_audio_ms = -1;
    echoir_session_free(session);
    CHECK(echoir_session_create(engine, &params, nullptr, nullptr) == nullptr);
    echoir_engine_free(engine);
}

TEST_CASE("fake build: read_lag reports the audio not yet decoded") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    echoir_lag lag{};
    CHECK(echoir_session_read_lag(session, &lag) == ECHOIR_ERR_INVALID_ARG);  // struct_size 0
    lag.struct_size = sizeof(echoir_lag);
    REQUIRE(echoir_session_read_lag(session, &lag) == ECHOIR_OK);
    CHECK(lag.head_ms == 0);
    CHECK(lag.backlog_ms == 0);

    // Not started, so the pushed audio waits.
    const std::vector<float> pcm = speech(10 * kBlock);
    REQUIRE(echoir_session_push_f32(session, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_read_lag(session, &lag) == ECHOIR_OK);
    CHECK(lag.head_ms == 1000);
    CHECK(lag.decoded_ms == 0);
    CHECK(lag.backlog_ms == 1000);
    CHECK(lag.tick_ms == 0);

    REQUIRE(echoir_session_flush(session) == ECHOIR_OK);
    REQUIRE(echoir_session_read_lag(session, &lag) == ECHOIR_OK);
    CHECK(lag.decoded_ms == 1000);
    CHECK(lag.backlog_ms == 0);

    // A host built against the first version (struct_size up to tick_ms) keeps working, and a host
    // built against a later, larger struct gets the fields this version does not know zeroed.
    struct {
        echoir_lag known;
        int64_t later;
    } larger{};
    larger.known.struct_size = sizeof(larger);
    larger.later = 7;
    REQUIRE(echoir_session_read_lag(session, &larger.known) == ECHOIR_OK);
    CHECK(larger.known.head_ms == 1000);
    CHECK(larger.later == 0);
    lag.struct_size = offsetof(echoir_lag, tick_ms) + sizeof(lag.tick_ms);
    CHECK(echoir_session_read_lag(session, &lag) == ECHOIR_OK);
    lag.struct_size = offsetof(echoir_lag, tick_ms);
    CHECK(echoir_session_read_lag(session, &lag) == ECHOIR_ERR_INVALID_ARG);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("fake sessions write a tape through the C ABI, one run after another") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_tape* tape = echoir_tape_create(nullptr);
    REQUIRE(tape != nullptr);

    echoir_session_params params{};
    echoir_session_params_init(&params);
    CHECK(params.tape == nullptr);
    CHECK(params.tape_track == nullptr);
    params.tape = tape;
    // A tape needs a track name.
    CHECK(echoir_session_create(engine, &params, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("tape_track") != std::string::npos);
    params.tape_track = "mic";

    const std::vector<float> pcm = speech(15 * kBlock);
    echoir_session* first = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(first != nullptr);
    std::int64_t offset = -1;
    REQUIRE(echoir_session_tape_offset_ms(first, &offset) == ECHOIR_OK);
    CHECK(offset == 0);
    REQUIRE(echoir_session_push_f32(first, pcm.data(), 15 * kBlock, kSampleRate, 1) == ECHOIR_OK);
    echoir_session_free(first);

    // The tape outlives its first session; the second one starts after it. The handle can go
    // before the session: the session keeps the tape.
    echoir_session* second = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(echoir_session_tape_offset_ms(second, &offset) == ECHOIR_OK);
    CHECK(offset == 1500);
    REQUIRE(echoir_session_push_f32(second, pcm.data(), 5 * kBlock, kSampleRate, 1) == ECHOIR_OK);
    size_t n = 0;
    REQUIRE(echoir_session_read_audio(second, 0, INT64_MAX, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 500 * 16);
    REQUIRE(echoir_tape_read_audio(tape, "mic", 0, INT64_MAX, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 2000 * 16);
    REQUIRE(echoir_session_read_peaks(second, 0, INT64_MAX, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 50);
    std::int64_t duration = 0;
    REQUIRE(echoir_tape_duration_ms(tape, &duration) == ECHOIR_OK);
    CHECK(duration == 2000);
    echoir_tape_free(tape);
    // The closed tape kept no folder, so the session's audio is gone; the envelope is not.
    CHECK(echoir_session_read_audio(second, 0, 100, nullptr, 0, &n) == ECHOIR_ERR_STATE);
    CHECK(echoir_session_read_peaks(second, 0, INT64_MAX, 10, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 50);
    echoir_session_free(second);

    // A struct from an older header ends before the tape fields: they are not read.
    echoir_session_params old{};
    echoir_session_params_init(&old);
    old.struct_size = static_cast<std::uint32_t>(offsetof(echoir_session_params, tape));
    echoir_session* plain = echoir_session_create(engine, &old, nullptr, nullptr);
    REQUIRE(plain != nullptr);
    CHECK(echoir_session_tape_offset_ms(plain, &offset) == ECHOIR_ERR_STATE);
    echoir_session_free(plain);
    echoir_engine_free(engine);
}

namespace {

// Pushes `blocks` blocks of speech in 100 ms pushes, processing after each, then flushes, and
// returns the PARTIAL and FINAL events. The same ticks run every time, so the events do not depend
// on timing or on other sessions of the engine.
std::vector<Seen> pull_speech(echoir_session* session, std::size_t blocks) {
    const std::vector<float> pcm = speech(blocks * kBlock);
    std::vector<Seen> events;
    const auto take = [&] {
        for (const Seen& e : drain(session)) {
            if (e.type == ECHOIR_EVENT_PARTIAL || e.type == ECHOIR_EVENT_FINAL) events.push_back(e);
        }
    };
    for (std::size_t i = 0; i < blocks; ++i) {
        if (echoir_session_push_f32(session, pcm.data() + i * kBlock, kBlock, kSampleRate, 1) !=
                ECHOIR_OK ||
            echoir_session_process(session) != ECHOIR_OK) {
            return {};
        }
        take();
    }
    if (echoir_session_flush(session) != ECHOIR_OK) {
        return {};
    }
    take();
    return events;
}

bool same_events(const std::vector<Seen>& a, const std::vector<Seen>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].id != b[i].id || a[i].text != b[i].text) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("fake engine serves two sessions at once, each with its own events") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);

    // Each session alone first, for the events it must give with the other one running.
    echoir_session* solo = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(solo != nullptr);
    const std::vector<Seen> alone_a = pull_speech(solo, 35);
    echoir_session_free(solo);
    solo = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(solo != nullptr);
    const std::vector<Seen> alone_b = pull_speech(solo, 15);
    echoir_session_free(solo);
    REQUIRE(!alone_a.empty());
    REQUIRE(!alone_b.empty());

    echoir_session* a = echoir_session_create(engine, &params, nullptr, nullptr);
    echoir_session* b = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    std::vector<Seen> events_b;
    std::thread thread_b([&] { events_b = pull_speech(b, 15); });
    const std::vector<Seen> events_a = pull_speech(a, 35);
    thread_b.join();
    CHECK(same_events(events_a, alone_a));
    CHECK(same_events(events_b, alone_b));

    // Both live at once: each worker decodes its own audio.
    echoir_session* live_a = echoir_session_create(engine, &params, nullptr, nullptr);
    echoir_session* live_b = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(live_a != nullptr);
    REQUIRE(live_b != nullptr);
    REQUIRE(echoir_session_start(live_a) == ECHOIR_OK);
    REQUIRE(echoir_session_start(live_b) == ECHOIR_OK);
    const std::vector<float> pcm = speech(3 * kSampleRate);
    REQUIRE(echoir_session_push_f32(live_a, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_push_f32(live_b, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);
    int finals_a = 0;
    int finals_b = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((finals_a == 0 || finals_b == 0) && std::chrono::steady_clock::now() < deadline) {
        for (const Seen& e : drain(live_a)) finals_a += e.type == ECHOIR_EVENT_FINAL ? 1 : 0;
        for (const Seen& e : drain(live_b)) finals_b += e.type == ECHOIR_EVENT_FINAL ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(finals_a > 0);
    CHECK(finals_b > 0);
    REQUIRE(echoir_session_stop(live_a) == ECHOIR_OK);
    REQUIRE(echoir_session_stop(live_b) == ECHOIR_OK);

    // The engine goes after all of its sessions.
    echoir_session_free(live_a);
    echoir_session_free(a);
    echoir_session_free(live_b);
    echoir_session_free(b);
    echoir_engine_free(engine);
}

TEST_CASE("fake sessions on one engine write two tracks of a tape from the same start") {
    echoir_engine* engine = create_engine();
    REQUIRE(engine != nullptr);
    echoir_tape* tape = echoir_tape_create(nullptr);
    REQUIRE(tape != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    params.tape = tape;
    const std::vector<float> pcm = speech(15 * kBlock);

    // An earlier run leaves 1.5 s on the tape.
    params.tape_track = "mic";
    echoir_session* earlier = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(earlier != nullptr);
    REQUIRE(echoir_session_push_f32(earlier, pcm.data(), pcm.size(), kSampleRate, 1) == ECHOIR_OK);
    echoir_session_free(earlier);

    // Both sessions are created before either takes audio, so their runs start together.
    echoir_session* mic = echoir_session_create(engine, &params, nullptr, nullptr);
    params.tape_track = "system";
    echoir_session* system = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(mic != nullptr);
    REQUIRE(system != nullptr);
    std::int64_t mic_offset = -1;
    std::int64_t system_offset = -1;
    REQUIRE(echoir_session_tape_offset_ms(mic, &mic_offset) == ECHOIR_OK);
    REQUIRE(echoir_session_tape_offset_ms(system, &system_offset) == ECHOIR_OK);
    CHECK(mic_offset == 1500);
    CHECK(system_offset == 1500);

    REQUIRE(echoir_session_start(mic) == ECHOIR_OK);
    REQUIRE(echoir_session_start(system) == ECHOIR_OK);
    REQUIRE(echoir_session_push_f32(mic, pcm.data(), 10 * kBlock, kSampleRate, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_push_f32(system, pcm.data(), 10 * kBlock, kSampleRate, 1) == ECHOIR_OK);
    REQUIRE(echoir_session_stop(mic) == ECHOIR_OK);
    REQUIRE(echoir_session_stop(system) == ECHOIR_OK);
    size_t n = 0;
    REQUIRE(echoir_tape_read_audio(tape, "mic", 1500, INT64_MAX, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 1000 * 16);
    REQUIRE(echoir_tape_read_audio(tape, "system", 1500, INT64_MAX, nullptr, 0, &n) == ECHOIR_OK);
    CHECK(n == 1000 * 16);
    std::int64_t duration = 0;
    REQUIRE(echoir_tape_duration_ms(tape, &duration) == ECHOIR_OK);
    CHECK(duration == 2500);

    echoir_session_free(mic);
    echoir_session_free(system);
    echoir_tape_free(tape);
    echoir_engine_free(engine);
}
