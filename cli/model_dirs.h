// Where the CLI looks for the model catalogue and the model files.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "models/registry.h"

namespace echoir::cli {

// The catalogue the CLI uses, and the directory of its model files.
struct Catalogue {
    models::Registry registry;
    std::filesystem::path dir;
};

// The directories searched for models, in order: models_dir (from --models-dir, empty when not
// given), $ECHOIR_MODELS, ./models, then the models directory beside the executable, and the
// repository's models/ when the executable runs from a build tree (build/<preset>/bin). argv0 is
// used when the platform does not report the executable's path.
std::vector<std::filesystem::path> model_search_dirs(const std::string& models_dir,
                                                     const char* argv0);

// The catalogue from the first search directory with a registry.json, whose directory is the models
// directory. Without one, the built-in catalogue, with the models directory of the first search
// directory that exists. Throws std::runtime_error when a registry.json does not parse.
Catalogue load_catalogue(const std::vector<std::filesystem::path>& search_dirs);

}  // namespace echoir::cli
