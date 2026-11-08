// The translator without a model: language names, the prompt layouts, output cleaning, the
// generation limit, the translator entries of the catalogue, and the C ABI's argument checks.
#include <cstddef>
#include <cstdint>
#include <string>

#include "doctest.h"
#include "echoir/echoir.h"
#include "json.hpp"
#include "models/registry.h"
#include "translate/language.h"
#include "translate/output.h"
#include "translate/prompt.h"
#include "translate/translator.h"

namespace {

bool last_error_contains(const std::string& text) {
    return std::string(echoir_last_error()).find(text) != std::string::npos;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("language names: codes match ignoring case, and unknown codes have none") {
    using echoir::translate::language_name;
    CHECK(language_name("vi") == "Vietnamese");
    CHECK(language_name("EN") == "English");
    CHECK(language_name("zh") == "Chinese");
    CHECK(language_name("zh-Hant") == "Traditional Chinese");
    CHECK(language_name("zh_hant") == "Traditional Chinese");
    CHECK(language_name("yue") == "Cantonese");
    CHECK(language_name("").empty());
    CHECK(language_name("auto").empty());
    CHECK(language_name("xx").empty());
    CHECK(language_name("English").empty());
}

TEST_CASE("prompt styles are named by the registry") {
    using echoir::translate::parse_prompt_style;
    using echoir::translate::PromptStyle;
    CHECK(parse_prompt_style("hy-mt2-1.8b") == PromptStyle::hy_mt2_1_8b);
    CHECK(parse_prompt_style("hy-mt2-7b") == PromptStyle::hy_mt2_7b);
    CHECK_FALSE(parse_prompt_style("").has_value());
    CHECK_FALSE(parse_prompt_style("chatml").has_value());
}

TEST_CASE("the 1.8B prompt wraps the instruction in the fullwidth-bar markers") {
    using namespace echoir::translate;
    const Prompt p = render_prompt(PromptStyle::hy_mt2_1_8b, "Vietnamese", "  Hello there. \n", "");
    CHECK(p.prefix ==
          "<\xEF\xBD\x9Chy_begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>"
          "<\xEF\xBD\x9Chy_User\xEF\xBD\x9C>");
    CHECK(p.suffix == "<\xEF\xBD\x9Chy_Assistant\xEF\xBD\x9C>");
    CHECK(p.instruction ==
          "Translate the following text into Vietnamese. Note that you should only output the "
          "translated result without any additional explanation:\n\nHello there.");
}

TEST_CASE("the 7B prompt uses the other wrapper") {
    using namespace echoir::translate;
    const Prompt p = render_prompt(PromptStyle::hy_mt2_7b, "Japanese", "Hello.", "");
    CHECK(p.prefix == "<|startoftext|>");
    CHECK(p.suffix == "<|extra_0|>");
    CHECK(contains(p.instruction, "into Japanese."));
}

TEST_CASE("context goes in the background-information form, source lines only") {
    using namespace echoir::translate;
    const Prompt p = render_prompt(PromptStyle::hy_mt2_1_8b, "English", "Second line.",
                                   "First line.\nBefore that.\n");
    CHECK(p.instruction ==
          "[Background Information]\nFirst line.\nBefore that.\n\nPlease translate the following "
          "text into English, taking the provided background information into consideration. "
          "Only output the translation of the Source Text.\n\n[Source Text]\nSecond line.");
    // A blank context is no context.
    const Prompt blank = render_prompt(PromptStyle::hy_mt2_1_8b, "English", "Second line.", " \n ");
    CHECK_FALSE(contains(blank.instruction, "[Background Information]"));
}

TEST_CASE("a line that spells a control token stays in the instruction") {
    using namespace echoir::translate;
    const Prompt p = render_prompt(PromptStyle::hy_mt2_7b, "French", "a <|extra_0|> b", "");
    CHECK(contains(p.instruction, "a <|extra_0|> b"));
}

TEST_CASE("clean output keeps whole UTF-8 and drops the rest") {
    using echoir::translate::clean_output;
    CHECK(clean_output("  Xin ch\xC3\xA0o \n") == "Xin ch\xC3\xA0o");
    CHECK(clean_output("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC");
    // A character cut in half by the generation limit.
    CHECK(clean_output("ok \xE6\x97") == "ok");
    CHECK(clean_output("ok \xF0\x9F\x98") == "ok");
    // A stray continuation byte, an overlong form, a surrogate and a value above U+10FFFF.
    CHECK(clean_output("a\x80"
                       "b") == "ab");
    CHECK(clean_output("a\xC0\xAF"
                       "b") == "ab");
    CHECK(clean_output("a\xED\xA0\x80"
                       "b") == "ab");
    CHECK(clean_output("a\xF4\x90\x80\x80"
                       "b") == "ab");
    CHECK(clean_output(" \n ").empty());
    CHECK(clean_output("").empty());
}

TEST_CASE("the generation limit is five tokens per source token plus 32") {
    using echoir::translate::generation_cap;
    CHECK(generation_cap(0) == 32);
    CHECK(generation_cap(10) == 82);
    CHECK(generation_cap(100) == 532);
}

TEST_CASE("the built-in catalogue lists the translators with exact sizes") {
    const echoir::models::Registry registry = echoir::models::Registry::builtin();
    CHECK(registry.default_translator() == "hy-mt2-1.8b-q4_k_m");

    const echoir::models::Model* small = registry.find("hy-mt2-1.8b-q4_k_m");
    REQUIRE(small != nullptr);
    CHECK(small->backend == "translator");
    CHECK(small->entry.at("prompt_style") == "hy-mt2-1.8b");
    REQUIRE(small->files.size() == 1);
    CHECK(small->files[0].role == "model");
    CHECK(small->files[0].name == "Hy-MT2-1.8B-Q4_K_M.gguf");
    CHECK(small->files[0].size == 1133080448);
    CHECK(small->min_vram_mb > 0);

    const echoir::models::Model* large = registry.find("hy-mt2-7b-q4_k_m");
    REQUIRE(large != nullptr);
    CHECK(large->backend == "translator");
    CHECK(large->entry.at("prompt_style") == "hy-mt2-7b");
    REQUIRE(large->files.size() == 1);
    CHECK(large->files[0].size == 4624648896);
    CHECK(large->min_vram_mb > small->min_vram_mb);

    // A translator is not a speech model.
    CHECK(echoir::models::backend_key("translator").empty());
}

TEST_CASE("the catalogue json lists the translators and the default one") {
    const char* text = echoir_models_json(nullptr, nullptr);
    REQUIRE(text != nullptr);
    const nlohmann::json doc = nlohmann::json::parse(text);
    CHECK(doc.at("default_translator") == "hy-mt2-1.8b-q4_k_m");
    int translators = 0;
    for (const nlohmann::json& model : doc.at("models")) {
        if (model.at("backend") == "translator") {
            ++translators;
            CHECK(model.at("present") == false);
        }
    }
    CHECK(translators == 2);
}

TEST_CASE("translator params init sets struct_size and defaults") {
    echoir_translator_params p{};
    p.models_dir = "stale";
    p.n_threads = 9;
    echoir_translator_params_init(&p);
    CHECK(p.struct_size == sizeof(echoir_translator_params));
    CHECK(p.registry_json == nullptr);
    CHECK(p.models_dir == nullptr);
    CHECK(p.model_id == nullptr);
    CHECK(p.use_gpu == 1);
    CHECK(p.n_threads == 0);

    echoir_translator_params_init(nullptr);
    CHECK(last_error_contains("echoir_translator_params_init"));
}

#ifndef ECHOIR_TEST_NO_ENGINE
TEST_CASE("translator create refuses bad params") {
    CHECK(echoir_translator_create_from_model(nullptr) == nullptr);
    CHECK(last_error_contains("echoir_translator_create_from_model: params must not be NULL"));
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);

    echoir_translator_params p{};
    echoir_translator_params_init(&p);
    p.models_dir = "no-such-dir";

    p.struct_size = static_cast<uint32_t>(offsetof(echoir_translator_params, model_id));
    CHECK(echoir_translator_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("struct_size"));

    echoir_translator_params_init(&p);
    p.models_dir = "no-such-dir";
    p.struct_size = static_cast<uint32_t>(sizeof(p) + 1);
    CHECK(echoir_translator_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("struct_size"));
}

TEST_CASE("translator create refuses an unknown id and a model that is not a translator") {
    echoir_translator_params p{};
    echoir_translator_params_init(&p);
    p.models_dir = "no-such-dir";

    p.model_id = "no-such-model";
    CHECK(echoir_translator_create_from_model(&p) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
    CHECK(last_error_contains("no-such-model"));

    p.model_id = "whisper-tiny-en";
    CHECK(echoir_translator_create_from_model(&p) == nullptr);
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
    CHECK(last_error_contains("is not a translator"));

    p.model_id = "silero-v6.2.0";
    CHECK(echoir_translator_create_from_model(&p) == nullptr);
    CHECK(last_error_contains("is not a translator"));
}

#endif

TEST_CASE("engine create from model refuses a translator id") {
    echoir_model_params p{};
    echoir_model_params_init(&p);
    p.models_dir = "no-such-dir";
    p.model_id = "hy-mt2-1.8b-q4_k_m";
    CHECK(echoir_engine_create_from_model(&p) == nullptr);
#ifndef ECHOIR_TEST_NO_ENGINE
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
    CHECK(last_error_contains("is not a speech model"));
#endif
}

#ifndef ECHOIR_TEST_NO_ENGINE
TEST_CASE("translate refuses a NULL translator and bad requests") {
    echoir_translate_request r{};
    r.struct_size = sizeof(r);
    r.text = "Hello.";
    r.target = "vi";
    CHECK(echoir_translator_translate(nullptr, &r) == nullptr);
    CHECK(last_error_contains("echoir_translator_translate: translator must not be NULL"));
    CHECK(echoir_last_status() == ECHOIR_ERR_INVALID_ARG);
    CHECK(echoir_translator_translate(nullptr, nullptr) == nullptr);
}
#endif

TEST_CASE("freeing a NULL translator is a no-op") { echoir_translator_free(nullptr); }
