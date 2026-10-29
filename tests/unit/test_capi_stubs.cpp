// Tests of the C ABI in src/capi/capi.cpp: version string, params_init defaults, NULL handling
// and the thread-local last error. Tests go through the public header, the same way a host would.
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "doctest.h"
#include "echoir/echoir.h"

// Set by tests/CMakeLists.txt from the project version, so there is one source of truth.
#ifndef ECHOIR_EXPECTED_VERSION
#error "ECHOIR_EXPECTED_VERSION must be defined by tests/CMakeLists.txt"
#endif

TEST_CASE("version string matches the project version") {
    CHECK(std::string(echoir_version()) == ECHOIR_EXPECTED_VERSION);
}

TEST_CASE("engine params init sets struct_size and defaults") {
    echoir_engine_params p{};
    p.model_path = "stale";  // must be cleared by init
    p.n_threads = 9;         // must be reset to the default 0
    echoir_engine_params_init(&p);

    CHECK(p.struct_size == sizeof(echoir_engine_params));
    CHECK(p.backend == ECHOIR_BACKEND_WHISPER);
    CHECK(p.model_path == nullptr);
    CHECK(p.mmproj_path == nullptr);
    CHECK(p.vad_model_path == nullptr);
    CHECK(p.use_gpu == 1);
    CHECK(p.n_threads == 0);
}

TEST_CASE("session params init sets struct_size and defaults") {
    echoir_session_params p{};
    echoir_session_params_init(&p);

    CHECK(p.struct_size == sizeof(echoir_session_params));
    CHECK(std::string(p.language) == "auto");
    CHECK(p.min_chunk_ms == 500);
    CHECK(p.max_segment_ms == 10000);
    CHECK(p.emit_partials == 1);
}

TEST_CASE("params init with NULL does not crash and records an error") {
    echoir_engine_params_init(nullptr);
    CHECK(std::string(echoir_last_error()).find("echoir_engine_params_init") != std::string::npos);

    echoir_session_params_init(nullptr);
    CHECK(std::string(echoir_last_error()).find("echoir_session_params_init") != std::string::npos);
}

#ifndef ECHOIR_FAKE_BACKEND
TEST_CASE("engine create without model paths returns NULL and records the reason") {
    // All paths are NULL, so the call fails before any model is read.
    echoir_engine_params ep{};
    echoir_engine_params_init(&ep);

    CHECK(echoir_engine_create(&ep) == nullptr);
    CHECK(std::string(echoir_last_error()).find("echoir_engine_create") != std::string::npos);
}
#endif

TEST_CASE("session create with a NULL engine returns NULL and records the reason") {
    echoir_session_params sp{};
    echoir_session_params_init(&sp);

    CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("echoir_session_create") != std::string::npos);
}

// Session params with the defaults, for the checks below.
echoir_session_params default_session_params() {
    echoir_session_params sp{};
    echoir_session_params_init(&sp);
    return sp;
}

// True when this build has the VAD and the backends. Without them every session create is a stub
// that reports "not built", so the checks of the session params are not reached. Otherwise the
// test is skipped, and this says so.
bool engine_available() {
    const echoir_session_params sp = default_session_params();
    echoir_session_create(nullptr, &sp, nullptr, nullptr);
    if (std::string(echoir_last_error()).find("not built") == std::string::npos) {
        return true;
    }
    MESSAGE("no engine in this build: skipped");
    return false;
}

TEST_CASE("session create refuses NULL params before it looks at the engine") {
    if (!engine_available()) {
        return;
    }
    CHECK(echoir_session_create(nullptr, nullptr, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("params must not be NULL") != std::string::npos);
}

TEST_CASE("session create refuses a struct_size larger than echoir_session_params") {
    if (!engine_available()) {
        return;
    }
    echoir_session_params sp = default_session_params();
    sp.struct_size = static_cast<uint32_t>(sizeof(echoir_session_params) + 1);
    CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("struct_size is larger") != std::string::npos);
}

TEST_CASE("session create refuses a struct_size that stops before emit_partials") {
    if (!engine_available()) {
        return;
    }
    echoir_session_params sp = default_session_params();
    sp.struct_size = static_cast<uint32_t>(offsetof(echoir_session_params, stream_policy) - 1);
    CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("struct_size is smaller") != std::string::npos);
}

TEST_CASE("session create passes an old-size struct on, and does not read stream_policy") {
    if (!engine_available()) {
        return;
    }
    // The struct ends at emit_partials, so stream_policy is beyond struct_size and is not read.
    // The create gets past the params, and fails only for the missing engine.
    echoir_session_params sp = default_session_params();
    sp.struct_size = static_cast<uint32_t>(offsetof(echoir_session_params, stream_policy));
    sp.stream_policy = 99;
    CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("engine must not be NULL") != std::string::npos);
}

TEST_CASE("session create refuses an unknown stream_policy") {
    if (!engine_available()) {
        return;
    }
    echoir_session_params sp = default_session_params();
    sp.stream_policy = 99;
    CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
    CHECK(std::string(echoir_last_error()).find("stream_policy is not a known policy") !=
          std::string::npos);
}

TEST_CASE("session create passes the agreement and rollback policies on to the engine check") {
    if (!engine_available()) {
        return;
    }
    const int32_t policies[] = {ECHOIR_POLICY_AGREEMENT, ECHOIR_POLICY_ROLLBACK};
    for (const int32_t policy : policies) {
        echoir_session_params sp = default_session_params();
        sp.stream_policy = policy;
        CHECK(echoir_session_create(nullptr, &sp, nullptr, nullptr) == nullptr);
        CHECK(std::string(echoir_last_error()).find("engine must not be NULL") !=
              std::string::npos);
    }
}

TEST_CASE("session calls on a NULL session fail without dereferencing it") {
    // Session handles are NULL here, so no call may reach a session. The status is an error in
    // every build; the message names the call.
    echoir_event ev{};
    const float pcm_f32[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int16_t pcm_s16[4] = {0, 0, 0, 0};

    CHECK(echoir_session_poll(nullptr, &ev) != ECHOIR_OK);
    CHECK(echoir_session_push_f32(nullptr, pcm_f32, 4, 16000, 1) != ECHOIR_OK);
    CHECK(echoir_session_push_s16(nullptr, pcm_s16, 4, 16000, 1) != ECHOIR_OK);
    CHECK(echoir_session_process(nullptr) != ECHOIR_OK);
    CHECK(echoir_session_flush(nullptr) != ECHOIR_OK);
    CHECK(echoir_session_reset(nullptr) != ECHOIR_OK);
    CHECK(echoir_session_clear(nullptr) != ECHOIR_OK);
    CHECK(std::string(echoir_last_error()).find("echoir_session_clear") != std::string::npos);
}

TEST_CASE("start and stop reject a NULL session") {
    CHECK(echoir_session_start(nullptr) != ECHOIR_OK);
    CHECK(echoir_session_stop(nullptr) != ECHOIR_OK);
    CHECK(std::string(echoir_last_error()).find("echoir_session_stop") != std::string::npos);
}

TEST_CASE("free functions accept NULL") {
    echoir_engine_free(nullptr);
    echoir_session_free(nullptr);
    CHECK(true);  // reaching this line means neither call crashed
}

TEST_CASE("last error is thread-local") {
    // Fresh threads start with no error, and their failures do not reach this thread.
    echoir_session_start(nullptr);
    const std::string mine = echoir_last_error();
    CHECK(mine.find("echoir_session_start") != std::string::npos);

    std::string other_before;
    std::string other_after;
    std::thread worker([&other_before, &other_after] {
        other_before = echoir_last_error();
        echoir_session_stop(nullptr);
        other_after = echoir_last_error();
    });
    worker.join();

    CHECK(other_before.empty());
    CHECK(other_after.find("echoir_session_stop") != std::string::npos);
    CHECK(std::string(echoir_last_error()) == mine);
}
