#include "bench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "input.h"
#include "json.hpp"
#include "latency.h"
#include "model_dirs.h"
#include "models.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "offline.h"
#include "options.h"
#include "score.h"
#include "stream.h"
#include "stream/engine_gate.h"
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

// One table row of a run with several streams: one stream of it. The times are in ms.
struct StreamsRow {
    std::string model;
    int stream = 0;  // 1-based
    text::ErrorRate score;
    std::optional<double> rtf;      // mean of the speed the session reported
    std::optional<double> wait_ms;  // mean of the time per tick it waited for the other streams
    std::optional<double> first_p50;
    std::optional<double> first_p95;
    std::optional<double> final_p50;
    std::optional<double> final_p95;
    std::optional<double> backlog_p95;
    std::optional<double> backlog_max;
    std::string verdict;  // "live", "strained" or "behind"
};

// The verdict of a stream from its backlog readings, with the margins of a live display: the
// backlog at the 90th percentile of the per-second peaks against min_chunk_ms plus 1.5 s
// (strained) or 4 s (behind), and the slope of the backlog over the run, 0.05 or 0.10 ms per ms,
// for a backlog that keeps growing.
std::string lag_verdict(const std::vector<std::pair<double, double>>& readings,
                        double min_chunk_ms) {
    if (readings.size() < 2) {
        return "live";
    }
    std::vector<double> peaks;
    double second_start = readings.front().first;
    double peak = 0.0;
    for (const auto& [at, backlog] : readings) {
        if (at >= second_start + 1000.0) {
            peaks.push_back(peak);
            second_start = at;
            peak = backlog;
        } else {
            peak = std::max(peak, backlog);
        }
    }
    peaks.push_back(peak);
    const double typical = percentile(peaks, 90.0).value_or(0.0);
    // Least-squares slope of the backlog over the time of the run.
    const auto n = static_cast<double>(readings.size());
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (const auto& [at, backlog] : readings) {
        const double x = at - readings.front().first;
        sum_x += x;
        sum_y += backlog;
        sum_xx += x * x;
        sum_xy += x * backlog;
    }
    const double denominator = n * sum_xx - sum_x * sum_x;
    const double slope = denominator > 0.0 ? (n * sum_xy - sum_x * sum_y) / denominator : 0.0;
    if (typical > min_chunk_ms + 4000.0 || slope > 0.10) {
        return "behind";
    }
    if (typical > min_chunk_ms + 1500.0 || slope > 0.05) {
        return "strained";
    }
    return "live";
}

void print_streams_header(int width) {
    std::printf("%-*s %6s %8s %7s %7s %9s %9s %9s %9s %11s %11s %8s\n", width, "model", "stream",
                "rate", "RTF", "wait", "1st p50", "1st p95", "final p50", "final p95",
                "backlog p95", "backlog max", "verdict");
    std::fflush(stdout);
}

void print_streams_row(const StreamsRow& row, int width) {
    std::printf("%-*s %6d %8.4f %s %s %s %s %s %s %s %s %8s\n", width, row.model.c_str(),
                row.stream, row.score.rate, cell(row.rtf, 7, "%7.3f").c_str(),
                cell(row.wait_ms, 7, "%7.0f").c_str(), cell(row.first_p50, 9, "%9.0f").c_str(),
                cell(row.first_p95, 9, "%9.0f").c_str(), cell(row.final_p50, 9, "%9.0f").c_str(),
                cell(row.final_p95, 9, "%9.0f").c_str(),
                cell(row.backlog_p95, 11, "%11.0f").c_str(),
                cell(row.backlog_max, 11, "%11.0f").c_str(), row.verdict.c_str());
    std::fflush(stdout);
}

// Runs o.streams sessions at once on one loaded model, as a host with several audio sources does.
// Each session gets the whole input, pushed at its own speed from the same moment, so their speech
// arrives together and their decodes wait for each other the most. Returns one row per stream.
std::vector<StreamsRow> run_streams_model(const BenchOptions& o, const std::string& id,
                                          const std::string& ref,
                                          const std::vector<fs::path>& model_dirs) {
    using Clock = std::chrono::steady_clock;

    TranscribeOptions options;
    options.input = parse_input(o.input);
    options.backend = !o.model.empty() && o.backend != "all" ? o.backend : "";
    options.model = id;
    options.policy = o.policy;
    options.run = o.run;

    const audio::PcmBuffer input = read_input(options.input, std::nullopt);
    LoadedModels models = load_models(options, model_dirs, vad::VadUse::streaming);
    check_request(*models.asr, options);
    const SessionConfig config = session_config(options, models);
    auto gate = std::make_shared<stream::EngineGate>();

    // What one stream records. Its sink runs on its worker or on its pusher, never on two threads
    // at once; the pusher writes the pushes, and the sampler the readings. Each is read after the
    // threads that write it are joined.
    struct Stream {
        std::vector<PushTime> pushes;
        std::vector<TextTime> partials;
        std::vector<TextTime> finals;
        std::vector<std::string> final_texts;
        std::vector<double> speeds;
        std::vector<double> waits;
        std::vector<std::pair<double, double>> readings;  // (ms since start, backlog ms)
        std::atomic<bool> failed{false};
        std::unique_ptr<Session> session;
    };
    const Clock::time_point origin = Clock::now();
    const auto ms_since_origin = [origin](Clock::time_point at) {
        return std::chrono::duration<double, std::milli>(at - origin).count();
    };
    std::vector<std::unique_ptr<Stream>> streams;
    for (int i = 0; i < o.streams; ++i) {
        auto s = std::make_unique<Stream>();
        Stream* raw = s.get();
        const SessionSink sink = [raw, &ms_since_origin](const SessionEvent& event) {
            const double at_ms = ms_since_origin(Clock::now());
            switch (event.type) {
                case ECHOIR_EVENT_PARTIAL:
                    if (!event.text.empty()) {
                        raw->partials.push_back({event.segment_id, event.end_ms, at_ms});
                    }
                    break;
                case ECHOIR_EVENT_FINAL:
                    if (!event.text.empty()) {
                        raw->finals.push_back({event.segment_id, event.end_ms, at_ms});
                        raw->final_texts.push_back(event.text);
                    }
                    break;
                case ECHOIR_EVENT_STATS: {
                    const nlohmann::json fields =
                        nlohmann::json::parse(event.detail, nullptr, false);
                    if (fields.is_object() && fields.contains("rtf") && fields["rtf"].is_number()) {
                        raw->speeds.push_back(fields["rtf"].get<double>());
                    }
                    if (fields.is_object() && fields.contains("wait_ms") &&
                        fields["wait_ms"].is_number()) {
                        raw->waits.push_back(fields["wait_ms"].get<double>());
                    }
                    break;
                }
                case ECHOIR_EVENT_ERROR:
                    std::fprintf(stderr, "error: %s\n", event.detail.c_str());
                    raw->failed = true;
                    break;
                case ECHOIR_EVENT_CLEARED:
                    break;
            }
        };
        s->session = std::make_unique<Session>(*models.asr, *models.vad, config, sink, gate);
        streams.push_back(std::move(s));
    }

    const auto rate = static_cast<std::int32_t>(input.rate);
    const auto channels = static_cast<std::int32_t>(input.channels);
    const std::size_t total_frames = input.samples.size() / input.channels;
    const std::size_t chunk_frames =
        std::max<std::size_t>(1, static_cast<std::size_t>(input.rate) * kStreamChunkMs / 1000);
    std::atomic<int> pushing{o.streams};
    const Clock::time_point start = Clock::now();
    std::vector<std::thread> pushers;
    for (const auto& s : streams) {
        s->session->start();
    }
    for (const auto& s : streams) {
        Stream* raw = s.get();
        pushers.emplace_back([&, raw] {
            for (std::size_t frame = 0; frame < total_frames && !raw->failed.load();
                 frame += chunk_frames) {
                const std::size_t frames = std::min(chunk_frames, total_frames - frame);
                const auto captured = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(frame + frames) / rate));
                std::this_thread::sleep_until(start + captured);
                raw->session->push(std::span<const float>(input.samples)
                                       .subspan(frame * input.channels, frames * input.channels),
                                   rate, channels);
                raw->pushes.push_back({static_cast<std::int64_t>((frame + frames) * 1000 / rate),
                                       ms_since_origin(Clock::now())});
            }
            --pushing;
            try {
                raw->session->flush();
                raw->session->stop();
            } catch (const std::exception&) {
                // The session has sent an ERROR event for it.
                raw->failed = true;
            }
        });
    }
    // The backlog of every stream, about 30 times a second while audio arrives, as a host that
    // shows the lag reads it.
    while (pushing.load() > 0) {
        const double at_ms = ms_since_origin(Clock::now());
        for (const auto& s : streams) {
            s->readings.emplace_back(at_ms, static_cast<double>(s->session->lag().backlog_ms));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    for (std::thread& t : pushers) {
        t.join();
    }

    std::vector<StreamsRow> rows;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const Stream& s = *streams[i];
        if (s.failed) {
            throw std::runtime_error("a decode failed during the run; see the error above");
        }
        StreamsRow row;
        row.model = id;
        row.stream = static_cast<int>(i) + 1;
        std::string line;
        for (const std::string& text : s.final_texts) {
            if (!line.empty()) {
                line += ' ';
            }
            line += text;
        }
        row.score = text::error_rate(line, ref);
        const auto mean = [](const std::vector<double>& values) -> std::optional<double> {
            if (values.empty()) {
                return std::nullopt;
            }
            return std::accumulate(values.begin(), values.end(), 0.0) /
                   static_cast<double>(values.size());
        };
        row.rtf = mean(s.speeds);
        row.wait_ms = mean(s.waits);
        const SegmentLatencies latencies = segment_latencies(s.pushes, s.partials, s.finals);
        row.first_p50 = percentile(latencies.first_partial_ms, 50.0);
        row.first_p95 = percentile(latencies.first_partial_ms, 95.0);
        row.final_p50 = percentile(latencies.final_ms, 50.0);
        row.final_p95 = percentile(latencies.final_ms, 95.0);
        std::vector<double> backlogs;
        for (const auto& reading : s.readings) {
            backlogs.push_back(reading.second);
        }
        row.backlog_p95 = percentile(backlogs, 95.0);
        if (!backlogs.empty()) {
            row.backlog_max = *std::max_element(backlogs.begin(), backlogs.end());
        }
        row.verdict = lag_verdict(s.readings, static_cast<double>(config.min_chunk_ms));
        rows.push_back(std::move(row));
    }
    return rows;
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
    if (o.stream && o.streams > 0) {
        std::printf("%d streams at once on one model; latency and backlog in ms\n", o.streams);
        print_streams_header(width);
        for (const std::string& id : models) {
            for (const StreamsRow& row : run_streams_model(o, id, ref, model_dirs)) {
                print_streams_row(row, width);
            }
        }
        return;
    }
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
