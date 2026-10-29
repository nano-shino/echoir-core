// The model catalogue: the entries of models/registry.json. The library builds the file in, and a
// host can pass its own catalogue as JSON text.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace echoir::models {

// One file of an entry.
struct File {
    std::string role;        // "model", "mmproj" or "vad"
    std::string name;        // file name, in the models directory
    std::int64_t size = -1;  // expected size in bytes; -1 when the catalogue gives none
};

// One entry of the catalogue.
struct Model {
    std::string id;       // for example "whisper-tiny-en"
    std::string backend;  // "whisper", "qwen3-asr" or "vad"
    std::vector<File> files;
    std::int64_t min_vram_mb = -1;  // estimated memory the model needs; -1 if the entry gives none
    nlohmann::json entry;           // the entry as written, with every field it has
};

class Registry {
public:
    // Parses catalogue text. Throws std::invalid_argument when it is not a catalogue.
    static Registry parse(std::string_view json_text);

    // The catalogue built into the library.
    static Registry builtin();

    // All entries, in catalogue order.
    const std::vector<Model>& models() const { return models_; }

    // The entry with this id, or nullptr. The pointer lives as long as the registry.
    const Model* find(const std::string& id) const;

    // The default VAD id of a backend key ("whisper" or "qwen"). Throws std::runtime_error when
    // the catalogue names none for it.
    std::string default_vad(const std::string& key) const;

    // The default VAD of every backend key, for the catalogue's JSON.
    const std::map<std::string, std::string>& default_vads() const { return default_vad_; }

private:
    std::vector<Model> models_;
    std::map<std::string, std::string> default_vad_;  // backend key to VAD id
};

// The catalogue text built into the library. It is generated from models/registry.json at build
// time.
std::string_view builtin_registry_json();

// The key a speech model's backend has in default_vad: "whisper" or "qwen". Empty for entries
// that are not speech models, such as VADs.
std::string backend_key(const std::string& registry_backend);

}  // namespace echoir::models
