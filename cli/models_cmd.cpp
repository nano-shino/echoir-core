#include "models_cmd.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "model_dirs.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "options.h"
#include "text/utf.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// Column widths. Backends are at most "qwen3-asr" long.
constexpr int kBackendWidth = 10;
constexpr int kStatusWidth = 8;

// The total size of the entry's files: the on-disk size of each file that is present, else the
// size the catalogue gives. -1 when some file has neither, so the total is unknown.
std::int64_t entry_size(const fs::path& dir, const models::Model& model) {
    std::int64_t total = 0;
    for (const models::File& file : model.files) {
        std::int64_t size = models::size_on_disk(dir / file.name);
        if (size < 0) {
            size = file.size;
        }
        if (size < 0) {
            return -1;
        }
        total += size;
    }
    return total;
}

// A byte count with a binary unit and one decimal, such as 74.1 MB. "-" when unknown.
std::string format_size(std::int64_t bytes) {
    if (bytes < 0) {
        return "-";
    }
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%lld B", static_cast<long long>(bytes));
        return buf;
    }
    constexpr const char* kUnits[] = {"KB", "MB", "GB"};
    double value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(kUnits)) {
        value /= 1024;
        ++unit;
    }
    std::snprintf(buf, sizeof buf, "%.1f %s", value, kUnits[unit]);
    return buf;
}

}  // namespace

void run_models_list(const std::vector<fs::path>& model_dirs) {
    const Catalogue catalogue = load_catalogue(model_dirs);
    int id_width = 2;  // at least the "ID" header
    for (const models::Model& model : catalogue.registry.models()) {
        id_width = std::max(id_width, static_cast<int>(model.id.size()));
    }
    std::printf("%-*s  %-*s  %-*s  %s\n", id_width, "ID", kBackendWidth, "BACKEND", kStatusWidth,
                "STATUS", "SIZE");
    for (const models::Model& model : catalogue.registry.models()) {
        const char* status = models::is_present(catalogue.dir, model) ? "present" : "missing";
        std::printf("%-*s  %-*s  %-*s  %s\n", id_width, model.id.c_str(), kBackendWidth,
                    model.backend.c_str(), kStatusWidth, status,
                    format_size(entry_size(catalogue.dir, model)).c_str());
    }
}

void run_models_path(const std::string& id, const std::vector<fs::path>& model_dirs) {
    const Catalogue catalogue = load_catalogue(model_dirs);
    const models::Model* model = catalogue.registry.find(id);
    if (model == nullptr) {
        throw UsageError(id + " is not a registry id");
    }
    // Every path is checked before any is printed, so a missing file prints nothing.
    std::vector<fs::path> paths;
    for (const models::File& file : model->files) {
        // resolve() throws when the file is missing or has the wrong size, with the command that
        // fetches the model.
        paths.push_back(models::resolve(catalogue.dir, *model, file.role));
    }
    for (const fs::path& path : paths) {
        std::printf("%s\n", text::path_utf8(path).c_str());
    }
}

}  // namespace echoir::cli
