#include "models.h"

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backends/backend_log.h"
#include "echoir/echoir.h"
#include "model_dirs.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "options.h"
#include "text/utf.h"
#include "vad/vad_overrides.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// Threads for the VAD: the default of make_vad.
constexpr int kVadThreads = 2;

}  // namespace

LoadedModels load_models(const TranscribeOptions& o, const std::vector<fs::path>& model_dirs,
                         vad::VadUse use) {
    // Loaded on first use, so models given as files need no catalogue.
    std::optional<Catalogue> catalogue;
    const auto lookup = [&]() -> const Catalogue& {
        if (!catalogue) {
            catalogue = load_catalogue(model_dirs);
        }
        return *catalogue;
    };
    const auto find_model = [&](const std::string& id) -> const models::Model& {
        const models::Model* model = lookup().registry.find(id);
        if (model == nullptr) {
            throw UsageError(id + " is neither a file nor a registry id");
        }
        return *model;
    };
    // An existing file is used as given. Anything else is a registry id, and the id's file for
    // the role is used.
    const auto resolve = [&](const std::string& arg, const char* role) -> fs::path {
        const fs::path path = text::path_from_utf8(arg);
        if (fs::is_regular_file(path)) {
            return path;
        }
        return models::resolve(lookup().dir, find_model(arg), role);
    };

    // --model: a file needs --backend, because it names no backend. An id names one, and the
    // two must agree when both are given.
    std::string backend = o.backend;
    const models::Model* entry = nullptr;
    fs::path model_file;
    const fs::path model_arg = text::path_from_utf8(o.model);
    if (fs::is_regular_file(model_arg)) {
        if (backend.empty()) {
            throw UsageError("--backend is required when --model is a file");
        }
        model_file = model_arg;
    } else {
        entry = &find_model(o.model);
        const std::string from_registry = models::backend_key(entry->backend);
        if (from_registry.empty()) {
            throw UsageError("model " + o.model + " is not a speech model");
        }
        if (backend.empty()) {
            backend = from_registry;
        } else if (backend != from_registry) {
            throw UsageError("model " + o.model + " is a " + from_registry + " model, not " +
                             backend);
        }
        model_file = models::resolve(lookup().dir, *entry, "model");
    }

    // Qwen needs a projector. A model id brings its own unless --mmproj says otherwise.
    fs::path mmproj_file;
    if (backend == "qwen") {
        if (!o.mmproj.empty()) {
            mmproj_file = resolve(o.mmproj, "mmproj");
        } else if (entry != nullptr) {
            mmproj_file = models::resolve(lookup().dir, *entry, "mmproj");
        } else {
            throw UsageError("--mmproj is required for qwen when --model is a file");
        }
    } else if (!o.mmproj.empty()) {
        throw UsageError("--mmproj applies only to the qwen backend");
    }
    // The default VAD depends on the backend, so it is looked up once the backend is known.
    const std::string vad_id =
        o.run.vad.empty() ? lookup().registry.default_vad(backend) : o.run.vad;
    const fs::path vad_file = resolve(vad_id, "vad");

    // The core copies the paths during make_backend, so these strings only need to live until then.
    const std::string model_path = text::path_utf8(model_file);
    const std::string mmproj_path = text::path_utf8(mmproj_file);
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = backend == "qwen" ? ECHOIR_BACKEND_QWEN3_ASR : ECHOIR_BACKEND_WHISPER;
    params.model_path = model_path.c_str();
    params.mmproj_path = mmproj_path.empty() ? nullptr : mmproj_path.c_str();
    params.use_gpu = o.run.use_gpu ? 1 : 0;
    params.n_threads = o.run.threads;

    vad::VadOverrides vad_overrides;
    for (const std::string& assignment : o.vad_params) {
        vad_overrides.add(assignment);
    }
    // The VAD loads before the backend, and Silero logs through whisper.cpp, so the log routing is
    // set up first.
    backends::configure_backend_logs();
    LoadedModels loaded;
    loaded.backend = backend;
    loaded.vad = vad::make_vad(vad_file, kVadThreads, vad_overrides, use);
    loaded.asr = make_backend(params);
    return loaded;
}

void check_request(IAsrBackend& asr, const TranscribeOptions& o) {
    const std::vector<std::string> no_history;
    asr.decode(DecodeRequest{std::span<const float>(), no_history, o.run.language, o.context});
}

}  // namespace echoir::cli
