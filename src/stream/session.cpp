#include "stream/session.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>

#include "errors.h"
#include "stream/signal.h"
#include "text/clean.h"
#include "text/repetition.h"
#include "text/utf.h"

// How a session turns audio into segments. All audio is 16 kHz mono f32 and all positions are
// sample indices, so 16 samples make a millisecond.
//
// Window. The session holds two buffers: carry_, audio that a tick left for the next one, and
// pending_, audio pushed since the last tick took it. A tick runs once more than min_chunk_ms of
// audio is pending. It joins carry_ and the pending audio into a window whose first sample is at
// session time origin_samples_, then runs the VAD over the window.
//
// Final segments. Each speech range from the VAD becomes a segment. A segment is decoded once,
// its text is cleaned, and it is sent as a FINAL. Its audio is never carried again, so no audio
// lands in two FINALs. Finals are checked for repetition on the raw decode. A repetitive final
// is still sent, but it clears the prompt history instead of becoming context. The prompt
// history holds the last two finals that are at least 1 s long. A decode whose text occurs in the
// history may be the decoder copying its prompt, so its audio is decoded again with no history
// (text::echoes_history). If the second text is in the history too, the line was said again and
// is kept. Otherwise the two decodes disagree, and the audio decodes to nothing.
//
// Ongoing segment. Only the last range can be ongoing, and only when it ends within kTailMargin
// (0.5 s) of the window end, since the speaker may not be done. It is decoded for display: its
// text goes out as a PARTIAL with the id it will keep. Its audio, from its start, is carried into
// the next tick, so the next decode hears the whole segment with more context. The ongoing text is
// checked for repetition only under the rollback policy (below).
//
// Carry. When nothing is ongoing, the last kCarryTail (0.5 s) of the window is carried, so speech
// that starts at the very end is still heard. When a segment is ongoing, the carry starts at its
// start. Neither carry reaches back past the end of the latest final, because audio that a FINAL
// already covers is never carried.
//
// Forced cut. A segment that reaches max_segment_ms is cut, so decodes stay short. The cut applies
// to any range that reaches the limit, not only an ongoing one, because the streaming VADs do not
// split long speech. The limit counts from the segment's start, not from the window end, so audio
// that piled up behind a long segment cannot move the limit. The cut is at the latest pause of at
// least 250 ms in the segment, and without one at the quietest 100 ms block in the 3 s before the
// limit (stream::find_cut). The part before the cut becomes a FINAL. The rest is carried, and the
// next tick decodes it again, where it may be ongoing as a new segment with a new id.
//
// Seam. The segment that starts within kSeamMs (0.5 s) of a forced cut differs from other
// segments in two ways, in each of its decodes, PARTIAL and FINAL:
//   1. No history. The latest final can end mid-sentence at the cut, and a decoder given it as
//      history writes words from it for the audio after the cut. So the echo check is skipped too.
//   2. A cut can still split a word, and a decoder then writes that word on both sides of the cut.
//      So up to 3 leading words that repeat the last words of the FINAL before the cut are
//      dropped (text::drop_seam_repeat). Speech can repeat a word across a pause, so this never
//      applies at a VAD boundary, only at a forced cut.
// The seam is kept until the next segment ends. clear(), reset() and a failed live tick drop it,
// like the continuation.
//
// Ids and events. Segment ids count up from 1 and are not reused, not even after reset(). An
// ongoing segment keeps its id until it is final. A FINAL takes over the id of a PARTIAL that was
// shown, so the host replaces the PARTIAL. An empty FINAL is sent only to retract a shown PARTIAL.
//
// Stabilizing the ongoing text, agreement policy (text::Stabilizer). Each decode of the ongoing
// segment goes into its history, and the text shown is built in three steps:
//   1. Common prefix. The longest common prefix of the two latest earlier decodes, or of the one
//      earlier decode and the new one, is the part that decodes agree on.
//   2. Sticky prefix. A common prefix longer than the stable prefix replaces it. The stable prefix
//      does not shrink, unless step 3 drops it.
//   3. Anchor. Once the stable prefix has at least 10 code points, its last 10 code points must
//      occur in the new decode. The shown text is the stable prefix, then the new decode after the
//      last occurrence of the anchor. If the anchor is missing, the new decode contradicts the
//      lock, so the stable prefix is dropped and the new decode is shown as it is.
// A FINAL carries the newest cleaned decode of its segment, not the stabilized text. When a live
// tick fails, the segment ends with the newest decode it has, without another decode
// (abandon_window).
//
// Rollback policy. Each decode of the ongoing segment re-feeds the segment's whole audio, and the
// decoder is given the segment's previous output as a prefix, minus its last tokens. So the decoder
// keeps the start of the text and revises only its end, and the shown text needs no stabilizer.
// The scheme follows the streaming decoder of Qwen3-ASR (QwenLM/Qwen3-ASR, Apache-2.0), applied
// per VAD segment instead of to the whole stream.
//   1. The first unfixed_decodes decodes of a segment get no prefix. Each later decode gets the
//      continuation of the one before it (the backend's output, header included), minus
//      rollback_tokens tokens from its end.
//   2. The PARTIAL shows the cleaned text of the newest decode.
//   3. A FINAL is decoded with no prefix, whether its segment ends at a VAD boundary or at a forced
//      cut. The newest continuation came from a decode that heard audio past the segment's end,
//      so as a prefix it would pull the next segment's words into this one. After a forced cut the
//      remainder is a new segment, and its first decode gets no prefix. At the end of the input,
//      flush() and reset() send the newest decode as the FINAL, since no audio follows it.
//   4. A decode whose raw output is repetitive drops the continuation, so the next decode starts
//      over. The text of that decode is shown as usual.
//   5. clear() and reset() drop the continuation, and so does the end of a segment. A failed tick
//      leaves the state as it was, so its retry gets the same prefix. A failed live tick ends its
//      segment, as abandon_window does.
//
// Timestamps are computed from the exact sample position, so rounding does not add up across ticks.
// clear() bumps a generation counter, and text decoded from earlier audio is then dropped. A tick
// that starts after a clear() also drops the continuation and the seam, because clear() runs
// without the decode lock and cannot drop them itself. reset() flushes and then drops
// all audio and text state.

namespace echoir {
namespace {

using text::utf32_to_utf8;
using text::utf8_to_utf32;

// A range that ends within this many samples of the window end is ongoing, if it is the last one.
constexpr std::int64_t kTailMargin = 8000;
// Audio kept when nothing is ongoing, so speech that starts at the very end is still heard.
constexpr std::int64_t kCarryTail = 8000;
// A final shorter than 1 s is not kept as prompt history, because short clips often decode to
// hallucinated text.
constexpr std::int64_t kMinPromptSamples = 16000;
// The decoder gets at most this many of the latest finals as history.
constexpr std::size_t kPromptHistory = 2;
// A segment that starts this many ms or less after a forced cut is decoded as its seam segment.
constexpr std::int64_t kSeamMs = 500;
// A forced cut needs room for a quiet block, and a block is 100 ms, so the longest segment must be
// at least two blocks.
constexpr std::int64_t kMinMaxSegmentMs = 200;

SessionEvent make_event(echoir_event_type type, std::int64_t id, std::int64_t start_ms,
                        std::int64_t end_ms, std::string text) {
    SessionEvent ev;
    ev.type = type;
    ev.segment_id = id;
    ev.start_ms = start_ms;
    ev.end_ms = end_ms;
    ev.text = std::move(text);
    return ev;
}

// The session whose sink is running on this thread, or null. A session call made from a sink is
// refused before it can wait on a lock that the sink's own call still holds.
thread_local const void* t_sink_owner = nullptr;

// Marks the calling thread as inside the sink of one session, for the length of a sink call.
class SinkScope {
public:
    explicit SinkScope(const void* owner) noexcept : previous_(t_sink_owner) {
        t_sink_owner = owner;
    }
    ~SinkScope() { t_sink_owner = previous_; }
    SinkScope(const SinkScope&) = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    const void* previous_;
};

}  // namespace

std::optional<SessionEvent> Session::State::closing_event() const {
    if (open_id == 0) {
        return std::nullopt;
    }
    std::string text = utf32_to_utf8(open_latest);
    if (text.empty() && !open_shown) {
        return std::nullopt;
    }
    return make_event(ECHOIR_EVENT_FINAL, open_id, open_start_ms, open_end_ms, std::move(text));
}

void Session::State::close_open() {
    open_text.reset();
    open_latest.clear();
    open_id = 0;
    open_shown = false;
    drop_continuation();
}

void Session::State::drop_continuation() {
    open_decodes = 0;
    continuation.clear();
}

void Session::State::drop_seam() {
    seam_text.clear();
    seam_ms = -1;
}

Session::Session(IAsrBackend& backend, vad::IVad& vad, SessionConfig config, SessionSink sink)
    : backend_(backend), vad_(vad), config_(std::move(config)), sink_(std::move(sink)) {
    if (config_.min_chunk_ms < 0) {
        throw std::invalid_argument("min_chunk_ms must not be negative");
    }
    if (config_.max_segment_ms < kMinMaxSegmentMs) {
        throw std::invalid_argument("max_segment_ms must be at least 200");
    }
    if (config_.unfixed_decodes < 0) {
        throw std::invalid_argument("unfixed_decodes must not be negative");
    }
    if (config_.rollback_tokens < 0) {
        throw std::invalid_argument("rollback_tokens must not be negative");
    }
    if (config_.policy == StreamPolicy::backend_default) {
        config_.policy =
            backend.supports_prefix() ? StreamPolicy::rollback : StreamPolicy::agreement;
    }
    if (config_.policy == StreamPolicy::rollback && !backend.supports_prefix()) {
        throw std::invalid_argument(
            "the rollback policy needs a backend with decoder prefix support");
    }
    if (!sink_) {
        throw std::invalid_argument("the session needs an event sink");
    }
    min_chunk_samples_ = config_.min_chunk_ms * stream::kSamplesPerMs;
    max_segment_samples_ = config_.max_segment_ms * stream::kSamplesPerMs;
}

Session::~Session() {
    // The worker uses the members below, so it must be joined before they are destroyed. From
    // its own sink the worker cannot be joined, and the members cannot be kept alive for it, so
    // the process ends with a message instead of running on freed memory.
    if (t_sink_owner == this) {
        std::fputs("echoir: a session was destroyed from its own event sink\n", stderr);
        std::abort();
    }
    // Outside a sink, stop() can fail only in a lock or a join that the caller cannot act on.
    try {
        stop();
    } catch (...) {
    }
}

void Session::push(std::span<const float> interleaved, std::int32_t sample_rate,
                   std::int32_t channels) {
    if (sample_rate <= 0) {
        throw std::invalid_argument("sample rate must be positive");
    }
    if (channels <= 0) {
        throw std::invalid_argument("channel count must be positive");
    }
    if (interleaved.size() % static_cast<std::size_t>(channels) != 0) {
        throw std::invalid_argument("sample count is not a whole number of frames");
    }
    for (const float s : interleaved) {
        if (!std::isfinite(s)) {
            throw std::invalid_argument("samples must be finite");
        }
    }

    {
        std::lock_guard<std::mutex> audio(audio_mu_);
        if (!resampler_ || sample_rate != rate_ || channels != channels_) {
            // A format change starts a new resampler. The old one's tail is written out first, so
            // the last samples of the old format are not lost.
            if (resampler_) {
                resampler_->flush(pending_);
            }
            resampler_.emplace(static_cast<std::uint32_t>(sample_rate),
                               static_cast<std::uint32_t>(channels));
            rate_ = sample_rate;
            channels_ = channels;
        }
        resampler_->push(interleaved, pending_);
    }
    wake_worker();
}

void Session::process() {
    refuse_from_sink("process");
    std::lock_guard<std::mutex> decode(decode_mu_);
    if (mode_ == Mode::live) {
        throw StateError("process cannot run while the session is started");
    }
    mode_ = Mode::pull;
    run_tick(false, false);
}

void Session::flush() {
    refuse_from_sink("flush");
    std::lock_guard<std::mutex> decode(decode_mu_);
    drain_resampler();
    run_tick(true, false);
}

void Session::reset() {
    refuse_from_sink("reset");
    std::lock_guard<std::mutex> decode(decode_mu_);
    try {
        drain_resampler();
        run_tick(true, false);
    } catch (...) {
        // The caller asked for a fresh start, so the state goes even when the final decode fails.
        drop_state();
        throw;
    }
    drop_state();
}

void Session::start() {
    refuse_from_sink("start");
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (worker_.joinable()) {
        throw StateError("the session is already started");
    }
    std::uint64_t run = 0;
    Mode previous = Mode::unset;
    {
        std::lock_guard<std::mutex> decode(decode_mu_);
        if (mode_ == Mode::pull) {
            throw StateError("start cannot run after process; a session is either pulled or live");
        }
        previous = mode_;
        mode_ = Mode::live;
        run = run_generation_;
    }
    {
        std::lock_guard<std::mutex> worker(worker_mu_);
        worker_running_ = true;
        // Audio pushed before start is decoded at once.
        wake_ = true;
    }
    try {
        worker_ = std::thread(&Session::worker_loop, this, run);
    } catch (...) {
        {
            std::lock_guard<std::mutex> worker(worker_mu_);
            worker_running_ = false;
        }
        // No worker runs, so the session is not live. Its mode goes back as it was, and process()
        // stays available.
        std::lock_guard<std::mutex> decode(decode_mu_);
        mode_ = previous;
        throw;
    }
}

void Session::stop() {
    refuse_from_sink("stop");
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (worker_.joinable()) {
        {
            // The generation changes under decode_mu_, so a tick that is already running finishes
            // first, and every later tick of this run is skipped.
            std::lock_guard<std::mutex> decode(decode_mu_);
            ++run_generation_;
            std::lock_guard<std::mutex> worker(worker_mu_);
            worker_running_ = false;
        }
        worker_cv_.notify_all();
        worker_.join();
    }
    // A clear() on another thread runs its sink while it holds text_mu_, and nothing else waits
    // for that call. Taking the lock here waits for it to return before stop() does.
    std::lock_guard<std::mutex> text(text_mu_);
}

void Session::refuse_from_sink(const char* call) const {
    if (t_sink_owner == this) {
        throw StateError(std::string(call) + " cannot be called from a sink");
    }
}

void Session::worker_loop(std::uint64_t run) {
    while (true) {
        {
            std::unique_lock<std::mutex> worker(worker_mu_);
            worker_cv_.wait(worker, [this] { return wake_ || !worker_running_; });
            if (!worker_running_) {
                return;
            }
            wake_ = false;
        }
        // Ticks run back to back while they take audio. A tick that takes nothing ends the loop,
        // and the next push wakes the worker. A push during a tick sets wake_ again, so no audio
        // waits unnoticed.
        while (tick_worker(run)) {
        }
    }
}

bool Session::tick_worker(std::uint64_t run) noexcept {
    std::lock_guard<std::mutex> decode(decode_mu_);
    if (run != run_generation_) {
        return false;
    }
    try {
        return run_tick(false, true);
    } catch (...) {
        // run_tick has sent the ERROR event and dropped the failed window. The worker waits for
        // the next push, which brings new audio, so a failing backend neither spins the worker
        // nor decodes the same audio again.
        return false;
    }
}

void Session::wake_worker() {
    {
        std::lock_guard<std::mutex> worker(worker_mu_);
        if (!worker_running_) {
            return;
        }
        wake_ = true;
    }
    worker_cv_.notify_one();
}

void Session::clear() {
    refuse_from_sink("clear");
    std::lock_guard<std::mutex> text(text_mu_);
    ++generation_;
    SessionEvent ev;
    ev.type = ECHOIR_EVENT_CLEARED;
    emit_locked(ev);
}

bool Session::run_tick(bool finish, bool drop_on_failure) {
    // Read the generation before the audio is grabbed. A clear that lands after this point makes
    // the tick's text stale. Reading it after the grab would open a gap in which a clear lands
    // between the two reads and text from pre-clear audio still gets through.
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> text(text_mu_);
        generation = generation_;
    }

    // Snapshot the audio. Pushes can continue meanwhile, and they stay queued until this tick
    // commits.
    std::vector<float> window;
    std::size_t taken = 0;
    {
        std::lock_guard<std::mutex> audio(audio_mu_);
        taken = pending_.size();
        if (!finish && static_cast<std::int64_t>(taken) <= min_chunk_samples_) {
            return false;
        }
        window.reserve(carry_.size() + taken);
        window.insert(window.end(), carry_.begin(), carry_.end());
        window.insert(window.end(), pending_.begin(),
                      pending_.begin() + static_cast<std::ptrdiff_t>(taken));
    }
    if (window.empty()) {
        return false;
    }

    State next = state_;
    // clear() runs without the decode lock, so it cannot drop the continuation or the seam itself.
    // Both are dropped here instead when they are from before the clear, before anything decodes.
    if (next.decoded_generation != generation) {
        next.drop_continuation();
        next.drop_seam();
    }
    next.decoded_generation = generation;
    std::vector<SessionEvent> events;
    std::vector<float> current = std::move(window);
    std::int64_t origin = origin_samples_;
    std::vector<float> carry;
    std::size_t cut = 0;
    // The error is reported first. In live mode the window is then given up, so the next tick
    // starts from new audio and cannot fail on the same range again.
    const auto fail = [&](const std::string& message) {
        report_error(message);
        if (drop_on_failure) {
            abandon_window(taken, generation);
        }
    };
    try {
        while (true) {
            cut = analyze(current, finish, origin, next, events);
            if (!finish || cut >= current.size()) {
                break;
            }
            // A forced cut while finishing leaves a remainder that nothing else decodes. Analyze
            // it too. The cut is at least one sample in, so each pass shortens the window.
            origin += static_cast<std::int64_t>(cut);
            current.erase(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(cut));
        }
        carry.assign(current.begin() + static_cast<std::ptrdiff_t>(cut), current.end());
    } catch (const std::invalid_argument& e) {
        fail(e.what());
        throw;
    } catch (const std::bad_alloc& e) {
        fail(e.what());
        throw;
    } catch (const std::exception& e) {
        fail(e.what());
        throw BackendFailure(e.what());
    } catch (...) {
        fail("unknown failure in the decoder");
        throw BackendFailure("unknown failure in the decoder");
    }

    // Commit. Everything that can fail is done above, so a failed tick leaves no trace. The
    // steps below only move values. The carried audio, the time origin and the consumed pending
    // audio change together.
    state_ = std::move(next);
    origin_samples_ = origin + static_cast<std::int64_t>(cut);
    carry_ = std::move(carry);
    {
        std::lock_guard<std::mutex> audio(audio_mu_);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(taken));
    }
    deliver(events, generation);
    return taken > 0;
}

std::size_t Session::analyze(const std::vector<float>& window, bool finish, std::int64_t origin,
                             State& st, std::vector<SessionEvent>& events) {
    const auto len = static_cast<std::int64_t>(window.size());
    const std::vector<vad::Range> ranges = vad_.detect(window);
    const bool rollback = config_.policy == StreamPolicy::rollback;

    std::int64_t last_final_end = 0;  // window index where the latest final segment ends
    bool forced = false;
    bool tail_open = false;
    std::int64_t open_start = 0;  // window index where the ongoing segment starts
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        // The VAD reports milliseconds from the window start, so a sample index is ms * 16. Both
        // decode bounds are clamped to [0, len - 1]. The timestamp end is not clamped that way,
        // so a segment that runs to the window end is stamped with the true end.
        const std::int64_t start =
            std::clamp(ranges[i].start_ms * stream::kSamplesPerMs, std::int64_t{0}, len - 1);
        std::int64_t end =
            std::clamp(ranges[i].end_ms * stream::kSamplesPerMs, std::int64_t{0}, len - 1);
        if (end <= start) {
            continue;
        }
        std::int64_t end_time = std::min(ranges[i].end_ms * stream::kSamplesPerMs, len);

        const bool is_last = i + 1 == ranges.size();
        const bool at_tail = is_last && end >= len - kTailMargin;
        if (end - start >= max_segment_samples_) {
            forced = true;
            const std::int64_t cap = start + max_segment_samples_;
            end = static_cast<std::int64_t>(
                stream::find_cut(window, static_cast<std::size_t>(start),
                                 static_cast<std::size_t>(cap), static_cast<std::size_t>(end),
                                 stream::kCutSearchStream));
            end_time = end;
        }
        const bool ongoing = at_tail && !forced;
        const std::int64_t abs_start = (origin + start) / stream::kSamplesPerMs;
        const std::int64_t abs_end = (origin + end_time) / stream::kSamplesPerMs;

        if (st.prompts.size() > kPromptHistory) {
            st.prompts.erase(st.prompts.begin(),
                             st.prompts.end() - static_cast<std::ptrdiff_t>(kPromptHistory));
        }
        const bool at_seam =
            st.seam_ms >= 0 && abs_start >= st.seam_ms && abs_start <= st.seam_ms + kSeamMs;
        const std::vector<std::string> no_history;
        // Rollback: decode n of the ongoing segment continues from decode n-1, and the first
        // unfixed_decodes decodes start from nothing. A final is decoded alone, because the
        // continuation came from a decode that heard audio past the segment's end.
        DecodeRequest request{
            std::span<const float>(window.data() + start, static_cast<std::size_t>(end - start)),
            at_seam ? no_history : st.prompts, config_.language, config_.context};
        if (rollback && ongoing && st.open_decodes >= config_.unfixed_decodes) {
            request.prefix = st.continuation;
            request.rollback_tokens = config_.rollback_tokens;
        }
        DecodeResult decoded = backend_.decode(request);
        std::u32string raw = utf8_to_utf32(decoded.text);
        std::u32string clean = text::clean_segment(raw);
        // A decode that only repeats the history may be the decoder copying its prompt, so the
        // audio is decoded again without history. Speech that really repeats an earlier line
        // decodes to it again. Audio that decodes to something else holds no words the decoder
        // can agree on, such as noise or a fade-out, so it decodes to nothing.
        if (!at_seam && !st.prompts.empty() && text::echoes_history(clean, st.prompts)) {
            DecodeRequest retry{request.pcm, no_history, request.language, request.context,
                                request.prefix, request.rollback_tokens};
            decoded = backend_.decode(retry);
            raw = utf8_to_utf32(decoded.text);
            clean = text::clean_segment(raw);
            if (!text::echoes_history(clean, st.prompts)) {
                decoded = DecodeResult{};
                raw.clear();
                clean.clear();
            }
        }
        if (at_seam) {
            clean = text::drop_seam_repeat(clean, st.seam_text);
        }

        if (ongoing) {
            // The ongoing segment is shown as it grows, and the audio from its start is carried
            // into the next tick. Agreement shows the text stabilized against the earlier decodes.
            // Rollback shows the newest decode, since the prefix already keeps the earlier words.
            std::u32string shown = clean;
            if (!rollback) {
                shown = st.open_text.stabilize(clean);
            }
            st.open_latest = clean;
            if (st.open_id == 0) {
                st.open_id = st.next_id++;
            }
            st.open_start_ms = abs_start;
            st.open_end_ms = abs_end;
            ++st.open_decodes;
            if (rollback) {
                // A repetition loop is not kept as the prefix, or it would repeat in the next
                // decode. The next decode then starts over.
                if (text::is_repetitive(raw)) {
                    st.drop_continuation();
                } else {
                    st.continuation = decoded.continuation;
                }
            }
            tail_open = true;
            open_start = start;
            // A finishing tick sends the segment as FINAL below, so there is no PARTIAL to show.
            if (config_.emit_partials && !finish) {
                events.push_back(make_event(ECHOIR_EVENT_PARTIAL, st.open_id, abs_start, abs_end,
                                            utf32_to_utf8(shown)));
                st.open_shown = true;
            }
            continue;
        }

        // A repetition loop in the raw decode is not speech, so it clears the prompt history
        // instead of becoming context for the next decode.
        if (text::is_repetitive(raw)) {
            st.prompts.clear();
        } else if (!clean.empty() && end - start >= kMinPromptSamples) {
            st.prompts.push_back(utf32_to_utf8(clean));
        }

        // A final takes over the ongoing segment's id, so a PARTIAL the host shows is replaced.
        // An empty final is sent only to retract such a PARTIAL.
        std::string text = utf32_to_utf8(clean);
        if (!text.empty() || st.open_shown) {
            const std::int64_t id = st.open_id != 0 ? st.open_id : st.next_id++;
            events.push_back(
                make_event(ECHOIR_EVENT_FINAL, id, abs_start, abs_end, std::move(text)));
        }
        st.close_open();
        last_final_end = std::max(last_final_end, end);
        // The audio after a forced cut is left for the next call, which analyzes it again.
        if (forced) {
            st.seam_text = clean;
            st.seam_ms = abs_end;
            break;
        }
        st.drop_seam();
    }

    if (finish) {
        // Everything pending has been decoded, so the ongoing segment is final, with its newest
        // decode as the text.
        if (auto ev = st.closing_event()) {
            events.push_back(std::move(*ev));
        }
        st.close_open();
        if (!forced) {
            st.drop_seam();
        }
        // After a forced cut the remainder is not decoded yet, so return the cut and let the
        // caller analyze the rest.
        return forced ? static_cast<std::size_t>(last_final_end) : window.size();
    }

    // A forced cut ends the ongoing segment at the cut, and the audio after it is carried.
    if (forced) {
        return static_cast<std::size_t>(last_final_end);
    }
    if (tail_open) {
        return static_cast<std::size_t>(std::max(open_start, last_final_end));
    }
    // Nothing is ongoing. Keep the last 500 ms in case speech starts there, but never keep audio
    // that a final already covers, since it would be decoded twice.
    return static_cast<std::size_t>(std::max({len - kCarryTail, last_final_end, std::int64_t{0}}));
}

void Session::deliver(const std::vector<SessionEvent>& events, std::uint64_t generation) {
    std::lock_guard<std::mutex> text(text_mu_);
    for (const SessionEvent& ev : events) {
        const bool is_text = ev.type == ECHOIR_EVENT_PARTIAL || ev.type == ECHOIR_EVENT_FINAL;
        // Text decoded from audio grabbed before a clear is stale.
        if (is_text && generation != generation_) {
            continue;
        }
        emit_locked(ev);
    }
}

void Session::emit_locked(const SessionEvent& event) {
    const SinkScope scope(this);
    try {
        sink_(event);
    } catch (...) {
        // A sink must not unwind into the decoder. The event is lost, and the session goes on.
        std::fprintf(stderr, "echoir: an event sink threw; the event was dropped\n");
    }
}

void Session::report_error(const std::string& message) noexcept {
    try {
        SessionEvent ev;
        ev.type = ECHOIR_EVENT_ERROR;
        ev.detail = message;
        std::lock_guard<std::mutex> text(text_mu_);
        emit_locked(ev);
    } catch (...) {
        // Out of memory or a failed lock: the caller still gets the status from the throw.
    }
}

void Session::abandon_window(std::size_t taken, std::uint64_t generation) noexcept {
    try {
        // The ongoing segment is not decoded again. It ends with its newest decode, as a finish
        // would send it, but no decode runs here.
        std::vector<SessionEvent> events;
        if (auto ev = state_.closing_event()) {
            events.push_back(std::move(*ev));
        }
        {
            std::lock_guard<std::mutex> audio(audio_mu_);
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(taken));
        }
        // The window is carry_ followed by the taken audio. The next window starts after it.
        origin_samples_ += static_cast<std::int64_t>(carry_.size() + taken);
        carry_.clear();
        state_.close_open();
        state_.drop_seam();
        deliver(events, generation);
    } catch (...) {
        // Only an allocation or a failed lock gets here. When that happens before the state
        // changes, the window stays, and the next push decodes it again. The ERROR event has
        // already been sent either way.
    }
}

void Session::drain_resampler() {
    std::lock_guard<std::mutex> audio(audio_mu_);
    if (resampler_) {
        resampler_->flush(pending_);
        resampler_->reset();
    }
}

void Session::drop_state() {
    {
        std::lock_guard<std::mutex> audio(audio_mu_);
        pending_.clear();
    }
    const std::int64_t next_id = state_.next_id;
    state_ = State{};
    state_.next_id = next_id;
    carry_.clear();
    origin_samples_ = 0;
}

}  // namespace echoir
