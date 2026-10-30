// Model and fixture lookup for integration tests. Models are named by registry id, never by file
// name. A missing model or fixture makes the test print SKIP and return, so the suite passes on
// a machine without models. With ECHOIR_REQUIRE_MODELS=1 in the environment, a missing model fails
// the test instead, so a run that must cover the models cannot pass by skipping them.
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "doctest.h"
#include "json.hpp"
#include "text/env.h"
#include "text/utf.h"

namespace echoir::test {

inline std::filesystem::path source_dir() { return ECHOIR_SOURCE_DIR; }

// models/<file name> for the file with `role` ("model", "mmproj", "vad") of registry entry `id`,
// if the registry lists it and the file is on disk. $ECHOIR_MODELS overrides the models dir.
// Throws std::logic_error when the registry has no such id and role, so a typo fails the test
// instead of skipping it.
inline std::optional<std::filesystem::path> model_file(const std::string& id,
                                                       const std::string& role = "model") {
    std::filesystem::path dir = source_dir() / "models";
    if (const auto env = echoir::text::get_env("ECHOIR_MODELS"); env && !env->empty()) {
        dir = echoir::text::path_from_utf8(*env);
    }
    std::ifstream in(source_dir() / "models" / "registry.json");
    if (!in) throw std::logic_error("models/registry.json is missing");
    const auto registry = nlohmann::json::parse(in);
    for (const auto& model : registry.at("models")) {
        if (model.at("id") != id) continue;
        for (const auto& file : model.at("files")) {
            if (file.at("role") != role) continue;
            auto path = dir / file.at("name").get<std::string>();
            if (std::filesystem::exists(path)) return path;
            return std::nullopt;
        }
    }
    throw std::logic_error("no registry entry with id \"" + id + "\" and role \"" + role + "\"");
}

// tests/fixtures/<name>.wav, then build/fixtures/<name>.wav (tools/make-fixtures.py).
inline std::optional<std::filesystem::path> fixture(const std::string& name) {
    for (const char* sub : {"tests/fixtures", "build/fixtures"}) {
        auto path = source_dir() / sub / (name + ".wav");
        if (std::filesystem::exists(path)) return path;
    }
    return std::nullopt;
}

// True when ECHOIR_REQUIRE_MODELS is set to 1: missing models then fail the test.
inline bool models_required() {
    const auto env = echoir::text::get_env("ECHOIR_REQUIRE_MODELS");
    return env && *env == "1";
}

}  // namespace echoir::test

// Returns from the current test case with a SKIP message when `opt` is empty, or fails it when
// models are required (see models_required).
#define ECHOIR_REQUIRE_OR_SKIP(opt, what)                                          \
    do {                                                                           \
        if (!(opt)) {                                                              \
            if (::echoir::test::models_required()) {                               \
                FAIL("missing " << (what) << " (ECHOIR_REQUIRE_MODELS is set)");   \
            }                                                                      \
            MESSAGE("SKIP: missing " << (what));                                   \
            std::fprintf(stderr, "SKIP: missing %s\n", std::string(what).c_str()); \
            return;                                                                \
        }                                                                          \
    } while (0)
