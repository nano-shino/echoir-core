// C ABI sessions on the scripted backend (ECHOIR_FAKE_BACKEND builds only). No model files are
// read: the engine is created from a models directory that does not exist.
#include <algorithm>
#include <chrono>
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
