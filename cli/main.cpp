// The echoir command line. `echoir transcribe FILE --model ID --offline` transcribes a whole WAV
// file and prints one line per speech segment. `--stream` runs the same file through a streaming
// session instead. A `-` input reads standard input: a WAV, or raw samples when `--pcm`, `--rate`
// and `--channels` describe them. `--input mic` and `--input loopback` capture live audio
// from a device, and run until Ctrl-C or `--duration`. `echoir bench` scores several models on one
// file, and `echoir score` scores a transcript. `echoir models list` shows the registry's models
// and whether they are on disk, and `echoir models path ID` prints where a model's files are.
// `echoir devices` lists the capture and loopback devices. `echoir serve --stdio` reads JSON
// commands on standard input and writes events and replies on standard output
// (docs/protocol.md). Usage errors exit with 2, and a failure while running exits with 1.
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "CLI11.hpp"
#include "bench.h"
#include "capture/devices.h"
#include "devices_cmd.h"
#include "input.h"
#include "model_dirs.h"
#include "models.h"
#include "models_cmd.h"
#include "offline.h"
#include "output.h"
#include "score.h"
#include "serve.h"
#include "stream.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace cli = echoir::cli;

namespace {

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// The option that picks the directory searched for registry.json and the model files first.
void add_models_dir_option(CLI::App* command, std::string& models_dir) {
    command->add_option("--models-dir", models_dir,
                        "directory with registry.json and the model files; searched first");
}

// The options that transcribe and bench share.
void add_run_options(CLI::App* command, cli::RunSettings& run, std::string& models_dir) {
    command->add_option("--vad", run.vad,
                        "VAD model: a registry id or a file; default: the backend's VAD in "
                        "models/registry.json");
    command->add_option("--language", run.language, "auto, or an ISO code such as en")
        ->capture_default_str();
    command->add_option("--threads", run.threads, "threads for decoding; 0 takes the default")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command->add_flag("--gpu{true},!--no-gpu{false}", run.use_gpu,
                      "use the GPU when this build has one");
    add_models_dir_option(command, models_dir);
}

int usage_error(const char* message) {
    std::fprintf(stderr, "error: %s\n", message);
    return kExitUsage;
}

// Runs one command. An option that cannot work is a usage error. Any other failure prints its
// message and returns kExitFailure.
template <typename Command>
int guarded(Command&& command) {
    try {
        command();
        return 0;
    } catch (const cli::UsageError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitUsage;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitFailure;
    }
}

// Runs the command line and returns the exit status. The template takes the narrow argv on POSIX
// and the wide argv on Windows, which CLI11 converts to UTF-8. argv0 is the fallback for finding
// the executable's directory, and is null when the platform reports that path itself.
template <typename CharT>
int run(int argc, const CharT* const* argv, const char* argv0) {
    cli::TranscribeOptions transcribe_options;
    cli::BenchOptions bench;
    cli::ScoreOptions score;
    std::string input_arg;     // the positional input
    std::string input_option;  // --input, the same as the positional
    std::string pcm;
    int pcm_rate = 0;
    int pcm_channels = 0;
    std::string models_dir;
    std::string model_id;
    bool offline = false;
    bool stream = false;
    int chunk_ms = 500;
    std::string pace = "none";
    std::string format = "text";

    CLI::App app{"Speech-to-text from the command line"};
    app.require_subcommand(1);

    CLI::App* transcribe = app.add_subcommand("transcribe", "Transcribe a WAV file");
    // Named file rather than input: CLI11 matches --input against a positional named input.
    CLI::Option* input_positional = transcribe->add_option(
        "file", input_arg,
        "WAV file, at any sample rate and channel count; - reads standard input; mic[:device] or "
        "loopback[:device] captures live audio (with --stream). A file named mic is ./mic");
    CLI::Option* input_flag = transcribe->add_option(
        "--input", input_option,
        "the same as the input argument: a WAV file, -, mic[:device] or loopback[:device]");
    input_positional->excludes(input_flag);
    CLI::Option* pcm_flag =
        transcribe
            ->add_option("--pcm", pcm,
                         "raw samples on standard input: s16le (16-bit signed) or f32le "
                         "(32-bit float), little-endian; needs - as the input")
            ->check(CLI::IsMember({"s16le", "f32le"}));
    CLI::Option* rate_flag =
        transcribe->add_option("--rate", pcm_rate, "sample rate of the raw samples, in Hz")
            ->check(CLI::PositiveNumber)
            ->needs(pcm_flag);
    CLI::Option* channels_flag =
        transcribe->add_option("--channels", pcm_channels, "channels of the raw samples, per frame")
            ->check(CLI::PositiveNumber)
            ->needs(pcm_flag);
    pcm_flag->needs(rate_flag)->needs(channels_flag);
    transcribe
        ->add_option("--model", transcribe_options.model, "model: a registry id or a model file")
        ->required();
    transcribe
        ->add_option("--backend", transcribe_options.backend,
                     "whisper or qwen; needed when --model is a file, and otherwise taken from "
                     "the model")
        ->check(CLI::IsMember({"whisper", "qwen"}));
    transcribe->add_option("--mmproj", transcribe_options.mmproj,
                           "Qwen projector: a registry id or a file; a model id brings its own");
    std::string context_text;
    std::string context_file;
    CLI::Option* context_flag =
        transcribe->add_option("--context", context_text,
                               "text sent with every decode, such as proper names; line breaks "
                               "become spaces");
    CLI::Option* context_file_flag = transcribe->add_option(
        "--context-file", context_file, "the context text, read from a UTF-8 file");
    context_flag->excludes(context_file_flag);
    CLI::Option* offline_flag = transcribe->add_flag(
        "--offline", offline, "transcribe the whole file at once, then print the segments");
    CLI::Option* stream_flag = transcribe->add_flag(
        "--stream", stream, "transcribe through a streaming session, as live audio would be");
    offline_flag->excludes(stream_flag);
    transcribe
        ->add_option("--chunk-ms", chunk_ms,
                     "audio pushed per step with --stream, in ms; for live capture, the "
                     "largest push")
        ->check(CLI::PositiveNumber)
        ->needs(stream_flag)
        ->capture_default_str();
    transcribe
        ->add_option("--pace", pace,
                     "none: push as fast as possible; realtime: at the speed of the audio. "
                     "Ignored for live capture, which runs at the device's speed")
        ->check(CLI::IsMember({"none", "realtime"}))
        ->needs(stream_flag)
        ->capture_default_str();
    CLI::Option* duration_flag =
        transcribe
            ->add_option("--duration", transcribe_options.duration_seconds,
                         "live capture only: stop after this many seconds (default: until Ctrl-C)")
            ->check(CLI::PositiveNumber)
            ->needs(stream_flag);
    transcribe
        ->add_option("--policy", transcribe_options.policy,
                     "how the ongoing text is stabilized: agreement, or rollback, which needs "
                     "Qwen3-ASR (default: rollback on Qwen3-ASR, agreement on Whisper)")
        ->check(CLI::IsMember({"agreement", "rollback"}))
        ->needs(stream_flag);
    transcribe
        ->add_option("--min-chunk-ms", transcribe_options.min_chunk_ms,
                     "audio that must be pending before a streaming decode runs, in ms")
        ->check(CLI::NonNegativeNumber)
        ->needs(stream_flag)
        ->capture_default_str();
    transcribe
        ->add_option("--max-segment-ms", transcribe_options.max_segment_ms,
                     "ongoing segment length that forces a cut at a quiet point, in ms")
        ->check(CLI::NonNegativeNumber)
        ->needs(stream_flag)
        ->capture_default_str();
    transcribe->add_option("--format", format, "output: text (one line per segment), jsonl or srt")
        ->check(CLI::IsMember({"text", "jsonl", "srt"}))
        ->capture_default_str();
    // Development only: an empty group hides the option from --help.
    transcribe
        ->add_option("--vad-param", transcribe_options.vad_params,
                     "a VAD setting as key=value, repeatable")
        ->group("");
    add_run_options(transcribe, transcribe_options.run, models_dir);

    CLI::App* bench_cmd = app.add_subcommand(
        "bench", "Transcribe a WAV file with each model and score it against a reference text");
    bench_cmd->add_option("input", bench.input, "WAV file, at any sample rate and channel count")
        ->required();
    bench_cmd->add_option("--ref", bench.ref, "reference text file, UTF-8")->required();
    bench_cmd
        ->add_option("--backend", bench.backend,
                     "whisper, qwen or all: which default models run; with --model, must match "
                     "the model")
        ->check(CLI::IsMember({"whisper", "qwen", "all"}))
        ->capture_default_str();
    bench_cmd->add_option("--model", bench.model, "a registry id: runs only that model");
    add_run_options(bench_cmd, bench.run, models_dir);

    CLI::App* devices_cmd = app.add_subcommand(
        "devices", "List the capture devices and the loopback devices, with their indexes");

    CLI::App* serve = app.add_subcommand(
        "serve",
        "Run a JSON-lines service: commands on standard input, events and replies on "
        "standard output");
    bool serve_stdio = false;
    serve
        ->add_flag("--stdio", serve_stdio, "read commands from standard input (the only transport)")
        ->required();
    add_models_dir_option(serve, models_dir);

    CLI::App* score_cmd = app.add_subcommand(
        "score", "Score a transcript against a reference text, as one JSON line");
    score_cmd->add_option("hyp", score.hyp, "transcript file, or - for standard input");
    score_cmd->add_option("ref", score.ref, "reference text file, or - for standard input");
    score_cmd->add_flag("--normalize", score.normalize,
                        "print the normalized text of the transcript file instead of scoring");

    CLI::App* models = app.add_subcommand("models", "Show the models in the registry");
    models->require_subcommand(1);
    CLI::App* models_list = models->add_subcommand(
        "list", "List each registry model with its backend, whether it is on disk, and its size");
    add_models_dir_option(models_list, models_dir);
    CLI::App* models_path = models->add_subcommand(
        "path", "Print the path of each file of a model, or the command that fetches it");
    models_path->add_option("id", model_id, "registry id of the model")->required();
    add_models_dir_option(models_path, models_dir);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // Help exits 0. Any other parse error is a usage error.
        return app.exit(e) == 0 ? 0 : kExitUsage;
    }

    if (transcribe->parsed()) {
        if (!offline && !stream) {
            return usage_error(
                "pass --offline to transcribe the whole file, or --stream for a streaming session");
        }
        for (const std::string& setting : transcribe_options.vad_params) {
            const std::size_t eq = setting.find('=');
            if (eq == 0 || eq == std::string::npos) {
                const std::string message = "--vad-param expects key=value, got '" + setting + "'";
                return usage_error(message.c_str());
            }
        }
        if (input_positional->count() + input_flag->count() == 0) {
            return usage_error("give the input as a WAV file, or - for standard input");
        }
        try {
            transcribe_options.input =
                cli::parse_input(input_positional->count() > 0 ? input_arg : input_option);
        } catch (const cli::UsageError& e) {
            return usage_error(e.what());
        }
        const bool live = cli::is_capture(transcribe_options.input);
        if (live && offline) {
            return usage_error("live capture needs --stream; --offline transcribes a whole file");
        }
        if (duration_flag->count() > 0 && !live) {
            return usage_error("--duration needs --input mic or --input loopback");
        }
        if (pcm_flag->count() > 0) {
            if (transcribe_options.input.kind != cli::InputSpec::Kind::standard_input) {
                return usage_error("--pcm needs standard input: pass - as the input");
            }
            transcribe_options.raw_pcm =
                cli::RawPcmFormat{pcm, static_cast<std::uint32_t>(pcm_rate),
                                  static_cast<std::uint32_t>(pcm_channels)};
        }
        return guarded([&] {
            transcribe_options.context = cli::read_context(context_text, context_file);
            cli::EventWriter writer(cli::parse_format(format), stdout, cli::is_terminal(stdout));
            const std::vector<std::filesystem::path> dirs =
                cli::model_search_dirs(models_dir, argv0);
            if (offline) {
                std::int64_t index = 0;
                cli::transcribe_offline(transcribe_options, dirs, [&](const cli::Segment& seg) {
                    // Offline segments are finals, numbered in order like stream segment ids.
                    echoir::SessionEvent event;
                    event.type = ECHOIR_EVENT_FINAL;
                    event.segment_id = ++index;
                    event.start_ms = seg.start_ms;
                    event.end_ms = seg.end_ms;
                    event.text = seg.text;
                    writer.write(event);
                });
            } else {
                cli::transcribe_stream(
                    transcribe_options, chunk_ms,
                    pace == "realtime" ? cli::Pace::realtime : cli::Pace::none, dirs,
                    [&](const echoir::SessionEvent& event) { writer.write(event); });
            }
        });
    }

    if (devices_cmd->parsed()) {
        return guarded([&] { cli::run_devices_list(); });
    }

    if (serve->parsed()) {
        // The service's exit status is 0 for quit and for the end of input. guarded reports a
        // failure outside the service, such as an option that cannot work.
        int status = 0;
        const int failed = guarded([&] {
            cli::ServeHooks hooks;
            const std::vector<std::filesystem::path> dirs =
                cli::model_search_dirs(models_dir, argv0);
            hooks.load = [dirs](const cli::TranscribeOptions& options) {
                return cli::load_models(options, dirs, echoir::vad::VadUse::streaming);
            };
            hooks.devices = [] { return echoir::capture::list_devices(); };
            status = cli::StdioService(std::cin, std::cout, std::move(hooks)).run();
        });
        return failed != 0 ? failed : status;
    }

    if (bench_cmd->parsed()) {
        return guarded([&] { cli::run_bench(bench, cli::model_search_dirs(models_dir, argv0)); });
    }

    if (models->parsed()) {
        return guarded([&] {
            const std::vector<std::filesystem::path> dirs =
                cli::model_search_dirs(models_dir, argv0);
            if (models_path->parsed()) {
                cli::run_models_path(model_id, dirs);
            } else {
                cli::run_models_list(dirs);
            }
        });
    }

    // score: two files, or one file with --normalize.
    if (score.hyp.empty()) {
        return usage_error("score needs a transcript file, or - for standard input");
    }
    if (score.normalize && !score.ref.empty()) {
        return usage_error("--normalize takes one file");
    }
    if (!score.normalize && score.ref.empty()) {
        return usage_error("score needs a reference file, or --normalize");
    }
    if (score.hyp == "-" && score.ref == "-") {
        return usage_error("only one of hyp and ref can be -");
    }
    return guarded([&] { cli::run_score(score); });
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    // The transcript and the messages are UTF-8, so the console is switched to UTF-8 to show them.
    SetConsoleOutputCP(CP_UTF8);
    return run(argc, argv, nullptr);
}
#else
int main(int argc, char* argv[]) { return run(argc, argv, argc > 0 ? argv[0] : nullptr); }
#endif
