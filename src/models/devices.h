// The compute devices of this process and their memory, as ggml reports them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace echoir::models {

struct Device {
    std::string name;
    bool gpu = false;  // a GPU or an integrated GPU; false for CPU and accelerator devices
    std::int64_t total_mb = 0;
    std::int64_t free_mb = 0;
    std::string description;  // ggml's human-readable name, e.g. the GPU model; may be empty
    bool integrated = false;  // an integrated GPU, which shares system memory
};

// The devices ggml has in this build, in ggml's order. Needs the ggml backends, so it is defined
// only in builds with an engine. Other builds have no devices to report.
std::vector<Device> query_devices();

}  // namespace echoir::models
