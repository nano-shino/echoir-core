// Scripted backend, VAD and event log for the live-mode tests. They need no model, and they are
// cheap enough to run under ThreadSanitizer.
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backends/asr_backend.h"
#include "stream/session.h"
#include "stream/signal.h"
#include "vad/vad.h"

namespace echoir::test_support {

// 16 kHz audio in 100 ms blocks, the unit of the fake VAD.
inline constexpr std::size_t kBlockSamples = 1600;

// Each decode answers "w<ms>", with the length of its audio in ms. before_decode runs inside
// decode, before the answer: it may block, or throw to fail the decode.
class LiveBackend final : public IAsrBackend {
public:
    std::function<void()> before_decode;
    std::atomic<std::int64_t> decodes{0};

    DecodeResult decode(const DecodeRequest& request) override {
        ++decodes;
        if (before_decode) {
            before_decode();
        }
        const auto ms = static_cast<std::int64_t>(request.pcm.size()) * 1000 / 16000;
        const std::string text = "w" + std::to_string(ms);
        return DecodeResult{text, text};  // no prefix support, so the continuation is the text
    }

    std::string name() const override { return "fake"; }
};

// Records every decode request, for tests of the decoder prefix. The audio is read the way the
// ramp audio of tests/unit/test_session.cpp is laid out: sample k holds k, so the first sample
// gives the window position. answer_for scripts the text and the continuation of a decode. The
// default answer is "[start,end)" for both, so the next decode of the segment gets it as prefix.
// supports_prefix() is configurable, and a prefix sent to a backend without it throws, as the real
// backends do.
class PrefixBackend final : public IAsrBackend {
public:
    struct Call {
        std::int64_t start = 0;  // the first sample's value
        std::int64_t end = 0;    // start plus the number of samples
        std::string prefix;
        int rollback_tokens = 0;
    };
    struct Answer {
        std::string text;
        std::string continuation;
    };

    bool prefix_support = true;
    std::function<Answer(std::int64_t start, std::int64_t end)> answer_for;
    std::vector<Call> calls;

    DecodeResult decode(const DecodeRequest& request) override {
        if (request.pcm.empty()) {
            throw std::logic_error("decode was called with no audio");
        }
        if (!prefix_support && !request.prefix.empty()) {
            throw std::invalid_argument("this backend does not take a prefix");
        }
        const auto start = static_cast<std::int64_t>(request.pcm.front());
        const auto end = start + static_cast<std::int64_t>(request.pcm.size());
        calls.push_back(Call{start, end, std::string(request.prefix), request.rollback_tokens});
        Answer answer{"[" + std::to_string(start) + "," + std::to_string(end) + ")", {}};
        answer.continuation = answer.text;
        if (answer_for) {
            answer = answer_for(start, end);
        }
        return DecodeResult{answer.text, answer.continuation};
    }

    bool supports_prefix() const override { return prefix_support; }
    std::string name() const override { return "fake"; }
};

// Reports each run of 100 ms blocks whose RMS is above a small threshold as one speech range.
// A tone block is speech and a zero block is silence.
class LiveVad final : public vad::IVad {
public:
    std::vector<vad::Range> detect(std::span<const float> pcm) override {
        std::vector<vad::Range> out;
        const std::size_t blocks = pcm.size() / kBlockSamples;
        std::int64_t run_start = -1;
        for (std::size_t b = 0; b <= blocks; ++b) {
            const bool loud =
                b < blocks && stream::rms(pcm.subspan(b * kBlockSamples, kBlockSamples)) > 0.01;
            if (loud && run_start < 0) {
                run_start = static_cast<std::int64_t>(b) * 100;
            } else if (!loud && run_start >= 0) {
                out.push_back(vad::Range{run_start, static_cast<std::int64_t>(b) * 100});
                run_start = -1;
            }
        }
        return out;
    }

    std::string name() const override { return "fake"; }
};

// A 440 Hz tone of `samples` samples when speech is set, silence otherwise. phase carries the
// tone across calls, so consecutive blocks join without a click.
inline std::vector<float> tone(std::size_t samples, bool speech, std::size_t& phase) {
    std::vector<float> out(samples, 0.0f);
    if (!speech) {
        return out;
    }
    constexpr double kTwoPi = 6.283185307179586;
    for (std::size_t i = 0; i < samples; ++i, ++phase) {
        out[i] = static_cast<float>(
            0.3 * std::sin(kTwoPi * 440.0 * static_cast<double>(phase) / 16000.0));
    }
    return out;
}

// Events from a sink, with the thread that delivered each one. Thread-safe.
class EventLog {
public:
    struct Entry {
        SessionEvent event;
        std::thread::id thread;
    };

    void add(const SessionEvent& event) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.push_back(Entry{event, std::this_thread::get_id()});
    }

    std::vector<Entry> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

    // Counts the events of one kind.
    std::size_t count(echoir_event_type type) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const Entry& e : entries_) {
            if (e.event.type == type) ++n;
        }
        return n;
    }

private:
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
};

// Polls until pred() is true or the timeout passes. Returns the final value of pred().
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace echoir::test_support
