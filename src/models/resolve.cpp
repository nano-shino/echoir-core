#include "models/resolve.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

#include "errors.h"
#include "models/registry.h"
#include "text/utf.h"

namespace echoir::models {

namespace fs = std::filesystem;

std::int64_t size_on_disk(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return -1;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    return ec ? -1 : static_cast<std::int64_t>(size);
}

bool is_present(const fs::path& dir, const File& file) {
    if (dir.empty()) {
        return false;
    }
    const fs::path path = dir / file.name;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    return file.size < 0 || size_on_disk(path) == file.size;
}

bool is_present(const fs::path& dir, const Model& model) {
    return std::all_of(model.files.begin(), model.files.end(),
                       [&](const File& file) { return is_present(dir, file); });
}

fs::path resolve(const fs::path& dir, const Model& model, const std::string& role) {
    const auto found = std::find_if(model.files.begin(), model.files.end(),
                                    [&](const File& file) { return file.role == role; });
    if (found == model.files.end()) {
        throw std::invalid_argument("model " + model.id + " has no " + role + " file");
    }
    const fs::path path = dir / found->name;
#ifdef ECHOIR_FAKE_BACKEND
    // The fake build reads no model file, so the path is returned without a check.
    return path;
#endif
    const std::string where = text::path_utf8(path);
    const std::string fetch = "; fetch it with tools/fetch-models.sh " + model.id +
                              " (tools/fetch-models.ps1 " + model.id + " on Windows)";

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        throw ModelLoadError("model " + model.id + ": " + found->name + " not found in " +
                             text::path_utf8(dir) + fetch);
    }
    if (found->size >= 0) {
        const std::int64_t actual = size_on_disk(path);
        if (actual != found->size) {
            const std::string got =
                actual < 0 ? "cannot be read" : std::to_string(actual) + " bytes";
            throw ModelLoadError("model " + model.id + ": " + where + " is " + got +
                                 ", the registry says " + std::to_string(found->size) + fetch);
        }
    }
    return path;
}

}  // namespace echoir::models
