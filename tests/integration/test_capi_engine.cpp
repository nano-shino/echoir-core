// C ABI engine create and free. This file builds into its own test binary, which links only the
// echoir library (tests/CMakeLists.txt). The core objects are not linked next to it, so the
// backend logging state exists once in the process. It uses only the public header, as a host
// would. Cases that need a model skip when the model is missing.
#include <cstddef>
#include <cstdint>
#include <string>

#include "doctest.h"
#include "echoir/echoir.h"
#include "test_support.h"

namespace {

bool last_error_contains(const std::string& text) {
    return std::string(echoir_last_error()).find(text) != std::string::npos;
}

// Engine params with the given paths and the defaults for a CPU engine. The caller keeps the
// strings alive for the call.
echoir_engine_params engine_params(const char* model, const char* vad) {
    echoir_engine_params params{};
    echoir_engine_params_init(&params);
    params.model_path = model;
    params.vad_model_path = vad;
    params.use_gpu = 0;
    return params;
}

}  // namespace

TEST_CASE("engine create rejects NULL params") {
    CHECK(echoir_engine_create(nullptr) == nullptr);
    CHECK(last_error_contains("echoir_engine_create: params must not be NULL"));
}

TEST_CASE("engine create rejects a struct_size smaller than the published struct") {
    echoir_engine_params params = engine_params("no-such-model.bin", "no-such-vad.bin");
    params.struct_size = sizeof(echoir_engine_params) - 1;
    CHECK(echoir_engine_create(&params) == nullptr);
    CHECK(last_error_contains("struct_size"));
}

// The fake build loads no files, so the cases that check missing or unset model paths are left out.
#ifndef ECHOIR_FAKE_BACKEND
TEST_CASE("engine create requires a VAD model path") {
    // The VAD path is checked before any model loads, so the model path need not exist.
    echoir_engine_params params = engine_params("no-such-model.bin", nullptr);
    CHECK(echoir_engine_create(&params) == nullptr);
    CHECK(last_error_contains("vad_model_path is required"));

    params.vad_model_path = "";
    CHECK(echoir_engine_create(&params) == nullptr);
    CHECK(last_error_contains("vad_model_path is required"));
}

TEST_CASE("engine create reports a missing model in the last error") {
    const std::string model =
        (echoir::test::source_dir() / "models" / "no-such-model.bin").string();
    echoir_engine_params params = engine_params(model.c_str(), "no-such-vad.bin");
    CHECK(echoir_engine_create(&params) == nullptr);
    // Engine create returns a pointer, so the MODEL_LOAD status is not visible here. The message
    // names the function and the file.
    CHECK(last_error_contains("echoir_engine_create: "));
    CHECK(last_error_contains("no-such-model.bin"));
}

TEST_CASE("engine create reports a missing VAD model in the last error") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");

    const std::string model_path = model->string();
    echoir_engine_params params = engine_params(model_path.c_str(), "no-such-vad.bin");
    CHECK(echoir_engine_create(&params) == nullptr);
    CHECK(last_error_contains("no-such-vad.bin"));
}
#endif

TEST_CASE("engine create loads whisper-tiny-en and silero, then frees the engine") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto vad = echoir::test::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(vad, "silero-v6.2.0 (vad) model");

    const std::string model_path = model->string();
    const std::string vad_path = vad->string();
    echoir_engine_params params = engine_params(model_path.c_str(), vad_path.c_str());
    echoir_engine* engine = echoir_engine_create(&params);
    REQUIRE(engine != nullptr);
    echoir_engine_free(engine);
}

TEST_CASE("session create takes stream_policy by struct_size, and refuses rollback on whisper") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto vad = echoir::test::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(vad, "silero-v6.2.0 (vad) model");

    const std::string model_path = model->string();
    const std::string vad_path = vad->string();
    echoir_engine_params params = engine_params(model_path.c_str(), vad_path.c_str());
    echoir_engine* engine = echoir_engine_create(&params);
    REQUIRE(engine != nullptr);

    // A struct from before stream_policy: the field is beyond struct_size, so it is not read.
    echoir_session_params sp{};
    echoir_session_params_init(&sp);
    sp.struct_size = static_cast<uint32_t>(offsetof(echoir_session_params, stream_policy));
    sp.stream_policy = 99;
    echoir_session* session = echoir_session_create(engine, &sp, nullptr, nullptr);
    REQUIRE(session != nullptr);
    echoir_session_free(session);

    // The full struct, with the agreement policy.
    echoir_session_params_init(&sp);
    sp.stream_policy = ECHOIR_POLICY_AGREEMENT;
    session = echoir_session_create(engine, &sp, nullptr, nullptr);
    REQUIRE(session != nullptr);
    echoir_session_free(session);

    // Whisper has no decoder prefix, so the rollback policy is refused and the engine is free.
    echoir_session_params_init(&sp);
    sp.stream_policy = ECHOIR_POLICY_ROLLBACK;
    CHECK(echoir_session_create(engine, &sp, nullptr, nullptr) == nullptr);
    CHECK(last_error_contains("prefix"));
    // The refused create does not hold the engine, so the next one succeeds.
    echoir_session_params_init(&sp);
    session = echoir_session_create(engine, &sp, nullptr, nullptr);
    REQUIRE(session != nullptr);
    echoir_session_free(session);

    // A struct from a newer header, and an unknown policy, are refused.
    echoir_session_params_init(&sp);
    sp.struct_size = static_cast<uint32_t>(sizeof(echoir_session_params) + 1);
    CHECK(echoir_session_create(engine, &sp, nullptr, nullptr) == nullptr);
    CHECK(last_error_contains("struct_size is larger"));
    echoir_session_params_init(&sp);
    sp.stream_policy = 99;
    CHECK(echoir_session_create(engine, &sp, nullptr, nullptr) == nullptr);
    CHECK(last_error_contains("stream_policy is not a known policy"));

    echoir_engine_free(engine);
}

TEST_CASE("freeing NULL is a no-op") {
    echoir_engine_free(nullptr);
    echoir_session_free(nullptr);
    CHECK(true);  // reaching this line means neither call crashed
}

TEST_CASE("model params init sets struct_size and defaults") {
    echoir_model_params p{};
    p.model_id = "stale";  // must be cleared by init
    p.use_gpu = 0;
    p.n_threads = 9;
    echoir_model_params_init(&p);

    CHECK(p.struct_size == sizeof(echoir_model_params));
    CHECK(p.registry_json == nullptr);
    CHECK(p.models_dir == nullptr);
    CHECK(p.model_id == nullptr);
    CHECK(p.vad_id == nullptr);
    CHECK(p.use_gpu == 1);
    CHECK(p.n_threads == 0);
}

TEST_CASE("model params init with NULL does not crash and records an error") {
    echoir_model_params_init(nullptr);
    CHECK(last_error_contains("echoir_model_params_init"));
}

TEST_CASE("engine create from model refuses a struct_size larger than echoir_model_params") {
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.model_id = "whisper-tiny-en";
    p.models_dir = ".";
    p.struct_size = static_cast<uint32_t>(sizeof(echoir_model_params) + 1);
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("struct_size is larger"));
}

TEST_CASE("engine create from model names an unknown id in the last error") {
    const std::string dir = (echoir::test::source_dir() / "models").string();
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.model_id = "no-such-id";
    p.models_dir = dir.c_str();
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("echoir_engine_create_from_model"));
    CHECK(last_error_contains("no-such-id"));
}

TEST_CASE("engine create from model requires a model id and a models directory") {
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.models_dir = ".";
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("model_id is required"));

#ifndef ECHOIR_FAKE_BACKEND
    p.model_id = "whisper-tiny-en";
    p.models_dir = nullptr;
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("models_dir is required"));
#endif
}

// The fake build loads no files, so a missing file is not an error there.
#ifndef ECHOIR_FAKE_BACKEND
TEST_CASE("engine create from model reports a missing file by name and id") {
    const std::string dir = (echoir::test::source_dir() / "no-such-models-dir").string();
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.model_id = "whisper-tiny-en";
    p.models_dir = dir.c_str();
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("ggml-tiny.en.bin"));
    CHECK(last_error_contains("whisper-tiny-en"));
}
#endif

TEST_CASE("engine create from model loads whisper-tiny-en from the models directory") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto vad = echoir::test::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(vad, "silero-v6.2.0 (vad) model");

    const std::string dir = (echoir::test::source_dir() / "models").string();
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.model_id = "whisper-tiny-en";
    p.models_dir = dir.c_str();
    p.use_gpu = 0;
    echoir_engine* engine = echoir_engine_create_from_model(&p);
    REQUIRE(engine != nullptr);
    echoir_engine_free(engine);
}

TEST_CASE("models json parses and lists devices, default VADs and the catalogue") {
    const char* text = echoir_models_json(nullptr, nullptr);
    REQUIRE(text != nullptr);
    const nlohmann::json doc = nlohmann::json::parse(text);

    REQUIRE(doc.at("devices").is_array());
    CHECK_FALSE(doc.at("devices").empty());
    for (const nlohmann::json& device : doc.at("devices")) {
        CHECK(device.at("type").is_string());
        CHECK(device.at("memory_free_mb").is_number_integer());
        CHECK(device.at("description").is_string());
        CHECK(device.at("integrated").is_boolean());
    }
    CHECK(doc.at("default_vad").at("whisper") == "silero-v6.2.0");

    bool found = false;
    for (const nlohmann::json& model : doc.at("models")) {
        if (model.at("id") != "whisper-tiny-en") {
            continue;
        }
        found = true;
        CHECK(model.at("present") == false);  // no models directory: nothing is present
        CHECK(model.at("fits").is_boolean());
        REQUIRE(model.at("files").size() == 1);
        CHECK(model.at("files")[0].at("present") == false);
    }
    CHECK(found);
}

TEST_CASE("models json reads the models directory and the given catalogue") {
    const std::string dir = (echoir::test::source_dir() / "models").string();
    const char* text = echoir_models_json(nullptr, dir.c_str());
    REQUIRE(text != nullptr);
    const nlohmann::json doc = nlohmann::json::parse(text);
    for (const nlohmann::json& model : doc.at("models")) {
        CHECK(model.at("present").is_boolean());
    }

    const char* custom = R"({"default_vad": {"whisper": "v"}, "models": []})";
    const char* only = echoir_models_json(custom, nullptr);
    REQUIRE(only != nullptr);
    const nlohmann::json small = nlohmann::json::parse(only);
    CHECK(small.at("models").empty());
}

TEST_CASE("models json reports a catalogue that does not parse") {
    CHECK(echoir_models_json("not json", nullptr) == nullptr);
    CHECK(last_error_contains("echoir_models_json"));
}
