#include "models/registry.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"

namespace echoir::models {

Registry Registry::parse(std::string_view json_text) {
    try {
        const nlohmann::json doc = nlohmann::json::parse(std::string(json_text));
        Registry registry;
        for (const nlohmann::json& item : doc.at("models")) {
            Model model;
            model.id = item.at("id").get<std::string>();
            model.backend = item.at("backend").get<std::string>();
            for (const nlohmann::json& file : item.at("files")) {
                model.files.push_back({file.at("role").get<std::string>(),
                                       file.at("name").get<std::string>(),
                                       file.value("size", std::int64_t{-1})});
            }
            model.min_vram_mb = item.value("min_vram_mb", std::int64_t{-1});
            model.entry = item;
            registry.models_.push_back(std::move(model));
        }
        registry.default_vad_ = doc.at("default_vad").get<std::map<std::string, std::string>>();
        return registry;
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(std::string("not a model registry: ") + e.what());
    }
}

Registry Registry::builtin() { return parse(builtin_registry_json()); }

const Model* Registry::find(const std::string& id) const {
    for (const Model& model : models_) {
        if (model.id == id) {
            return &model;
        }
    }
    return nullptr;
}

std::string Registry::default_vad(const std::string& key) const {
    const auto found = default_vad_.find(key);
    if (found == default_vad_.end()) {
        throw std::runtime_error("the registry has no default VAD for the " + key + " backend");
    }
    return found->second;
}

std::string backend_key(const std::string& registry_backend) {
    if (registry_backend == "whisper") {
        return "whisper";
    }
    if (registry_backend == "qwen3-asr") {
        return "qwen";
    }
    return {};
}

}  // namespace echoir::models
