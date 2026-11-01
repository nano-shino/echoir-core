#include "bench.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "input.h"
#include "json.hpp"
#include "latency.h"
#include "model_dirs.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "offline.h"
#include "options.h"
#include "score.h"
#include "stream.h"
#include "text/score.h"
#include "text/utf.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// The default model of each backend. Both are entries of models/registry.json.
constexpr const char* kWhisperModel = "whisper-tiny-en";
constexpr const char* kQwenModel = "qwen3-asr-0.6b-q8_0";

// The audio pushed per step in a streaming run. It is the default of `transcribe --chunk-ms`.
constexpr int kStreamChunkMs = 500;

// One table row: a finished run.
struct Row {
    std::string backend;
    std::string model;
    text::ErrorRate score;
    OfflineStats stats;
};

// One table row of a streaming run. The latencies are in ms, and empty when the run had no text.
struct StreamRow {
    std::string backend;
    std::string model;
    std::string policy;  // "default", "agreement" or "rollback"
    text::ErrorRate score;
    double audio_s = 0.0;
    std::optional<double> rtf;  // mean of the speed the session reported, when it reported any
    std::optional<double> first_p50;
    std::optional<double> first_p95;
    std::optional<double> final_p50;
    std::optional<double> final_p95;
    std::size_t finals = 0;  // segments with text, the sample the final latencies come from
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
        if (o.stream && o.policy == "rollback" && backend == "whisper") {
            std::fprintf(stderr, "note: skipping %s: the rollback policy needs Qwen3-ASR\n",
                         id.c_str());
            continue;
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

// A latency or a speed as a table cell of the given width: "-" when there is no value.
std::string cell(const std::optional<double>& value, int width, const char* format) {
    if (!value) {
        return std::string(static_cast<std::size_t>(width - 1), ' ') + "-";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, format, *value);
    return buf;
}

void print_stream_header(int width) {
    std::printf("%-8s %-*s %-9s %8s %5s %5s %5s %8s %8s %7s %9s %9s %9s %9s %6s\n", "backend",
                width, "model", "policy", "rate", "subs", "dels", "ins", "ref_len", "audio s",
                "RTF", "1st p50", "1st p95", "final p50", "final p95", "finals");
    std::fflush(stdout);
}

void print_stream_row(const StreamRow& row, int width) {
    std::printf("%-8s %-*s %-9s %8.4f %5zu %5zu %5zu %8zu %8.2f %s %s %s %s %s %6zu\n",
                row.backend.c_str(), width, row.model.c_str(), row.policy.c_str(), row.score.rate,
                row.score.subs, row.score.dels, row.score.ins, row.score.ref_len, row.audio_s,
                cell(row.rtf, 7, "%7.3f").c_str(), cell(row.first_p50, 9, "%9.0f").c_str(),
                cell(row.first_p95, 9, "%9.0f").c_str(), cell(row.final_p50, 9, "%9.0f").c_str(),
                cell(row.final_p95, 9, "%9.0f").c_str(), row.finals);
    std::fflush(stdout);
}

// The registry backend of a model, as the table names it: "whisper" or "qwen".
std::string backend_of(const std::string& id, const std::vector<fs::path>& model_dirs) {
    const Catalogue catalogue = load_catalogue(model_dirs);
    const models::Model* entry = catalogue.registry.find(id);
    if (entry == nullptr) {
        throw std::runtime_error(id + " is not in the registry");
    }
    return models::backend_key(entry->backend);
}

// Runs one model through a streaming session. The audio is pushed at its own speed, so the
// latencies are those a live user would see. The speed is the mean of the STATS events.
StreamRow run_stream_model(const BenchOptions& o, const std::string& id, const std::string& ref,
                           const std::vector<fs::path>& model_dirs) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point origin = Clock::now();
    const auto ms_since_origin = [origin](Clock::time_point at) {
        return std::chrono::duration<double, std::milli>(at - origin).count();
    };

    TranscribeOptions options;
    options.input = parse_input(o.input);
    options.backend = !o.model.empty() && o.backend != "all" ? o.backend : "";
    options.model = id;
    options.policy = o.policy;
    options.run = o.run;

    // The sink and the push observer run on different threads. The sink calls are serialized by
    // the session, and the observer runs on this thread, so the records need no lock.
    std::vector<PushTime> pushes;
    std::vector<TextTime> partials;
    std::vector<TextTime> finals;
    std::vector<double> speeds;
    std::vector<std::string> final_texts;
    const StreamSink sink = [&](const SessionEvent& event) {
        const double at_ms = ms_since_origin(Clock::now());
        switch (event.type) {
            case ECHOIR_EVENT_PARTIAL:
                if (!event.text.empty()) {
                    partials.push_back({event.segment_id, event.end_ms, at_ms});
                }
                break;
            case ECHOIR_EVENT_FINAL:
                if (!event.text.empty()) {
                    finals.push_back({event.segment_id, event.end_ms, at_ms});
                    final_texts.push_back(event.text);
                }
                break;
            case ECHOIR_EVENT_STATS: {
                const nlohmann::json fields = nlohmann::json::parse(event.detail, nullptr, false);
                if (fields.is_object() && fields.contains("rtf") && fields["rtf"].is_number()) {
                    speeds.push_back(fields["rtf"].get<double>());
                }
                break;
            }
            case ECHOIR_EVENT_CLEARED:
            case ECHOIR_EVENT_ERROR:
                break;
        }
    };
    const PushObserver observer = [&](std::int64_t audio_end_ms, Clock::time_point pushed) {
        pushes.push_back({audio_end_ms, ms_since_origin(pushed)});
    };
    transcribe_stream(options, kStreamChunkMs, Pace::realtime, model_dirs, sink, observer);

    StreamRow row;
    row.backend = backend_of(id, model_dirs);
    row.model = id;
    row.policy = o.policy.empty() ? "default" : o.policy;
    std::string line;
    for (const std::string& text : final_texts) {
        if (!line.empty()) {
            line += ' ';
        }
        line += text;
    }
    row.score = text::error_rate(line, ref);
    row.audio_s = pushes.empty() ? 0.0 : static_cast<double>(pushes.back().audio_end_ms) / 1000.0;
    if (!speeds.empty()) {
        row.rtf =
            std::accumulate(speeds.begin(), speeds.end(), 0.0) / static_cast<double>(speeds.size());
    }
    const SegmentLatencies latencies = segment_latencies(pushes, partials, finals);
    row.first_p50 = percentile(latencies.first_partial_ms, 50.0);
    row.first_p95 = percentile(latencies.first_partial_ms, 95.0);
    row.final_p50 = percentile(latencies.final_ms, 50.0);
    row.final_p95 = percentile(latencies.final_ms, 95.0);
    row.finals = latencies.final_ms.size();
    return row;
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

    // Rows are printed as their runs finish, so the finished rows stay when a later run fails.
    const int width = model_column_width(models);
    std::printf("audio %s, reference %s\n", o.input.c_str(), o.ref.c_str());
    if (o.stream) {
        std::printf(
            "latency in ms, from the moment the audio up to the text's end reached the "
            "session\n");
        print_stream_header(width);
        for (const std::string& id : models) {
            print_stream_row(run_stream_model(o, id, ref, model_dirs), width);
        }
        return;
    }

    // With --model, a backend other than "all" must match the model, and transcribe checks that.
    // Otherwise the default model's registry entry names the backend.
    const std::string backend = !o.model.empty() && o.backend != "all" ? o.backend : "";
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
