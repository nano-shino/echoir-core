// Streaming session: turns audio pushed in real time into PARTIAL and FINAL segment events.
//
// The session works on a window of audio: the tail carried over from the previous tick, plus
// everything pushed since. A tick runs the VAD over the window and decodes each speech range. The
// last range, when it ends close to the window end, is ongoing. Its text is reported as PARTIAL,
// and its audio is carried into the next tick, so the decoder hears the whole segment again with
// more context. Every other range is final: it is reported once as FINAL, and its audio is never
// carried again. A segment that reaches max_segment_ms is cut at a quiet point and finalized. The
// windowing rules and the way the ongoing text is stabilized are described in
// session.cpp.
//
// Timestamps are milliseconds from the start of the session or from the last reset.
//
// Two ways to drive a session, and a session uses only one of them:
//  - pull: the caller runs process() on its own thread, and sinks run on that thread.
//  - live: start() launches one worker thread, which decodes as audio arrives. The worker runs
//    the sink for its events. stop() joins the worker, so no worker sink call runs after stop()
//    returns.
//
// Threads: push() may be called from any thread and never waits for a decode. process(), flush()
// and reset() run one at a time. clear() may run at any time. flush(), reset() and clear() run
// their sink on the calling thread, in live mode too. stop() waits for such a call that is still
// running. A call the host makes after stop() returns runs its sink on the host's thread. Inside
// a sink only push() may be called: the other session calls throw StateError.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "audio/resampler.h"
#include "backends/asr_backend.h"
#include "echoir/echoir.h"
#include "text/stabilizer.h"
#include "vad/vad.h"

namespace echoir {

// How the text of the ongoing segment is stabilized. See session.cpp.
enum class StreamPolicy {
    agreement,        // common prefix of the earlier decodes; works with any backend
    rollback,         // the previous decode minus its last tokens is the prefix; needs a prefix
    backend_default,  // rollback when the backend has a decoder prefix, else agreement
};

// Settings of a session. Times are milliseconds; the session converts them to samples.
struct SessionConfig {
    std::string language = "auto";        // passed to the backend; "auto" or "" lets it detect
    std::string context;                  // user text sent with every decode, ahead of the history
    std::int64_t min_chunk_ms = 500;      // a tick runs once more than this much audio is pending
    std::int64_t max_segment_ms = 10000;  // a segment is cut when it reaches this length
    bool emit_partials = true;            // false: no PARTIAL events
    // The C++ default is agreement, which runs on any backend. The C ABI and the CLI default to
    // backend_default.
    StreamPolicy policy = StreamPolicy::agreement;
    // Rollback only: the first unfixed_decodes decodes of an ongoing segment get no prefix, and
    // each later one drops rollback_tokens tokens from the end of the previous output.
    int unfixed_decodes = 2;
    int rollback_tokens = 5;
};

// One event. Times are milliseconds from the start of the session or from the last reset. The
// strings are valid only during the sink call.
struct SessionEvent {
    echoir_event_type type = ECHOIR_EVENT_FINAL;
    std::int64_t segment_id = 0;  // a PARTIAL and the FINAL that replaces it share this id
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;    // PARTIAL and FINAL, UTF-8; empty for other kinds
    std::string detail;  // ERROR: the message
};

// Receives the events in order. It runs on the thread that drives the session (see the threading
// notes on Session). A sink that throws loses that event, and the session goes on.
using SessionSink = std::function<void(const SessionEvent&)>;

class Session {
public:
    // Throws std::invalid_argument if min_chunk_ms is negative, if max_segment_ms is below 200 (the
    // quietest-point search needs room for a 100 ms block), if unfixed_decodes or rollback_tokens
    // is negative, if the policy is rollback and the backend has no prefix support, or if sink is
    // empty.
    Session(IAsrBackend& backend, vad::IVad& vad, SessionConfig config, SessionSink sink);
    // Stops the worker, if one runs. Destroying the session from its own sink aborts the process:
    // the worker would keep running on members that are already gone.
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Appends interleaved frames at any rate and channel count. sample_rate is in Hz and channels
    // is the number of interleaved channels per frame. The audio is converted to 16 kHz mono and
    // copied, and the call does not wait for a decode. Throws std::invalid_argument for a
    // non-positive rate or channel count, a sample count that is not whole frames, or a non-finite
    // sample.
    void push(std::span<const float> interleaved, std::int32_t sample_rate, std::int32_t channels);

    // Runs one tick if more than min_chunk_ms of audio is pending, and does nothing otherwise.
    // Throws StateError in live mode. Throws BackendFailure when the VAD or the decoder fails.
    // Throws std::invalid_argument when the decoder rejects the request, such as an unsupported
    // language, or when the VAD rejects a window too long for it. An ERROR event is sent first, and
    // the audio of the failed tick stays pending, so a later call retries it. The events of a
    // successful tick go to the sink on the calling thread.
    void process();

    // Decodes all pending audio and finalizes the ongoing segment as a FINAL event. Timestamps keep
    // running. Throws like process(). In live mode it waits for a tick that the worker is running,
    // and the events go to the sink on the calling thread.
    void flush();

    // flush(), then drops all audio and text state, so timestamps restart at 0. Audio pushed
    // while reset runs is dropped too. Segment ids keep counting up. The state is dropped even
    // when the final decode fails, and then the exception is rethrown.
    void reset();

    // Drops the text in flight and sends CLEARED on the calling thread. Text decoded from audio
    // grabbed before the call is dropped as well. The audio of a segment that is still ongoing is
    // kept, so that segment's text can show again in a later PARTIAL. Timestamps keep running.
    void clear();

    // Live mode. start() launches the worker, which decodes whenever more than min_chunk_ms is
    // pending. Audio pushed before start() is decoded at once. Throws StateError if the session
    // ran process(), or if it is already started. Other failures, such as a thread that cannot
    // start, are rethrown. stop() joins the worker and does not flush. Audio that was not decoded
    // stays pending. A live decode that fails sends an ERROR event and drops the audio of that
    // tick, so the worker never decodes the same audio twice after a failure.
    // Throws StateError when called from a sink, as do the other session calls except push().
    void start();
    void stop();

    // Sends an ERROR event with the message, for a failure outside the decoder, such as a lost
    // capture device. It is serialized with the other events, so it never runs with them at the
    // same time. Thread-safe. Throws nothing. Not from a sink.
    void report_error(const std::string& message) noexcept;

private:
    enum class Mode { unset, pull, live };

    // Everything a tick can change. A tick works on a copy and stores it back only when the
    // whole tick succeeded, so a failed tick leaves no trace.
    struct State {
        std::vector<std::string> prompts;  // last finals, UTF-8, passed to the decoder as history
        text::Stabilizer open_text;        // agreement: decodes of the ongoing segment, for display
        std::u32string open_latest;        // newest cleaned decode of the ongoing segment
        std::int64_t open_id = 0;          // 0 when no segment is ongoing
        bool open_shown = false;           // a PARTIAL has been queued for open_id
        std::int64_t open_start_ms = 0;
        std::int64_t open_end_ms = 0;
        std::int64_t next_id = 1;
        // Rollback: decodes of the ongoing segment so far, and the continuation that the next
        // decode of it starts from. Both are empty or zero when no segment is ongoing.
        int open_decodes = 0;
        std::string continuation;
        // The latest FINAL, when it ended at a forced cut, and the cut in session ms. The segment
        // that starts at the cut is decoded with it or with no history (history_at_cut()), and
        // drops its leading words that repeat it. Empty and -1 otherwise.
        std::u32string seam_text;
        std::int64_t seam_ms = -1;
        // The clear() generation that the continuation and the seam were decoded under. clear()
        // makes both stale.
        std::uint64_t decoded_generation = 0;

        // The FINAL that ends the ongoing segment: its newest decode, which a finish sends. Empty
        // when there is no ongoing segment, or when it has no text and no PARTIAL was shown.
        std::optional<SessionEvent> closing_event() const;
        // Forgets the ongoing segment. Its id is not reused, because next_id keeps counting.
        void close_open();
        // Forgets the continuation, so the next decode of the ongoing segment starts over.
        void drop_continuation();
        void drop_seam();
    };

    // Runs one tick. Returns true when the tick took pending audio, and false when there was
    // nothing to do. A failed tick throws, after it has sent its ERROR event. With drop_on_failure
    // set (live mode), a failed tick also gives up its window: see abandon_window.
    bool run_tick(bool finish, bool drop_on_failure);
    // Runs the VAD and the decoder over the window and queues the events. origin is the session
    // time of window[0], in samples. Returns the window index where the carried audio starts.
    // With finish set, it finalizes the ongoing segment and returns the window size. The one
    // exception is a forced cut, which returns the cut: the remainder still needs a decode.
    std::size_t analyze(const std::vector<float>& window, bool finish, std::int64_t origin,
                        State& st, std::vector<SessionEvent>& events);
    // Sends the events in order, skipping text from before the generation was bumped by clear().
    void deliver(const std::vector<SessionEvent>& events, std::uint64_t generation);
    void emit_locked(const SessionEvent& event);
    // Gives up the window of a failed live tick: the carried audio and the first `taken` pending
    // samples are dropped, and an ongoing segment ends with its newest decode. Nothing is decoded
    // here, so the failing range is never tried again. The ERROR event was already sent, so this
    // does not report anything on its own failure.
    void abandon_window(std::size_t taken, std::uint64_t generation) noexcept;
    // Throws StateError when the calling thread is inside a sink of this session. The call would
    // otherwise wait on a lock that the sink's own call still holds.
    void refuse_from_sink(const char* call) const;
    void drain_resampler();
    void drop_state();
    // The worker's body: waits for a wake-up, then ticks while ticks take audio.
    void worker_loop(std::uint64_t run);
    // One tick for the worker, skipped when run is no longer the current run. A failed tick is
    // dropped, not retried, so the worker waits for the next push.
    bool tick_worker(std::uint64_t run) noexcept;
    // Asks the worker to check for audio. Does nothing when no worker runs.
    void wake_worker();

    IAsrBackend& backend_;
    vad::IVad& vad_;
    SessionConfig config_;
    SessionSink sink_;
    std::int64_t min_chunk_samples_ = 0;
    std::int64_t max_segment_samples_ = 0;

    // Lock order: lifecycle_mu_, decode_mu_, worker_mu_, then audio_mu_ or text_mu_. audio_mu_ and
    // text_mu_ are never held together. worker_mu_ is never held across a decode.
    std::mutex lifecycle_mu_;  // start and stop, so the worker handle is changed by one caller
    std::mutex decode_mu_;     // held for a whole tick by process, flush, reset and the worker
    std::mutex audio_mu_;      // pending_ and resampler_; push holds it only while converting
    std::mutex text_mu_;       // generation_, and every sink call
    std::mutex worker_mu_;     // the worker's wake-up state; push takes it to wake the worker
    std::condition_variable worker_cv_;

    std::optional<audio::Resampler> resampler_;  // audio_mu_
    std::int32_t rate_ = 0;                      // audio_mu_: format of resampler_
    std::int32_t channels_ = 0;                  // audio_mu_
    std::vector<float> pending_;                 // audio_mu_: 16 kHz mono, not yet ticked
    std::uint64_t generation_ = 0;               // text_mu_: bumped by clear()

    std::vector<float> carry_;         // decode_mu_: audio carried into the next tick
    std::int64_t origin_samples_ = 0;  // decode_mu_: session time of carry_[0], in samples
    State state_;                      // decode_mu_
    Mode mode_ = Mode::unset;          // decode_mu_
    // decode_mu_: bumped by stop(). A worker tick from an older run does nothing.
    std::uint64_t run_generation_ = 0;

    std::thread worker_;           // lifecycle_mu_
    bool worker_running_ = false;  // worker_mu_
    bool wake_ = false;            // worker_mu_: audio may be pending
};

}  // namespace echoir
