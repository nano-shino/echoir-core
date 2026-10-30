// Session state machine, driven by a scripted backend and a scripted VAD, so no model is needed.
//
// Sample k of a session holds the value k. The fake backend reads the absolute position of each
// decode from its first sample and its length, and the fake VAD reads the window position the
// same way. Speech is scripted in absolute samples, and the VAD reports it in milliseconds from
// the window start, as the real VADs do. Every push is a whole number of milliseconds, so the
// ranges land on exact samples and the assertions can compare exact ranges.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/live_fakes.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "errors.h"
#include "stream/event_queue.h"
#include "stream/session.h"
#include "vad/vad.h"

namespace {

namespace ts = echoir::test_support;
using echoir::SessionConfig;
using echoir::SessionEvent;
using Range = std::pair<std::int64_t, std::int64_t>;  // absolute samples [first, second)

constexpr std::int64_t kMs = 16;  // samples per millisecond at 16 kHz
// Largest segment at the 20 s cap of cap_20s_config(), which the forced-cut tests use.
constexpr std::int64_t kMaxSegment = 320000;

// Audio whose sample k holds first + k. Exact in float for positions under 2^24 (about 17 min).
std::vector<float> ramp(std::int64_t first, std::int64_t count) {
    std::vector<float> out(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<float>(first + i);
    }
    return out;
}

class FakeBackend : public echoir::IAsrBackend {
public:
    struct Call {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::vector<std::string> history;
        std::string context;
    };

    // Text for a decode of samples [start, end). The default names the span.
    std::function<std::string(std::int64_t, std::int64_t)> text_for;
    // Runs inside decode, after the call is recorded. It may throw, or call the session.
    std::function<void(std::int64_t, std::int64_t)> before_return;
    std::vector<Call> calls;

    echoir::DecodeResult decode(const echoir::DecodeRequest& request) override {
        if (request.pcm.empty()) {
            throw std::logic_error("decode was called with no audio");
        }
        const auto start = static_cast<std::int64_t>(request.pcm.front());
        const auto end = start + static_cast<std::int64_t>(request.pcm.size());
        calls.push_back(Call{start, end, request.history, std::string(request.context)});
        if (before_return) {
            before_return(start, end);
        }
        const std::string text = text_for ? text_for(start, end) : span_text(start, end);
        return echoir::DecodeResult{text, text};  // no prefix support, so continuation is the text
    }

    std::string name() const override { return "fake"; }

    static std::string span_text(std::int64_t start, std::int64_t end) {
        return "[" + std::to_string(start) + "," + std::to_string(end) + ")";
    }
};

class FakeVad : public echoir::vad::IVad {
public:
    std::vector<Range> speech;              // absolute speech, in samples
    std::vector<std::size_t> window_sizes;  // one entry per detect call
    std::vector<float> last_window;         // the window of the latest detect call

    std::vector<echoir::vad::Range> detect(std::span<const float> pcm) override {
        window_sizes.push_back(pcm.size());
        last_window.assign(pcm.begin(), pcm.end());
        std::vector<echoir::vad::Range> out;
        if (speech.empty() || pcm.empty()) {
            return out;
        }
        const auto origin = static_cast<std::int64_t>(pcm[0]);
        const auto len = static_cast<std::int64_t>(pcm.size());
        if (origin % kMs != 0 || len % kMs != 0) {
            throw std::logic_error("test windows must start and end on whole milliseconds");
        }
        for (const Range& r : speech) {
            const std::int64_t lo = std::max<std::int64_t>(r.first - origin, 0);
            const std::int64_t hi = std::min<std::int64_t>(r.second - origin, len);
            if (hi <= lo) {
                continue;
            }
            if (lo % kMs != 0 || hi % kMs != 0) {
                throw std::logic_error("speech edges must fall on whole milliseconds");
            }
            out.push_back({lo / kMs, hi / kMs});
        }
        return out;
    }

    std::string name() const override { return "fake"; }
};

// A session wired to a backend and the fake VAD, with every event it sends collected in order.
template <typename Backend>
class RigOf {
public:
    explicit RigOf(SessionConfig config = {}) {
        session_ = std::make_unique<echoir::Session>(
            backend, vad, std::move(config),
            [this](const SessionEvent& ev) { events.push_back(ev); });
    }

    // Pushes the next `samples` samples of the timeline, which is absolute until restart().
    void push_samples(std::int64_t samples) {
        session_->push(ramp(next_, samples), 16000, 1);
        next_ += samples;
    }

    void push_ms(std::int64_t ms) { push_samples(ms * kMs); }

    void restart_timeline() { next_ = 0; }
    echoir::Session& session() { return *session_; }

    Backend backend;
    FakeVad vad;
    std::vector<SessionEvent> events;

private:
    std::int64_t next_ = 0;
    std::unique_ptr<echoir::Session> session_;  // last, so it is destroyed before the fakes
};

using Rig = RigOf<FakeBackend>;
using PrefixRig = RigOf<ts::PrefixBackend>;
using Answer = ts::PrefixBackend::Answer;

// The default settings with the rollback policy.
SessionConfig rollback_config() {
    SessionConfig config;
    config.policy = echoir::StreamPolicy::rollback;
    return config;
}

// The default settings with a 20 s segment cap. The forced-cut tests below measure their cut
// points at this cap, so they set it rather than rely on the default.
SessionConfig cap_20s_config() {
    SessionConfig config;
    config.max_segment_ms = 20000;
    return config;
}

// Pushes one second of audio and runs a tick, `seconds` times. Each tick takes all of its audio.
template <typename RigType>
void run_seconds(RigType& rig, int seconds) {
    for (int i = 0; i < seconds; ++i) {
        rig.push_ms(1000);
        rig.session().process();
    }
}

std::vector<Range> final_ranges(const std::vector<SessionEvent>& events) {
    std::vector<Range> out;
    for (const SessionEvent& ev : events) {
        if (ev.type != ECHOIR_EVENT_FINAL) {
            continue;
        }
        // FINAL text from the fake backend is "[first,second)".
        const std::size_t comma = ev.text.find(',');
        out.push_back({std::stoll(ev.text.substr(1, comma - 1)),
                       std::stoll(ev.text.substr(comma + 1, ev.text.size() - comma - 2))});
    }
    return out;
}

std::size_t count_type(const std::vector<SessionEvent>& events, echoir_event_type type) {
    std::size_t n = 0;
    for (const SessionEvent& ev : events) {
        n += ev.type == type ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("session: no sample range is decoded twice as FINAL, and FINALs are monotonic") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}, {64000, 96000}, {112000, 152000}};  // 0.5-2.5, 4-6, 7-9.5 s
    for (int i = 0; i < 24; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();

    const std::vector<Range> finals = final_ranges(rig.events);
    CHECK(finals == std::vector<Range>{{8000, 40000}, {64000, 96000}, {112000, 152000}});
    for (std::size_t k = 1; k < finals.size(); ++k) {
        CHECK(finals[k].first >= finals[k - 1].second);
    }
    // The open segment is shown as PARTIAL while it grows, under one id.
    CHECK(count_type(rig.events, ECHOIR_EVENT_PARTIAL) > 0);
}

TEST_CASE("session: a forced cut ends a long segment at 20 s, anchored at its start") {
    Rig rig(cap_20s_config());
    rig.vad.speech = {{0, 720000}};  // 45 s of speech with no pause
    for (int i = 0; i < 94; ++i) {   // then 2 s of silence
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();

    const std::vector<Range> finals = final_ranges(rig.events);
    REQUIRE(finals.size() >= 3);
    // The first cap is 320000, and the search covers the 3 s before it, [272000, 320000). The
    // signal rises over time, so the first block is the quietest, and the cut lands 800 samples
    // into it.
    CHECK(finals.front() == Range{0, 272800});
    for (std::size_t k = 0; k < finals.size(); ++k) {
        CHECK(finals[k].second - finals[k].first <= kMaxSegment);
        if (k > 0) {
            CHECK(finals[k].first == finals[k - 1].second);
        }
    }
    CHECK(finals.back().second == 720000);
    // Every decode of this speech is shorter than the limit: the open tail is below the cap, and
    // every forced cut lands before it.
    for (const FakeBackend::Call& call : rig.backend.calls) {
        CHECK(call.end - call.start < kMaxSegment);
    }
}

TEST_CASE("session: a tail segment that reaches the limit exactly is cut, not decoded whole") {
    Rig rig(cap_20s_config());
    rig.vad.speech = {{0, 320000}};  // exactly 20 s, running to 0.5 s before the window end
    rig.push_ms(20001);
    rig.session().process();

    // The search covers the 3 s before the cap, [272000, 318400), which ends a quiet block before
    // the segment end.
    CHECK(final_ranges(rig.events) == std::vector<Range>{{0, 272800}});
    CHECK(count_type(rig.events, ECHOIR_EVENT_PARTIAL) == 0);
    for (const FakeBackend::Call& call : rig.backend.calls) {
        CHECK(call.end - call.start < kMaxSegment);
    }
}

TEST_CASE("session: a forced cut is anchored at segment start, and the remainder is decoded") {
    // The segment starts at 1 s, not at the window origin, so a cap anchored at the window
    // origin would cut at a different place.
    Rig rig(cap_20s_config());
    rig.vad.speech = {{0, 8000}, {16000, 360000}};  // 0-0.5 s, then 1-22.5 s to the window end
    rig.push_ms(22500);
    rig.session().process();  // the first forced cut happens here, in a tick that is not finishing
    rig.session().flush();    // the remainder after the cut is decoded here

    // The cap is 16000 + 320000 = 336000, and the search covers [288000, 336000). The cut is at
    // 288000 + 800, as in the first case.
    const std::vector<Range> finals = final_ranges(rig.events);
    CHECK(finals == std::vector<Range>{{0, 8000}, {16000, 288800}, {288800, 359999}});

    // The cut ends the segment, so the remainder starts a new one with a new id.
    std::vector<std::int64_t> ids;
    for (const SessionEvent& ev : rig.events) {
        if (ev.type == ECHOIR_EVENT_FINAL) {
            ids.push_back(ev.segment_id);
        }
    }
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] != ids[1]);
    CHECK(ids[1] != ids[2]);
    CHECK(ids[0] != ids[2]);
}

TEST_CASE("session: a flush decodes the audio that follows a forced cut") {
    // Same speech, but the cut is made by the flush itself, so nothing else would decode the
    // remainder.
    Rig rig(cap_20s_config());
    rig.vad.speech = {{0, 8000}, {16000, 360000}};
    rig.push_ms(22500);
    rig.session().flush();

    CHECK(final_ranges(rig.events) ==
          std::vector<Range>{{0, 8000}, {16000, 288800}, {288800, 359999}});
    CHECK(rig.events.back().end_ms == 22500);
}

TEST_CASE("session: a long range that is not the last is cut at the limit too") {
    // The VAD does not split long speech, so the 21 s range is not cut by the VAD. It is closed by
    // silence, and a second range follows it in the same window.
    Rig rig(cap_20s_config());
    rig.vad.speech = {{0, 336000}, {352000, 360000}};  // 0-21 s, then 22-22.5 s
    rig.push_ms(25000);
    rig.session().process();  // the cut ends this call's analysis, so the rest is carried
    rig.session().flush();

    CHECK(final_ranges(rig.events) ==
          std::vector<Range>{{0, 272800}, {272800, 336000}, {352000, 360000}});
}

// Text for the speech of the forced-cut cases: the segment from 1 s ends with "we", and the
// remainder after its cut at 288800 starts with it again.
std::string seam_text(std::int64_t start, std::int64_t end) {
    if (start == 16000) {
        return "We drove down the road, we.";
    }
    if (start == 288800) {
        return "We spent the night.";
    }
    return FakeBackend::span_text(start, end);
}

std::vector<std::string> texts_of(const std::vector<SessionEvent>& events, echoir_event_type type) {
    std::vector<std::string> out;
    for (const SessionEvent& ev : events) {
        if (ev.type == type) {
            out.push_back(ev.text);
        }
    }
    return out;
}

TEST_CASE("session: the segment after a forced cut drops the words that repeat the cut FINAL") {
    Rig rig(cap_20s_config());
    rig.backend.text_for = seam_text;
    rig.vad.speech = {{0, 8000}, {16000, 400000}};
    rig.push_ms(22500);
    rig.session().process();  // cut at 288800
    rig.push_ms(1000);
    rig.session().process();  // the remainder is ongoing
    rig.session().flush();

    CHECK(texts_of(rig.events, ECHOIR_EVENT_PARTIAL) == std::vector<std::string>{"spent the night."});
    CHECK(texts_of(rig.events, ECHOIR_EVENT_FINAL) ==
          std::vector<std::string>{"[0,8000)", "We drove down the road, we.", "spent the night."});
}

TEST_CASE("session: a word repeated across a VAD boundary is kept") {
    Rig rig;
    rig.backend.text_for = [](std::int64_t start, std::int64_t) {
        return std::string(start == 8000 ? "No," : "no, stop.");
    };
    rig.vad.speech = {{8000, 40000}, {48000, 80000}};  // 0.5 s apart
    rig.push_ms(6000);
    rig.session().process();
    rig.session().flush();

    CHECK(texts_of(rig.events, ECHOIR_EVENT_FINAL) == std::vector<std::string>{"No,", "no, stop."});
}

TEST_CASE("session: a clear after a forced cut drops the seam") {
    Rig rig(cap_20s_config());
    rig.backend.text_for = seam_text;
    rig.vad.speech = {{0, 8000}, {16000, 360000}};
    rig.push_ms(22500);
    rig.session().process();  // cut at 288800
    rig.session().clear();
    rig.session().flush();

    const std::vector<std::string> finals = texts_of(rig.events, ECHOIR_EVENT_FINAL);
    REQUIRE(finals.size() == 3);
    CHECK(finals[2] == "We spent the night.");
}

TEST_CASE("session: a partial and its final share an id, and the next segment gets a new one") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}};  // 0.5-2.5 s, still running at the first tick
    rig.push_ms(2000);
    rig.session().process();  // open: PARTIAL
    rig.push_ms(2000);
    rig.session().process();                           // closed by silence: FINAL, same id
    rig.vad.speech = {{8000, 40000}, {48000, 96000}};  // 3-6 s: a new segment, still open
    rig.push_ms(2000);
    rig.session().process();
    rig.session().flush();

    REQUIRE(rig.events.size() >= 3);
    CHECK(rig.events[0].type == ECHOIR_EVENT_PARTIAL);
    CHECK(rig.events[1].type == ECHOIR_EVENT_FINAL);
    CHECK(rig.events[1].text == "[8000,40000)");
    CHECK(rig.events[1].segment_id == rig.events[0].segment_id);
    CHECK(rig.events[2].type == ECHOIR_EVENT_PARTIAL);
    CHECK(rig.events[2].segment_id != rig.events[0].segment_id);
}

TEST_CASE("session: timestamps stay exact when ticks are not whole 16-sample multiples") {
    // Silent ticks of 8001 samples each leave the time origin 1 sample off a millisecond
    // boundary. Truncating the origin to ms on every tick would drift a whole ms by now.
    Rig rig;
    for (int i = 0; i < 16; ++i) {
        rig.push_samples(8001);
        rig.session().process();
    }
    // The origin is now 120016 samples, which is a whole number of ms, so the VAD can speak in ms.
    rig.vad.speech = {{128016 + 1600, 128016 + 6400}};
    rig.push_samples(16000);
    rig.session().process();

    REQUIRE(count_type(rig.events, ECHOIR_EVENT_FINAL) == 1);
    const SessionEvent& ev = rig.events.back();
    CHECK(ev.start_ms == 8101);
    CHECK(ev.end_ms == 8401);
}

TEST_CASE("session: a clear during a tick drops the text that tick decoded") {
    Rig rig;
    rig.vad.speech = {{8000, 24000}};  // 0.5-1.5 s
    rig.push_ms(2000);

    bool cleared = false;
    rig.backend.before_return = [&](std::int64_t, std::int64_t) {
        if (!cleared) {
            cleared = true;
            rig.session().clear();  // runs on the same thread, while the tick is decoding
        }
    };
    rig.session().process();
    // The range runs to the window end, so the tick decoded it as the open segment and queued a
    // PARTIAL. The PARTIAL was decoded before the clear, so it must not reach the host.
    REQUIRE(rig.events.size() == 1);
    CHECK(rig.events[0].type == ECHOIR_EVENT_CLEARED);
}

TEST_CASE("session: a clear during the decode of a finished range drops its FINAL") {
    Rig rig;
    rig.vad.speech = {{8000, 24000}};  // 0.5-1.5 s, closed by silence before the window end
    rig.push_ms(3000);

    bool cleared = false;
    rig.backend.before_return = [&](std::int64_t, std::int64_t) {
        if (!cleared) {
            cleared = true;
            rig.session().clear();
        }
    };
    rig.session().process();
    REQUIRE(rig.backend.calls.size() == 1);  // the closed range was decoded as a FINAL
    REQUIRE(rig.events.size() == 1);
    CHECK(rig.events[0].type == ECHOIR_EVENT_CLEARED);

    // The range was finalized, so it is not carried and is never sent again. Speech after the
    // clear still comes through as usual.
    rig.backend.before_return = nullptr;
    rig.vad.speech = {{8000, 24000}, {56000, 72000}};  // 3.5-4.5 s
    rig.push_ms(2000);
    rig.session().process();
    rig.session().flush();
    CHECK(final_ranges(rig.events) == std::vector<Range>{{56000, 72000}});
    CHECK(count_type(rig.events, ECHOIR_EVENT_CLEARED) == 1);
}

TEST_CASE("session: reset commits the open segment, then restarts the timeline at 0") {
    Rig rig;
    rig.vad.speech = {{8000, 400000}};  // from 0.5 s, far past the end of this test
    rig.push_ms(2000);
    rig.session().process();
    rig.session().reset();

    REQUIRE(!rig.events.empty());
    const SessionEvent& last = rig.events.back();
    CHECK(last.type == ECHOIR_EVENT_FINAL);
    CHECK(last.segment_id == 1);
    CHECK(last.text == "[8000,31999)");  // the decode stops one sample short of the window end
    CHECK(last.start_ms == 500);
    CHECK(last.end_ms == 2000);

    // The next audio is timed from 0, and segment ids keep counting up.
    rig.vad.speech = {{4000, 12000}};
    rig.restart_timeline();
    rig.push_ms(1500);
    rig.session().flush();
    const SessionEvent& after = rig.events.back();
    CHECK(after.type == ECHOIR_EVENT_FINAL);
    CHECK(after.segment_id == 2);
    CHECK(after.text == "[4000,12000)");
    CHECK(after.start_ms == 250);
    CHECK(after.end_ms == 750);
}

TEST_CASE("session: a repetitive decode clears the decoder history") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}, {64000, 96000}, {112000, 144000}, {160000, 192000}};
    rig.backend.text_for = [](std::int64_t start, std::int64_t) -> std::string {
        switch (start) {
            case 8000:
                return "alpha one two";
            case 64000:
                return "bravo three four";
            case 112000:
                return "thank you. thank you. thank you. thank you. ";  // a loop
            default:
                return "delta five six";
        }
    };
    for (int i = 0; i < 25; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();

    // Each final's decode, checked against the history the decoder was given.
    const std::vector<std::string> expected_text = {"alpha one two", "bravo three four",
                                                    "thank you. thank you. thank you. thank you. ",
                                                    "delta five six"};
    std::vector<std::string> texts;
    for (const SessionEvent& ev : rig.events) {
        if (ev.type == ECHOIR_EVENT_FINAL) {
            texts.push_back(ev.text);
        }
    }
    CHECK(texts == expected_text);

    for (const FakeBackend::Call& call : rig.backend.calls) {
        if (call.start == 64000 && call.end == 96000) {
            CHECK(call.history == std::vector<std::string>{"alpha one two"});
        } else if (call.start == 112000 && call.end == 144000) {
            CHECK(call.history == std::vector<std::string>{"alpha one two", "bravo three four"});
        } else if (call.start == 160000 && call.end == 192000) {
            // The loop before it cleared the history, and the loop itself was not added.
            CHECK(call.history.empty());
        }
    }
}

TEST_CASE("session: a decode that copies the history, and differs without it, is dropped") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}, {64000, 96000}, {112000, 144000}};
    // The third segment copies its prompt while it has one, as Qwen3-ASR does on a fade-out, and
    // decodes to an unrelated guess without it.
    rig.backend.text_for = [&rig](std::int64_t start, std::int64_t) -> std::string {
        switch (start) {
            case 8000:
                return "Or am I wrong?";
            case 64000:
                return "Understood, sir.";
            default:
                return rig.backend.calls.back().history.empty() ? "Okay."
                                                                : "Or am I wrong? Understood, sir.";
        }
    };
    for (int i = 0; i < 25; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();

    std::vector<std::string> texts;
    for (const SessionEvent& ev : rig.events) {
        CHECK(ev.text != "Or am I wrong? Understood, sir.");
        CHECK(ev.text != "Okay.");
        if (ev.type == ECHOIR_EVENT_FINAL && !ev.text.empty()) {
            texts.push_back(ev.text);
        }
    }
    CHECK(texts == std::vector<std::string>{"Or am I wrong?", "Understood, sir."});
    // The dropped segment was decoded again with no history.
    bool retried = false;
    for (const FakeBackend::Call& call : rig.backend.calls) {
        retried = retried || (call.start == 112000 && call.history.empty());
    }
    CHECK(retried);
    // A decode whose text is not in its history is not repeated, so it keeps that history.
    for (const FakeBackend::Call& call : rig.backend.calls) {
        if (call.start == 64000) {
            CHECK(call.history == std::vector<std::string>{"Or am I wrong?"});
        }
    }
}

TEST_CASE("session: a line said again is kept, since it decodes the same without history") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}, {64000, 96000}};
    rig.backend.text_for = [](std::int64_t, std::int64_t) -> std::string { return "Yes, sir."; };
    for (int i = 0; i < 15; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();

    std::vector<std::string> texts;
    for (const SessionEvent& ev : rig.events) {
        if (ev.type == ECHOIR_EVENT_FINAL) {
            texts.push_back(ev.text);
        }
    }
    CHECK(texts == std::vector<std::string>{"Yes, sir.", "Yes, sir."});
}

TEST_CASE("session: repetition is judged on the raw decode, before cleaning") {
    Rig rig;
    rig.vad.speech = {{8000, 24000}, {40000, 56000}, {64000, 72000}};
    rig.backend.text_for = [](std::int64_t start, std::int64_t) -> std::string {
        if (start == 8000) {
            return "alpha one two";
        }
        if (start == 40000) {
            // Raw, this is a runaway run. Cleaned, it collapses to one character, which is not.
            return std::string(20, '!');
        }
        return "delta four five";
    };
    rig.push_ms(6000);
    rig.session().flush();

    // The runaway decode clears the history, so the decode after it sees no prompt at all. If the
    // check ran on the cleaned text, "!" would be added and the history would not be empty.
    bool found = false;
    for (const FakeBackend::Call& call : rig.backend.calls) {
        if (call.start == 64000) {
            found = true;
            CHECK(call.history.empty());
        }
    }
    CHECK(found);
}

TEST_CASE("session: a failed tick emits nothing, and the retry emits each final once") {
    Rig rig;
    rig.vad.speech = {{8000, 40000}, {64000, 96000}};
    rig.push_ms(8000);  // both segments close inside one tick

    bool failed = false;
    rig.backend.before_return = [&](std::int64_t start, std::int64_t end) {
        if (start == 64000 && end == 96000 && !failed) {
            failed = true;
            throw std::runtime_error("decoder broke");
        }
    };
    CHECK_THROWS_AS(rig.session().process(), echoir::BackendFailure);
    // The first segment was decoded, but its FINAL is held back with the rest of the failed tick.
    REQUIRE(rig.events.size() == 1);
    CHECK(rig.events[0].type == ECHOIR_EVENT_ERROR);
    CHECK(rig.events[0].detail == "decoder broke");

    rig.session().process();  // the audio is still pending, so this retries the same tick
    CHECK(final_ranges(rig.events) == std::vector<Range>{{8000, 40000}, {64000, 96000}});
}

TEST_CASE("session: process does nothing until more than min_chunk_ms is pending") {
    Rig rig;
    rig.push_ms(500);  // exactly min_chunk_ms: not enough
    rig.session().process();
    CHECK(rig.vad.window_sizes.empty());

    rig.push_samples(16);  // one more millisecond is more than min_chunk_ms
    rig.session().process();
    CHECK(rig.vad.window_sizes == std::vector<std::size_t>{8016});
}

TEST_CASE("session: partials can be turned off") {
    SessionConfig config;
    config.emit_partials = false;
    Rig rig(config);
    rig.vad.speech = {{8000, 40000}};
    for (int i = 0; i < 8; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    CHECK(count_type(rig.events, ECHOIR_EVENT_PARTIAL) == 0);
    CHECK(final_ranges(rig.events) == std::vector<Range>{{8000, 40000}});
}

TEST_CASE("session: the configured context goes with every decode") {
    SessionConfig config;
    config.context = "Vocabulary: Zorvath, Quimby";
    Rig rig(config);
    rig.vad.speech = {{8000, 40000}, {64000, 96000}};
    for (int i = 0; i < 12; ++i) {
        rig.push_ms(500);
        rig.session().process();
    }
    rig.session().flush();
    REQUIRE_FALSE(rig.backend.calls.empty());
    for (const FakeBackend::Call& call : rig.backend.calls) {
        CHECK(call.context == "Vocabulary: Zorvath, Quimby");
    }
}

TEST_CASE("session: a sink that throws does not stop the session") {
    FakeBackend backend;
    FakeVad vad;
    vad.speech = {{8000, 40000}};
    int seen = 0;
    echoir::Session session(backend, vad, SessionConfig{}, [&seen](const SessionEvent&) {
        ++seen;
        throw std::runtime_error("sink failure");
    });
    session.push(ramp(0, 48000), 16000, 1);
    CHECK_NOTHROW(session.process());
    CHECK_NOTHROW(session.flush());
    CHECK(seen > 0);
    CHECK(!backend.calls.empty());
}

TEST_CASE("session: bad configuration and bad pushes are rejected") {
    FakeBackend backend;
    FakeVad vad;
    const auto sink = [](const SessionEvent&) {};

    SessionConfig tiny_segment;
    tiny_segment.max_segment_ms = 100;
    CHECK_THROWS_AS(echoir::Session(backend, vad, tiny_segment, sink), std::invalid_argument);

    SessionConfig negative_chunk;
    negative_chunk.min_chunk_ms = -1;
    CHECK_THROWS_AS(echoir::Session(backend, vad, negative_chunk, sink), std::invalid_argument);

    echoir::Session session(backend, vad, SessionConfig{}, sink);
    const std::vector<float> samples(32, 0.0f);
    CHECK_THROWS_AS(session.push(samples, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(session.push(samples, 16000, 0), std::invalid_argument);
    CHECK_THROWS_AS(session.push(samples, 16000, 3),
                    std::invalid_argument);  // 32 is not whole frames

    std::vector<float> nan_samples(16, 0.0f);
    nan_samples[5] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(session.push(nan_samples, 16000, 1), std::invalid_argument);
}

TEST_CASE("session: stereo at 48 kHz is downmixed to mono and resampled to 16 kHz") {
    Rig rig;
    // One second of stereo with a different constant on each channel. Their average is 0.125, so
    // keeping one channel, or summing them, gives a value that the check below rejects.
    std::vector<float> stereo(48000 * 2);
    for (std::size_t i = 0; i < stereo.size(); i += 2) {
        stereo[i] = 0.5f;
        stereo[i + 1] = -0.25f;
    }
    rig.session().push(stereo, 48000, 2);
    rig.session().flush();
    REQUIRE(!rig.vad.window_sizes.empty());
    CHECK(rig.vad.window_sizes.back() >= 15968);
    CHECK(rig.vad.window_sizes.back() <= 16032);
    // The middle of the window is far from the filter's start-up and tail, so it holds the mix.
    const std::vector<float>& window = rig.vad.last_window;
    REQUIRE(window.size() > 100);
    CHECK(std::fabs(window[window.size() / 2] - 0.125f) < 1e-3f);
}

// The rollback tests push 1 s per tick, so every tick takes audio. An ongoing decode stops one
// sample short of the window end, so the decodes of a segment that starts at 0.5 s end at 15999,
// 31999, 47999, and so on.

TEST_CASE("rollback: decodes 0 and 1 of a segment get no prefix, decode 2 continues decode 1") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};  // from 0.5 s, past the end of this test
    run_seconds(rig, 4);
    const std::vector<ts::PrefixBackend::Call>& calls = rig.backend.calls;
    REQUIRE(calls.size() == 4);
    CHECK(calls[0].start == 8000);
    CHECK(calls[0].end == 15999);
    CHECK(calls[0].prefix.empty());
    CHECK(calls[1].end == 31999);
    CHECK(calls[1].prefix.empty());
    CHECK(calls[2].end == 47999);
    CHECK(calls[2].prefix == "[8000,31999)");  // decode 1's continuation
    CHECK(calls[2].rollback_tokens == 5);
    CHECK(calls[3].end == 63999);
    CHECK(calls[3].prefix == "[8000,47999)");
    CHECK(calls[3].rollback_tokens == 5);
}

TEST_CASE("backend_default is rollback with prefix support, and agreement without it") {
    SessionConfig config;
    config.policy = echoir::StreamPolicy::backend_default;
    for (const bool support : {true, false}) {
        INFO("prefix support " << support);
        // The policy is resolved when the session is made, so the backend is set up first.
        ts::PrefixBackend backend;
        backend.prefix_support = support;
        FakeVad vad;
        vad.speech = {{8000, 400000}};
        echoir::Session session(backend, vad, config, [](const SessionEvent&) {});
        for (int i = 0; i < 3; ++i) {
            session.push(ramp(i * 16000, 16000), 16000, 1);
            session.process();
        }
        const std::vector<ts::PrefixBackend::Call>& calls = backend.calls;
        REQUIRE(calls.size() == 3);
        CHECK(calls[2].prefix.empty() == !support);
    }
}

// The newest continuation heard audio past the VAD end, so the final decode must not reuse it.
TEST_CASE(
    "rollback: a segment that ends at a VAD boundary is decoded for its FINAL with no prefix") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 40000}};  // 0.5-2.5 s
    run_seconds(rig, 3);               // three ongoing decodes
    rig.push_ms(1000);                 // the speech has ended by now, so the segment is final
    rig.session().process();

    const std::vector<ts::PrefixBackend::Call>& calls = rig.backend.calls;
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].prefix == "[8000,31999)");
    CHECK(calls[3].start == 8000);
    CHECK(calls[3].end == 40000);
    CHECK(calls[3].prefix.empty());
    CHECK(calls[3].rollback_tokens == 0);
    CHECK(final_ranges(rig.events) == std::vector<Range>{{8000, 40000}});
}

TEST_CASE("rollback: a forced cut gets no prefix, and the remainder starts at decode 0") {
    SessionConfig config = rollback_config();
    config.max_segment_ms = 20000;
    PrefixRig rig(config);
    rig.vad.speech = {{0, 720000}};  // 45 s of speech with no pause
    run_seconds(rig, 25);
    rig.session().flush();

    const std::vector<ts::PrefixBackend::Call>& calls = rig.backend.calls;
    const std::vector<Range> finals = final_ranges(rig.events);
    REQUIRE(!finals.empty());
    CHECK(finals.front() == Range{0, 272800});

    // The ongoing decode of the 20 s before the cap had a prefix. The cut itself is decoded alone.
    const ts::PrefixBackend::Call* before = nullptr;
    const ts::PrefixBackend::Call* cut = nullptr;
    for (const ts::PrefixBackend::Call& call : calls) {
        if (call.start == 0 && call.end == 319999) {
            before = &call;
        }
        if (call.start == 0 && call.end == 272800) {
            cut = &call;
        }
    }
    REQUIRE(before != nullptr);
    REQUIRE(cut != nullptr);
    CHECK(!before->prefix.empty());
    CHECK(cut->prefix.empty());
    CHECK(cut->rollback_tokens == 0);

    // The remainder is a new segment, so its first decode gets no prefix.
    const ts::PrefixBackend::Call* first_rest = nullptr;
    for (const ts::PrefixBackend::Call& call : calls) {
        if (call.start == 272800) {
            first_rest = &call;
            break;
        }
    }
    REQUIRE(first_rest != nullptr);
    CHECK(first_rest->prefix.empty());
}

TEST_CASE("rollback: the PARTIAL shows the newest decode, not a stabilized text") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};
    int decodes = 0;
    rig.backend.answer_for = [&decodes](std::int64_t, std::int64_t) -> Answer {
        ++decodes;
        if (decodes == 1) {
            return {"alpha beta", "alpha beta"};
        }
        if (decodes == 2) {
            return {"alpha gamma", "alpha gamma"};
        }
        return {"delta epsilon", "delta epsilon"};
    };
    run_seconds(rig, 3);
    std::vector<std::string> partials;
    for (const SessionEvent& ev : rig.events) {
        if (ev.type == ECHOIR_EVENT_PARTIAL) {
            partials.push_back(ev.text);
        }
    }
    CHECK(partials == std::vector<std::string>{"alpha beta", "alpha gamma", "delta epsilon"});
}

TEST_CASE("rollback: a repetitive ongoing decode drops the continuation, so decode 0 follows") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};
    const std::string loop = "thank you. thank you. thank you. thank you. ";
    int decodes = 0;
    rig.backend.answer_for = [&decodes, loop](std::int64_t, std::int64_t end) -> Answer {
        ++decodes;
        if (decodes == 2) {
            return {loop, loop};  // the second decode loops
        }
        const std::string text = "[8000," + std::to_string(end) + ")";
        return {text, text};
    };
    run_seconds(rig, 5);
    const std::vector<ts::PrefixBackend::Call>& calls = rig.backend.calls;
    REQUIRE(calls.size() == 5);
    CHECK(calls[1].end == 31999);
    // The loop is decode 1. The decodes after it count from 0 again, so decodes 2 and 3 get no
    // prefix, and decode 4 continues decode 3.
    CHECK(calls[2].prefix.empty());
    CHECK(calls[3].prefix.empty());
    CHECK(calls[4].prefix == "[8000,63999)");
    CHECK(calls[4].rollback_tokens == 5);
}

TEST_CASE("rollback: clear() drops the continuation, so the next decode starts over") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};
    run_seconds(rig, 3);
    REQUIRE(rig.backend.calls.size() == 3);
    REQUIRE(!rig.backend.calls[2].prefix.empty());

    rig.session().clear();
    rig.push_ms(1000);
    rig.session().process();
    REQUIRE(rig.backend.calls.size() == 4);
    CHECK(rig.backend.calls[3].end == 63999);
    CHECK(rig.backend.calls[3].prefix.empty());
    CHECK(rig.events.back().type == ECHOIR_EVENT_PARTIAL);
}

TEST_CASE("rollback: reset() drops the continuation, and the next segment starts at decode 0") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};
    run_seconds(rig, 3);
    rig.session().reset();  // the open segment is decoded once more, then all state is dropped
    REQUIRE(rig.backend.calls.size() == 4);
    CHECK(rig.events.back().type == ECHOIR_EVENT_FINAL);
    CHECK(rig.events.back().text == "[8000,47999)");

    rig.vad.speech = {{4000, 12000}};
    rig.restart_timeline();
    rig.push_ms(1500);
    rig.session().process();  // closed by silence, so it is decoded as a FINAL
    REQUIRE(rig.backend.calls.size() == 5);
    CHECK(rig.backend.calls[4].start == 4000);
    CHECK(rig.backend.calls[4].prefix.empty());
}

TEST_CASE("rollback: a failed tick keeps its prefix, so the retry sends the same one") {
    PrefixRig rig(rollback_config());
    rig.vad.speech = {{8000, 400000}};
    int decodes = 0;
    rig.backend.answer_for = [&decodes](std::int64_t, std::int64_t end) -> Answer {
        ++decodes;
        if (decodes == 3) {
            throw std::runtime_error("decoder broke");
        }
        const std::string text = "[8000," + std::to_string(end) + ")";
        return {text, text};
    };
    rig.push_ms(1000);
    rig.session().process();
    rig.push_ms(1000);
    rig.session().process();
    rig.push_ms(1000);
    CHECK_THROWS_AS(rig.session().process(), echoir::BackendFailure);
    rig.session().process();  // the audio of the failed tick is still pending, so it is retried

    const std::vector<ts::PrefixBackend::Call>& calls = rig.backend.calls;
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].end == 47999);
    CHECK(calls[3].end == 47999);
    CHECK(calls[2].prefix == "[8000,31999)");
    CHECK(calls[3].prefix == "[8000,31999)");
    CHECK(calls[3].rollback_tokens == 5);
    CHECK(count_type(rig.events, ECHOIR_EVENT_ERROR) == 1);
    CHECK(rig.events.back().type == ECHOIR_EVENT_PARTIAL);
}

TEST_CASE("agreement: no decode gets a prefix, and the rollback defaults are 2 and 5") {
    const SessionConfig defaults{};
    CHECK(defaults.policy == echoir::StreamPolicy::agreement);
    CHECK(defaults.unfixed_decodes == 2);
    CHECK(defaults.rollback_tokens == 5);

    PrefixRig rig;
    rig.vad.speech = {{8000, 400000}};
    run_seconds(rig, 4);
    REQUIRE(rig.backend.calls.size() == 4);
    for (const ts::PrefixBackend::Call& call : rig.backend.calls) {
        CHECK(call.prefix.empty());
        CHECK(call.rollback_tokens == 0);
    }
}

TEST_CASE("rollback: a backend without prefix support is refused, and bad settings are rejected") {
    FakeBackend backend;  // no prefix support
    FakeVad vad;
    const auto sink = [](const SessionEvent&) {};
    CHECK_THROWS_AS(echoir::Session(backend, vad, rollback_config(), sink), std::invalid_argument);

    ts::PrefixBackend no_prefix;
    no_prefix.prefix_support = false;
    CHECK_THROWS_AS(echoir::Session(no_prefix, vad, rollback_config(), sink),
                    std::invalid_argument);

    ts::PrefixBackend with_prefix;
    SessionConfig negative_unfixed = rollback_config();
    negative_unfixed.unfixed_decodes = -1;
    CHECK_THROWS_AS(echoir::Session(with_prefix, vad, negative_unfixed, sink),
                    std::invalid_argument);
    SessionConfig negative_tokens = rollback_config();
    negative_tokens.rollback_tokens = -1;
    CHECK_THROWS_AS(echoir::Session(with_prefix, vad, negative_tokens, sink),
                    std::invalid_argument);

    // The agreement policy runs on a backend without prefix support.
    CHECK_NOTHROW(echoir::Session(backend, vad, SessionConfig{}, sink));
}

TEST_CASE("event queue: partials coalesce, overflow drops partials, finals are kept") {
    echoir::EventQueue queue;
    SessionEvent partial;
    partial.type = ECHOIR_EVENT_PARTIAL;
    partial.segment_id = 7;
    partial.text = "a";
    queue.push(partial);
    partial.text = "ab";
    queue.push(partial);

    SessionEvent out;
    REQUIRE(queue.pop(out));
    CHECK(out.text == "ab");
    CHECK(!queue.pop(out));

    // Fill the queue with partials of different segments, which do not coalesce.
    for (std::int64_t id = 1; id <= 1024; ++id) {
        partial.segment_id = id;
        queue.push(partial);
    }
    partial.segment_id = 2000;
    queue.push(partial);  // full: a partial is dropped

    SessionEvent final_event;
    final_event.type = ECHOIR_EVENT_FINAL;
    final_event.segment_id = 3000;
    queue.push(final_event);  // full: the oldest partial makes room

    std::size_t popped = 0;
    std::int64_t first_id = -1;
    while (queue.pop(out)) {
        if (popped == 0) {
            first_id = out.segment_id;
        }
        ++popped;
    }
    CHECK(popped == 1024);
    CHECK(first_id == 2);  // partial 1 was evicted, and the final went to the back
    CHECK(out.type == ECHOIR_EVENT_FINAL);
    CHECK(out.segment_id == 3000);
}

TEST_CASE("event queue: finals are kept past capacity while the host does not poll") {
    echoir::EventQueue queue;
    SessionEvent final_event;
    final_event.type = ECHOIR_EVENT_FINAL;
    for (std::int64_t id = 1; id <= 1100; ++id) {
        final_event.segment_id = id;
        queue.push(final_event);
    }
    std::size_t popped = 0;
    SessionEvent out;
    while (queue.pop(out)) {
        ++popped;
    }
    CHECK(popped == 1100);
}
