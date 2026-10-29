// The JSON that echoir_models_json returns: the devices, the default VADs and each catalogue entry
// with its presence and fit.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "models/devices.h"
#include "models/registry.h"

namespace echoir::models {

// The catalogue as JSON text:
//   {"devices":[{"name","type":"gpu"|"cpu","memory_total_mb","memory_free_mb"}],
//    "default_vad":{"whisper":id,"qwen":id},
//    "models":[{<entry>,"files":[{<file>,"present":bool}],"present":bool,"fits":bool}]}
// A file is present when it is in dir and has the size the catalogue gives. A model fits when its
// min_vram_mb is at most the free memory of the best device: the GPU with the most free memory, or
// the CPU when there is no GPU. An entry without min_vram_mb fits. An empty dir has no files.
std::string catalogue_json(const Registry& registry, const std::filesystem::path& dir,
                           const std::vector<Device>& devices);

}  // namespace echoir::models
