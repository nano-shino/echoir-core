// C ABI capture: the device list, and a session that captures its own audio. These cases run on
// miniaudio's null backend (ECHOIR_AUDIO_BACKEND=null, set by tests/CMakeLists.txt), which has one
// silent capture device and no loopback. Cases that need a speech model skip when it is missing.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "echoir/echoir.h"
#include "json.hpp"
#include "test_support.h"

namespace {

using json = nlohmann::json;

// Counts the events a session delivers. quiet is set once echoir_session_stop() has returned, so
// any callback after that point is a late event.
struct Probe {
    std::atomic<int> events{0};
    std::atomic<int> late{0};
    std::atomic<bool> quiet{false};
};

void on_event(const echoir_event*, void* user) {
    auto* probe = static_cast<Probe*>(user);
    if (probe->quiet.load()) {
        probe->late.fetch_add(1);
    }
    probe->events.fetch_add(1);
}

// The model paths of whisper-tiny-en and the Silero VAD. Absent when either file is missing.
struct Models {
    std::string model;
    std::string vad;
};

std::optional<Models> find_models() {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    const auto vad = echoir::test::model_file("silero-v6.2.0", "vad");
    if (!model || !vad) {
        return std::nullopt;
    }
    return Models{model->string(), vad->string()};
}

echoir_engine* create_engine(const Models& models) {
    echoir_engine_params params{};
    echoir_engine_params_init(&params);
    params.model_path = models.model.c_str();
    params.vad_model_path = models.vad.c_str();
    params.use_gpu = 0;
    return echoir_engine_create(&params);
}

echoir_session* create_session(echoir_engine* engine, Probe* probe) {
    echoir_session_params params{};
    echoir_session_params_init(&params);
    return echoir_session_create(engine, &params, on_event, probe);
}

}  // namespace

TEST_CASE("devices json lists capture devices and an empty loopback list on the null backend") {
    const char* text = echoir_devices_json();
    REQUIRE(text != nullptr);
    const json doc = json::parse(text);
    REQUIRE(doc.at("capture").is_array());
    REQUIRE(doc.at("loopback").is_array());
    CHECK(doc.at("loopback").empty());
    REQUIRE(!doc.at("capture").empty());
    for (const json& device : doc.at("capture")) {
        CHECK(device.at("id").is_string());
        CHECK(!device.at("id").get<std::string>().empty());
        CHECK(device.at("name").is_string());
        CHECK(device.at("default").is_boolean());
    }
}

TEST_CASE("start capture rejects a NULL session and an unknown source") {
    CHECK(echoir_session_start_capture(nullptr, ECHOIR_SOURCE_MIC, nullptr) ==
          ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_session_start_capture(nullptr, static_cast<echoir_source>(99), nullptr) ==
          ECHOIR_ERR_INVALID_ARG);
}

TEST_CASE("loopback is not implemented on the null backend") {
    const auto models = find_models();
    ECHOIR_REQUIRE_OR_SKIP(models, "whisper-tiny-en and silero-v6.2.0 (vad) models");
    echoir_engine* engine = create_engine(*models);
    REQUIRE(engine != nullptr);
    Probe probe;
    echoir_session* session = create_session(engine, &probe);
    REQUIRE(session != nullptr);

    CHECK(echoir_session_start_capture(session, ECHOIR_SOURCE_LOOPBACK, nullptr) ==
          ECHOIR_ERR_NOT_IMPLEMENTED);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("an unknown device id is an invalid argument") {
    const auto models = find_models();
    ECHOIR_REQUIRE_OR_SKIP(models, "whisper-tiny-en and silero-v6.2.0 (vad) models");
    echoir_engine* engine = create_engine(*models);
    REQUIRE(engine != nullptr);
    Probe probe;
    echoir_session* session = create_session(engine, &probe);
    REQUIRE(session != nullptr);

    CHECK(echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, "no-such-device") ==
          ECHOIR_ERR_INVALID_ARG);
    // A refused start leaves the session free to start again.
    CHECK(echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, nullptr) == ECHOIR_OK);
    CHECK(echoir_session_stop(session) == ECHOIR_OK);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("push is refused while the session captures") {
    const auto models = find_models();
    ECHOIR_REQUIRE_OR_SKIP(models, "whisper-tiny-en and silero-v6.2.0 (vad) models");
    echoir_engine* engine = create_engine(*models);
    REQUIRE(engine != nullptr);
    Probe probe;
    echoir_session* session = create_session(engine, &probe);
    REQUIRE(session != nullptr);

    REQUIRE(echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, nullptr) == ECHOIR_OK);
    const std::vector<float> samples(1600, 0.0f);
    CHECK(echoir_session_push_f32(session, samples.data(), 1600, 16000, 1) == ECHOIR_ERR_STATE);
    const std::vector<int16_t> pcm(1600, 0);
    CHECK(echoir_session_push_s16(session, pcm.data(), 1600, 16000, 1) == ECHOIR_ERR_STATE);
    CHECK(echoir_session_stop(session) == ECHOIR_OK);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("100 start and stop cycles deliver no late events") {
    const auto models = find_models();
    ECHOIR_REQUIRE_OR_SKIP(models, "whisper-tiny-en and silero-v6.2.0 (vad) models");
    echoir_engine* engine = create_engine(*models);
    REQUIRE(engine != nullptr);
    Probe probe;
    echoir_session* session = create_session(engine, &probe);
    REQUIRE(session != nullptr);

    for (int cycle = 0; cycle < 100; ++cycle) {
        probe.quiet = false;
        REQUIRE(echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, nullptr) == ECHOIR_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(echoir_session_stop(session) == ECHOIR_OK);
        probe.quiet = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(probe.late.load() == 0);

    echoir_session_free(session);
    echoir_engine_free(engine);
}

TEST_CASE("capture works in queue mode") {
    const auto models = find_models();
    ECHOIR_REQUIRE_OR_SKIP(models, "whisper-tiny-en and silero-v6.2.0 (vad) models");
    echoir_engine* engine = create_engine(*models);
    REQUIRE(engine != nullptr);
    echoir_session_params params{};
    echoir_session_params_init(&params);
    echoir_session* session = echoir_session_create(engine, &params, nullptr, nullptr);
    REQUIRE(session != nullptr);

    REQUIRE(echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, nullptr) == ECHOIR_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(echoir_session_stop(session) == ECHOIR_OK);
    // Queue mode: the events wait for poll, and an empty queue reports ECHOIR_ERR_STATE.
    echoir_event event{};
    while (echoir_session_poll(session, &event) == ECHOIR_OK) {
    }

    echoir_session_free(session);
    echoir_engine_free(engine);
}
