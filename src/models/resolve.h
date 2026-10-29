// Finds the files of catalogue entries in a models directory.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "models/registry.h"

namespace echoir::models {

// The size in bytes of the regular file at path, or -1 when there is none or it cannot be read.
std::int64_t size_on_disk(const std::filesystem::path& path);

// True when the file is in dir and, when the catalogue gives its size, has that size. An empty dir
// has no files.
bool is_present(const std::filesystem::path& dir, const File& file);

// True when every file of the entry is present.
bool is_present(const std::filesystem::path& dir, const Model& model);

// The path of the entry's file with this role in dir. Throws ModelLoadError when the file is
// missing or has another size than the catalogue gives. The message names the file, the id and the
// command that fetches the model. Throws std::invalid_argument when the entry has no file with this
// role.
std::filesystem::path resolve(const std::filesystem::path& dir, const Model& model,
                              const std::string& role);

}  // namespace echoir::models
