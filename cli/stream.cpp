#include "stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "capture/feed.h"
#include "devices_cmd.h"
#include "input.h"
#include "models.h"

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// Set by the Ctrl-C handler, which does nothing else, so the capture loop can stop.
volatile std::sig_atomic_t g_interrupted = 0;

void on_interrupt(int) {
    g_interrupted = 1;
    // A second Ctrl-C ends the process the usual way, in case the run is stuck.
    std::signal(SIGINT, SIG_DFL);
}

// Catches Ctrl-C for the length of a capture run, then puts the previous handler back.
class InterruptScope {
public:
    InterruptScope() : previous_(std::signal(SIGINT, on_interrupt)) { g_interrupted = 0; }
    ~InterruptScope() {
        if (previous_ != SIG_ERR) {
            std::signal(SIGINT, previous_);
        }
    }
    InterruptScope(const InterruptScope&) = delete;
    InterruptScope& operator=(const InterruptScope&) = delete;

private:
    void (*previous_)(int);
};

// The number of frames in a chunk of chunk_ms, and at least one.
std::size_t frames_in_chunk(std::uint32_t rate, int chunk_ms) {
    return std::max<std::size_t>(1, static_cast<std::size_t>(rate) * chunk_ms / 1000);
}

// Passes events on, and remembers an ERROR event, so that the run fails at the end. The flag is
// atomic, because a live session reports from its worker thread.
StreamSink recording_sink(const StreamSink& sink, std::atomic<bool>& failed) {
    return [&sink, &failed](const SessionEvent& event) {
        if (event.type == ECHOIR_EVENT_ERROR) {
            failed = true;
        }
        sink(event);
    };
}

// Captures live audio and runs it through a live session, until Ctrl-C, the --duration limit, a
// lost device or a session error. Models load first, then the device opens. Pace does not apply:
// a device delivers audio at its own speed.
void run_capture(const TranscribeOptions& o, const std::vector<fs::path>& model_dirs,
                 const StreamSink& sink) {
    const capture::Source source = o.input.kind == InputSpec::Kind::loopback
                                       ? capture::Source::loopback
                                       : capture::Source::mic;
    // Checked before any model is read, so a backend without loopback fails at once.
    check_source_available(source);
    const std::string device_id = device_id_for(source, o.input.device);

    LoadedModels models = load_models(o, model_dirs, vad::VadUse::streaming);
    check_request(*models.asr, o);
    std::atomic<bool> failed{false};
    const StreamSink watching = recording_sink(sink, failed);
    Session session(*models.asr, *models.vad, session_config(o, models), watching);
    // Declared after the session, so the device is closed before the session is destroyed.
    capture::CaptureFeed feed(source, device_id, session);

    const auto rate = static_cast<std::int32_t>(feed.rate());
    const auto channels = static_cast<std::int32_t>(feed.channels());
    const std::string name = feed.device_name();

    InterruptScope interrupt;
    session.start();
    feed.start();
    std::fprintf(stderr, "listening on %s (%d Hz, %d ch); press Ctrl-C to stop\n", name.c_str(),
                 rate, channels);

    const auto started = std::chrono::steady_clock::now();
    const bool timed = o.duration_seconds > 0.0;
    const std::chrono::duration<double> limit(o.duration_seconds);
    const auto poll = std::chrono::milliseconds(50);
    bool waiting = false;
    while (g_interrupted == 0 && !failed.load() && !feed.ended()) {
        if (timed && std::chrono::steady_clock::now() - started >= limit) {
            break;
        }
        if (feed.waiting() != waiting) {
            waiting = !waiting;
            if (waiting) {
                std::fprintf(stderr, "the audio device is gone; waiting for one to appear\n");
            } else {
                std::fprintf(stderr, "listening on %s\n", feed.device_name().c_str());
            }
        }
        std::this_thread::sleep_for(poll);
    }

    // The feed stops the device, so no more audio arrives, and pushes what it captured. Then the
    // session finalizes its last segment.
    const bool ended = feed.ended();
    feed.stop();
    session.flush();
    session.stop();

    const double seconds = static_cast<double>(feed.frames_delivered()) / rate;
    std::fprintf(stderr, "captured %s (%d Hz, %d ch): %.1f s, %llu frames dropped\n", name.c_str(),
                 rate, channels, seconds,
                 static_cast<unsigned long long>(feed.dropped_frames()));
    if (feed.peak() == 0.0f) {
        std::fprintf(stderr,
                     "warning: no signal arrived from %s. On macOS, the terminal app needs "
                     "microphone permission (System Settings > Privacy & Security > Microphone).\n",
                     name.c_str());
    }
    if (ended) {
        throw std::runtime_error("capture stopped early: " + name);
    }
    if (failed) {
        throw std::runtime_error("a decode failed during the stream; see the error event");
    }
}

// Pushes the input and drives the session. The input is opened first, so a bad input is reported
// before any model loads. A bad model fails before any audio is decoded. A capture device is
// handled by run_capture.
void run_stream(const TranscribeOptions& o, int chunk_ms, Pace pace,
                const std::vector<fs::path>& model_dirs, const StreamSink& sink) {
    if (chunk_ms <= 0) {
        throw std::invalid_argument("--chunk-ms must be positive");
    }
    if (is_capture(o.input)) {
        run_capture(o, model_dirs, sink);
        return;
    }
    const std::unique_ptr<ChunkSource> source = open_chunk_source(o.input, o.raw_pcm);
    // The input keeps its own rate and channel count: the session resamples what it is given.
    const std::uint32_t rate = source->rate();
    const std::uint32_t channels = source->channels();

    LoadedModels models = load_models(o, model_dirs, vad::VadUse::streaming);
    // A bad language or context fails here, so it is reported even when the input has no speech.
    check_request(*models.asr, o);

    // Live decoding reports a failure as an ERROR event from the worker, and the push loop reads
    // the flag while the worker runs. So the flag is atomic. It stops the push loop and makes the
    // run fail at the end.
    std::atomic<bool> failed{false};
    const StreamSink watching = recording_sink(sink, failed);
    Session session(*models.asr, *models.vad, session_config(o, models), watching);

    // The frames pushed per step, from --chunk-ms.
    const std::size_t chunk_frames = frames_in_chunk(rate, chunk_ms);
    const auto rate_i = static_cast<std::int32_t>(rate);
    const auto channels_i = static_cast<std::int32_t>(channels);
    if (pace == Pace::none) {
        for (std::span<const float> chunk = source->next(chunk_frames); !chunk.empty();
             chunk = source->next(chunk_frames)) {
            session.push(chunk, rate_i, channels_i);
            session.process();
        }
        session.flush();
        return;
    }

    // Realtime: the worker decodes while the audio is pushed. Each chunk is pushed once its audio
    // would have been captured, so the session sees audio at the speed of the input.
    session.start();
    const auto origin = std::chrono::steady_clock::now();
    std::size_t frames_pushed = 0;
    // A failed decode ends the run, as it does with Pace::none, so the rest of the input is not
    // paced through for nothing.
    for (std::span<const float> chunk = source->next(chunk_frames);
         !chunk.empty() && !failed.load(); chunk = source->next(chunk_frames)) {
        frames_pushed += chunk.size() / channels;
        const auto captured = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(frames_pushed) / rate));
        std::this_thread::sleep_until(origin + captured);
        session.push(chunk, rate_i, channels_i);
    }
    session.flush();
    session.stop();
    if (failed) {
        throw std::runtime_error("a decode failed during the stream; see the error event");
    }
}

}  // namespace

SessionConfig session_config(const TranscribeOptions& o, const LoadedModels& models) {
    const bool rollback = o.policy == "rollback";
    if (rollback && !models.asr->supports_prefix()) {
        throw UsageError("the rollback policy is not supported by the " + models.backend +
                         " backend");
    }
    SessionConfig config;
    config.language = o.run.language;
    // The context is set once here, so the session puts it in every request.
    config.context = o.context;
    config.policy = o.policy.empty() ? StreamPolicy::backend_default
                    : rollback       ? StreamPolicy::rollback
                                     : StreamPolicy::agreement;
    config.min_chunk_ms = o.min_chunk_ms;
    config.max_segment_ms = o.max_segment_ms;
    return config;
}

void transcribe_stream(const TranscribeOptions& o, int chunk_ms, Pace pace,
                       const std::vector<fs::path>& model_dirs, const StreamSink& sink) {
    // Set once an ERROR event has reached the sink. The session sends one for a failure in a
    // tick, and this reports the failures that come before the session exists.
    bool error_sent = false;
    const StreamSink reporting_sink = [&](const SessionEvent& event) {
        error_sent = error_sent || event.type == ECHOIR_EVENT_ERROR;
        sink(event);
    };
    try {
        run_stream(o, chunk_ms, pace, model_dirs, reporting_sink);
    } catch (const std::exception& e) {
        if (!error_sent) {
            SessionEvent event;
            event.type = ECHOIR_EVENT_ERROR;
            event.detail = e.what();
            try {
                reporting_sink(event);
            } catch (...) {
                // The caller still gets the failure from the rethrow below.
            }
        }
        throw;
    }
}

}  // namespace echoir::cli
