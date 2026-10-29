// `echoir models`: lists the models in the registry, and prints where their files are.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace echoir::cli {

// Prints one line per catalogue entry: its id, its backend, whether every file of it is present
// (on disk with the size the catalogue gives), and its size. Throws std::runtime_error when the
// catalogue cannot be read.
void run_models_list(const std::vector<std::filesystem::path>& model_dirs);

// Prints the absolute path of each file of the entry, in registry order. Throws UsageError when
// the id is not in the registry, and std::runtime_error when a file is missing. The message then
// names the command that fetches the model, and no path is printed.
void run_models_path(const std::string& id, const std::vector<std::filesystem::path>& model_dirs);

}  // namespace echoir::cli
