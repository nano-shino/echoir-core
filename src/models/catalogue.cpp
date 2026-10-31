#include "models/catalogue.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "models/devices.h"
#include "models/registry.h"
#include "models/resolve.h"

namespace echoir::models {
namespace {

using json = nlohmann::json;

// The free memory of the device that a model is loaded on: the GPU with the most free memory,
// else the CPU with the most. Zero when there are no devices.
std::int64_t best_free_mb(const std::vector<Device>& devices) {
    const Device* best = nullptr;
    for (const Device& device : devices) {
        if (best == nullptr ||
            std::make_pair(device.gpu, device.free_mb) > std::make_pair(best->gpu, best->free_mb)) {
            best = &device;
        }
    }
    return best == nullptr ? 0 : best->free_mb;
}

bool fits(const Model& model, std::int64_t free_mb) {
    return model.min_vram_mb < 0 || model.min_vram_mb <= free_mb;
}

}  // namespace

std::string catalogue_json(const Registry& registry, const std::filesystem::path& dir,
                           const std::vector<Device>& devices) {
    json device_list = json::array();
    for (const Device& device : devices) {
        device_list.push_back({{"name", device.name},
                               {"type", device.gpu ? "gpu" : "cpu"},
                               {"description", device.description},
                               {"integrated", device.integrated},
                               {"memory_total_mb", device.total_mb},
                               {"memory_free_mb", device.free_mb}});
    }

    const std::int64_t free_mb = best_free_mb(devices);
    json model_list = json::array();
    for (const Model& model : registry.models()) {
        json item = model.entry;
        bool all_present = true;
        for (std::size_t i = 0; i < model.files.size(); ++i) {
            const bool present = is_present(dir, model.files[i]);
            item["files"][i]["present"] = present;
            all_present = all_present && present;
        }
        item["present"] = all_present;
        item["fits"] = fits(model, free_mb);
        model_list.push_back(std::move(item));
    }

    json out;
    out["devices"] = std::move(device_list);
    out["default_vad"] = registry.default_vads();
    out["models"] = std::move(model_list);
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace echoir::models
