#include "models/devices.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ggml-backend.h"

namespace echoir::models {
namespace {

constexpr std::int64_t kMiB = 1024 * 1024;

// Some backends pass a fixed-width hardware string through, such as the CPU brand padded with spaces.
std::string trimmed(const char* text) {
    const std::string value = text == nullptr ? std::string() : std::string(text);
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

}  // namespace

std::vector<Device> query_devices() {
    std::vector<Device> devices;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        std::size_t free_bytes = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);

        Device device;
        device.name = ggml_backend_dev_name(dev);
        device.description = trimmed(ggml_backend_dev_description(dev));
        device.gpu = type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        device.integrated = type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        device.total_mb = static_cast<std::int64_t>(total_bytes) / kMiB;
        device.free_mb = static_cast<std::int64_t>(free_bytes) / kMiB;
        devices.push_back(std::move(device));
    }
    return devices;
}

}  // namespace echoir::models
