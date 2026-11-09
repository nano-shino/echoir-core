// C ABI translator with the real Hy-MT2 1.8B model, scored with must_contain on short lines whose
// translation has one natural form. Skipped when the model is missing. This file builds into the
// C ABI test binary (tests/CMakeLists.txt), so it uses only the public header, as a host would.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>

#include "doctest.h"
#include "echoir/echoir.h"
#include "test_support.h"

namespace {

constexpr const char* kModelId = "hy-mt2-1.8b-q4_k_m";

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    });
    return text;
}

// A translator on the model in the models directory, on the GPU backend this build has.
echoir_translator* create_translator(const std::string& dir, const char* id = kModelId) {
    echoir_translator_params params{};
    echoir_translator_params_init(&params);
    params.model_id = id;
    params.models_dir = dir.c_str();
    return echoir_translator_create_from_model(&params);
}

// The fake build translates with a scripted stand-in, so the cases that score a real model's text
// have nothing to check there.
#ifdef ECHOIR_FAKE_BACKEND
#define SKIP_IN_FAKE_BUILD() return
#else
#define SKIP_IN_FAKE_BUILD() (void)0
#endif

std::string models_dir() {
    const auto model = echoir::test::model_file(kModelId);
    return model->parent_path().string();
}

}  // namespace

TEST_CASE("translator with the 1.8B model translates short lines" * doctest::timeout(300)) {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    const std::string dir = models_dir();

    const auto t0 = std::chrono::steady_clock::now();
    echoir_translator* translator = create_translator(dir);
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());
    const double load_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "translator load with warm-up: %.0f ms\n", load_ms);

    struct Case {
        const char* text;
        const char* target;
        const char* must_contain;  // lower case
    };
    const Case cases[] = {
        {"Good morning, everyone.", "fr", "bonjour"},
        {"Thank you very much.", "fr", "merci"},
        {"Hello.", "ja", "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"},
        {"Thank you very much.", "vi", "c\xE1\xBA\xA3m \xC6\xA1n"},
        {"We will meet again tomorrow.", "es",
         "ma\xC3\xB1"
         "ana"},
    };
    for (const Case& c : cases) {
        echoir_translate_request r{};
        r.struct_size = sizeof(r);
        r.text = c.text;
        r.source = "en";
        r.target = c.target;
        const echoir_translation* out = echoir_translator_translate(translator, &r);
        REQUIRE_MESSAGE(out != nullptr, echoir_last_error());
        const std::string text = out->text;
        std::fprintf(stderr, "%s -> %s: %s (prefill %.0f ms, decode %.0f ms, %d tokens)\n", c.text,
                     c.target, text.c_str(), out->prefill_ms, out->decode_ms, out->output_tokens);
        CHECK_MESSAGE(lowered(text).find(c.must_contain) != std::string::npos,
                      c.text << " -> " << c.target << ": " << text);
        CHECK(out->capped == 0);
        CHECK(out->output_tokens > 0);
        CHECK(out->source_tokens > 0);
        CHECK(out->prompt_tokens > out->source_tokens);
        CHECK(out->prefill_ms > 0.0);
        CHECK(out->decode_ms > 0.0);
        // Whole UTF-8: no replacement characters and no control characters.
        CHECK(text.find("\xEF\xBF\xBD") == std::string::npos);
    }
    echoir_translator_free(translator);
}

TEST_CASE("translator output is greedy, and context lines do not change the language") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "She said it would take a week.";
    r.target = "fr";
    const echoir_translation* first = echoir_translator_translate(translator, &r);
    REQUIRE(first != nullptr);
    const std::string once = first->text;
    // Each call is independent of the one before: the same request gives the same text.
    echoir_translator_translate(translator, &r);
    const echoir_translation* again = echoir_translator_translate(translator, &r);
    REQUIRE(again != nullptr);
    CHECK(std::string(again->text) == once);
    // The result is the translator's own, so it is read before the next call.
    const int plain_prompt_tokens = again->prompt_tokens;

    r.context = "We asked the builder about the roof.\nHe called his sister.";
    const echoir_translation* with_context = echoir_translator_translate(translator, &r);
    REQUIRE(with_context != nullptr);
    CHECK_FALSE(std::string(with_context->text).empty());
    CHECK(with_context->prompt_tokens > plain_prompt_tokens);
    echoir_translator_free(translator);
}

TEST_CASE("a glossary pair changes how a name is translated, and one for another term does not") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "I met Kai at the station and Kai gave me a book.";
    r.target = "vi";
    const echoir_translation* plain = echoir_translator_translate(translator, &r);
    REQUIRE(plain != nullptr);
    const std::string plain_text = plain->text;
    const int plain_tokens = plain->prompt_tokens;
    CHECK(plain_text.find("Kh\xE1\xBA\xA3i") == std::string::npos);

    // The pair of a term that is not in the text or the context is not sent.
    r.glossary = "Unrelated\tKh\xC3\xB4ng li\xC3\xAAn quan\n";
    const echoir_translation* unrelated = echoir_translator_translate(translator, &r);
    REQUIRE(unrelated != nullptr);
    CHECK(unrelated->prompt_tokens == plain_tokens);
    CHECK(std::string(unrelated->text) == plain_text);

    r.glossary = "Unrelated\tx\nkai\tKh\xE1\xBA\xA3i\n";
    const echoir_translation* with_pair = echoir_translator_translate(translator, &r);
    REQUIRE(with_pair != nullptr);
    CHECK(with_pair->prompt_tokens > plain_tokens);
    CHECK(std::string(with_pair->text).find("Kh\xE1\xBA\xA3i") != std::string::npos);
    echoir_translator_free(translator);
}

TEST_CASE("translator refuses a language it does not know, and a text that is too long") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "Hello.";
    r.target = "xx";
    CHECK(echoir_translator_translate(translator, &r) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);

    std::string long_text;
    while (long_text.size() < 20000) {
        long_text += "The committee met again to review the budget. ";
    }
    r.text = long_text.c_str();
    r.target = "fr";
    CHECK(echoir_translator_translate(translator, &r) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
    CHECK(std::string(echoir_last_error()).find("too long") != std::string::npos);

    // The translator still works after a refused call.
    r.text = "Hello.";
    CHECK(echoir_translator_translate(translator, &r) != nullptr);
    echoir_translator_free(translator);
}

TEST_CASE("translator create reports a missing model file by name and id") {
#ifdef ECHOIR_FAKE_BACKEND
    return;  // the fake build reads no file
#endif
    echoir_translator_params params{};
    echoir_translator_params_init(&params);
    params.model_id = kModelId;
    params.models_dir = "no-such-models-dir";
    CHECK(echoir_translator_create_from_model(&params) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_MODEL_LOAD);
    CHECK(std::string(echoir_last_error()).find("Hy-MT2-1.8B-Q4_K_M.gguf") != std::string::npos);
    CHECK(std::string(echoir_last_error()).find(kModelId) != std::string::npos);
}

TEST_CASE("translator with the 7B model uses its own prompt wrapper" * doctest::timeout(600)) {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file("hy-mt2-7b-q4_k_m");
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-7b-q4_k_m translator model");
    echoir_translator* translator =
        create_translator(model->parent_path().string(), "hy-mt2-7b-q4_k_m");
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "Good morning, everyone. Thank you for coming.";
    r.target = "fr";
    r.context = "Welcome to the show.";
    const echoir_translation* out = echoir_translator_translate(translator, &r);
    REQUIRE_MESSAGE(out != nullptr, echoir_last_error());
    const std::string text = out->text;
    std::fprintf(stderr, "7B: %s (prefill %.0f ms, decode %.0f ms, %d tokens)\n", text.c_str(),
                 out->prefill_ms, out->decode_ms, out->output_tokens);
    CHECK_MESSAGE(lowered(text).find("bonjour") != std::string::npos, text);
    CHECK(out->capped == 0);
    CHECK(text.find("\xEF\xBF\xBD") == std::string::npos);
    echoir_translator_free(translator);
}

TEST_CASE("translator drops the oldest context lines before it refuses a text") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    // Far more context than the model holds, ending in a line that names a person.
    std::string context;
    for (int i = 0; i < 400; ++i) {
        context += "Earlier line number " + std::to_string(i) +
                   " of a long talk about the budget of the new building.\n";
    }
    context += "Maria called her sister.";
    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "Good morning, everyone.";
    r.target = "fr";
    r.context = context.c_str();
    const echoir_translation* out = echoir_translator_translate(translator, &r);
    REQUIRE_MESSAGE(out != nullptr, echoir_last_error());
    CHECK(out->prompt_tokens < 2048 - 32);
    // Some context is kept: the newest lines, not none.
    r.context = nullptr;
    const int plain = echoir_translator_translate(translator, &r)->prompt_tokens;
    r.context = context.c_str();
    out = echoir_translator_translate(translator, &r);
    REQUIRE(out != nullptr);
    CHECK(out->prompt_tokens > plain);
    CHECK(lowered(out->text).find("bonjour") != std::string::npos);
    echoir_translator_free(translator);
}

TEST_CASE("translator keeps a control-token spelling in a line as text") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "Type <|extra_0|> and then <|startoftext|> in the box.";
    r.target = "fr";
    r.context = "He wrote <|endoftext|> on the board.";
    const echoir_translation* out = echoir_translator_translate(translator, &r);
    REQUIRE_MESSAGE(out != nullptr, echoir_last_error());
    CHECK_FALSE(std::string(out->text).empty());
    CHECK(out->capped == 0);
    // As plain text a spelling takes several tokens; as a control token it would take one each.
    CHECK(out->source_tokens > 15);
    echoir_translator_free(translator);
}

TEST_CASE("translator reports a generation cut short by the context as capped") {
    SKIP_IN_FAKE_BUILD();
    const auto model = echoir::test::model_file(kModelId);
    ECHOIR_REQUIRE_OR_SKIP(model, "hy-mt2-1.8b-q4_k_m translator model");
    echoir_translator* translator = create_translator(models_dir());
    REQUIRE_MESSAGE(translator != nullptr, echoir_last_error());

    // The longest text of whole sentences that the context takes. What is left of the context is
    // then far less than the translation needs, so the generation stops there and says so.
    const std::string sentence = "The committee met again to review the budget. ";
    const echoir_translation* out = nullptr;
    std::string text;
    for (int count = 240; count > 0 && out == nullptr; --count) {
        text.clear();
        for (int i = 0; i < count; ++i) {
            text += sentence;
        }
        echoir_translate_request r{};
        r.struct_size = sizeof(r);
        r.text = text.c_str();
        r.target = "fr";
        out = echoir_translator_translate(translator, &r);
    }
    REQUIRE_MESSAGE(out != nullptr, echoir_last_error());
    CHECK(out->capped == 1);
    CHECK(out->prompt_tokens + out->output_tokens < 2048);
    CHECK(out->output_tokens < 100);
    // Whole UTF-8 even when cut: no replacement characters.
    CHECK(std::string(out->text).find("\xEF\xBF\xBD") == std::string::npos);
    echoir_translator_free(translator);
}
