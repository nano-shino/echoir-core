#include "bench.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "input.h"
#include "model_dirs.h"
#include "offline.h"
#include "options.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "score.h"
#include "text/score.h"
#include "text/utf.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// The default model of each backend. Both are entries of models/registry.json.
constexpr const char* kWhisperModel = "whisper-tiny-en";
constexpr const char* kQwenModel = "qwen3-asr-0.6b-q8_0";

// One table row: a finished run.
struct Row {
    std::string backend;
    std::string model;
    text::ErrorRate score;
    OfflineStats stats;
};

// The models to run. A model named with --model runs alone. Otherwise each default model runs
// when its files and the default VAD of its backend are on disk.
std::vector<std::string> models_to_run(const BenchOptions& o,
                                       const std::vector<fs::path>& model_dirs) {
    if (!o.model.empty()) {
        return {o.model};
    }
    // Each candidate is a registry id and its backend.
    std::vector<std::pair<std::string, std::string>> candidates;
    if (o.backend == "all" || o.backend == "whisper") {
        candidates.emplace_back(kWhisperModel, "whisper");
    }
    if (o.backend == "all" || o.backend == "qwen") {
        candidates.emplace_back(kQwenModel, "qwen");
    }

    const Catalogue catalogue = load_catalogue(model_dirs);
    std::vector<std::string> ids;
    for (const auto& [id, backend] : candidates) {
        const models::Model* entry = catalogue.registry.find(id);
        if (entry == nullptr) {
            throw std::runtime_error(id + " is not in the registry");
        }
        try {
            // resolve() throws when a file of the entry is missing, and says how to fetch it.
            for (const models::File& file : entry->files) {
                models::resolve(catalogue.dir, *entry, file.role);
            }
            // The run also needs the backend's default VAD, unless --vad names one.
            if (o.run.vad.empty()) {
                const std::string vad = catalogue.registry.default_vad(backend);
                const models::Model* vad_entry = catalogue.registry.find(vad);
                if (vad_entry == nullptr) {
                    throw std::runtime_error(vad + " is not in the registry");
                }
                models::resolve(catalogue.dir, *vad_entry, "vad");
            }
            ids.push_back(id);
        } catch (const std::runtime_error& e) {
            std::fprintf(stderr, "note: skipping %s: %s\n", id.c_str(), e.what());
        }
    }
    return ids;
}

// The transcript as one line: the segment texts joined by single spaces. The end-to-end runner
// joins the same way, so both score the same text.
std::string transcript(const std::vector<Segment>& segments) {
    std::string line;
    for (const Segment& segment : segments) {
        if (!line.empty()) {
            line += ' ';
        }
        line += segment.text;
    }
    return line;
}

// The width of the model column: the longest name, and at least the header's "model".
int model_column_width(const std::vector<std::string>& models) {
    std::size_t width = std::string("model").size();
    for (const std::string& id : models) {
        width = std::max(width, id.size());
    }
    return static_cast<int>(width);
}

void print_header(int width) {
    std::printf("%-8s %-*s %8s %5s %5s %5s %8s %8s %8s %7s %7s\n", "backend", width, "model",
                "rate", "subs", "dels", "ins", "ref_len", "audio s", "wall s", "RTF", "load s");
    std::fflush(stdout);
}

// Flushed at once, so a row is on screen before the next model runs.
void print_row(const Row& row, int width) {
    const double rtf = row.stats.audio_s > 0.0 ? row.stats.wall_s / row.stats.audio_s : 0.0;
    std::printf("%-8s %-*s %8.4f %5zu %5zu %5zu %8zu %8.2f %8.2f %7.3f %7.2f\n",
                row.backend.c_str(), width, row.model.c_str(), row.score.rate, row.score.subs,
                row.score.dels, row.score.ins, row.score.ref_len, row.stats.audio_s,
                row.stats.wall_s, rtf, row.stats.load_s);
    std::fflush(stdout);
}

}  // namespace

void run_bench(const BenchOptions& o, const std::vector<fs::path>& model_dirs) {
    // A capture device has no recorded audio to score.
    if (is_capture(parse_input(o.input))) {
        throw UsageError("bench scores a WAV file, not a capture device");
    }
    const std::string ref = read_text_input(o.ref);
    const std::vector<std::string> models = models_to_run(o, model_dirs);
    if (models.empty()) {
        throw std::runtime_error(
            "no model to run; fetch one with tools/fetch-models.sh (tools/fetch-models.ps1 on "
            "Windows)");
    }

    // With --model, a backend other than "all" must match the model, and transcribe checks that.
    // Otherwise the default model's registry entry names the backend.
    const std::string backend = !o.model.empty() && o.backend != "all" ? o.backend : "";
    // Rows are printed as their runs finish, so the finished rows stay when a later run fails.
    const int width = model_column_width(models);
    std::printf("audio %s, reference %s\n", o.input.c_str(), o.ref.c_str());
    print_header(width);
    for (const std::string& id : models) {
        TranscribeOptions options;
        options.input = parse_input(o.input);
        options.backend = backend;
        options.model = id;
        options.run = o.run;
        const OfflineResult result = transcribe_offline(options, model_dirs);
        const Row row{result.backend, id, text::error_rate(transcript(result.segments), ref),
                      result.stats};
        print_row(row, width);
    }
}

}  // namespace echoir::cli
