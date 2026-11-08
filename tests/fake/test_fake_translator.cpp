// C ABI translator on the scripted translator (ECHOIR_FAKE_BACKEND builds only). No model file is
// read: the translator is created from a models directory that does not exist.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "echoir/echoir.h"

namespace {

echoir_translator* create_translator(const char* id = "hy-mt2-1.8b-q4_k_m") {
    echoir_translator_params params{};
    echoir_translator_params_init(&params);
    params.model_id = id;
    params.models_dir = "no-such-dir";
    return echoir_translator_create_from_model(&params);
}

echoir_translate_request request(const char* text, const char* target) {
    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = text;
    r.target = target;
    return r;
}

}  // namespace

TEST_CASE("fake translator creates from the default id and translates") {
    echoir_translator_params params{};
    echoir_translator_params_init(&params);
    params.models_dir = "no-such-dir";
    echoir_translator* t = echoir_translator_create_from_model(&params);
    REQUIRE(t != nullptr);

    echoir_translate_request r = request("  Good morning.  ", "vi");
    r.source = "en";
    const echoir_translation* out = echoir_translator_translate(t, &r);
    REQUIRE(out != nullptr);
    CHECK(out->struct_size == sizeof(echoir_translation));
    CHECK(std::string(out->text) == "[vi] Good morning.");
    CHECK(out->capped == 0);
    echoir_translator_free(t);
}

TEST_CASE("fake translator takes the 7B id, and refuses ids that are not translators") {
    echoir_translator* t = create_translator("hy-mt2-7b-q4_k_m");
    REQUIRE(t != nullptr);
    echoir_translator_free(t);

    CHECK(create_translator("whisper-tiny-en") == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
}

TEST_CASE("fake translator checks the request") {
    echoir_translator* t = create_translator();
    REQUIRE(t != nullptr);

    echoir_translate_request r = request("Hello.", "vi");
    CHECK(echoir_translator_translate(t, nullptr) == nullptr);

    r.text = nullptr;
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);

    r = request("   ", "vi");
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);

    r = request("Hello.", nullptr);
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    r = request("Hello.", "");
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    r = request("Hello.", "xx");
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    CHECK(std::string(echoir_last_error()).find("unsupported target language") !=
          std::string::npos);

    r = request("Hello.", "vi");
    r.source = "xx";
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    r.source = "auto";
    CHECK(echoir_translator_translate(t, &r) != nullptr);
    r.source = "";
    CHECK(echoir_translator_translate(t, &r) != nullptr);

    r = request("Hello.", "vi");
    r.struct_size = 4;
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    r.struct_size = sizeof(r) + 1;
    CHECK(echoir_translator_translate(t, &r) == nullptr);
    CHECK(std::string(echoir_last_error()).find("struct_size") != std::string::npos);

    // A struct from before the context field is valid.
    r.struct_size = static_cast<uint32_t>(offsetof(echoir_translate_request, context));
    r.context = "ignored";
    CHECK(echoir_translator_translate(t, &r) != nullptr);

    echoir_translator_free(t);
}

TEST_CASE("fake translator result is valid until the next call") {
    echoir_translator* t = create_translator();
    REQUIRE(t != nullptr);
    echoir_translate_request a = request("one", "ja");
    echoir_translate_request b = request("two", "ja");
    const echoir_translation* first = echoir_translator_translate(t, &a);
    REQUIRE(first != nullptr);
    CHECK(std::string(first->text) == "[ja] one");
    const echoir_translation* second = echoir_translator_translate(t, &b);
    REQUIRE(second != nullptr);
    CHECK(std::string(second->text) == "[ja] two");
    echoir_translator_free(t);
}

TEST_CASE("fake translator runs calls from several threads one at a time") {
    echoir_translator* t = create_translator();
    REQUIRE(t != nullptr);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            for (int n = 0; n < 50; ++n) {
                const std::string text = "line " + std::to_string(i) + " " + std::to_string(n);
                echoir_translate_request r = request(text.c_str(), "fr");
                // The result is shared, so a caller on another thread may see another call's text.
                // The call itself must be safe, and return a result.
                if (echoir_translator_translate(t, &r) == nullptr) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures == 0);
    echoir_translator_free(t);
}
