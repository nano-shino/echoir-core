// Stress test of live mode. Three threads push audio at the real-time rate, while another thread
// starts, stops, clears and resets the session at random for a minute. Run it under
// ThreadSanitizer (the macos-tsan preset) to check for data races. It fails when any sink call
// reaches a worker after stop() has returned. Clear and reset run their sink on the calling thread,
// which the test accounts for.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "../support/live_fakes.h"
#include "doctest.h"
#include "stream/session.h"

namespace {

namespace ts = echoir::test_support;
using echoir::SessionConfig;
using echoir::SessionEvent;
using namespace std::chrono_literals;

constexpr auto kRunTime = std::chrono::seconds(60);
constexpr int kPushers = 3;
// Each push is 100 ms of audio, pushed every 100 ms of wall time.
constexpr std::size_t kChunkMs = 100;

// True on the controller thread while it calls clear() or reset(). Their sink calls run on that
// thread, so they are allowed even when no worker runs.
thread_local bool g_control_call = false;

struct ControlCall {
    ControlCall() { g_control_call = true; }
    ~ControlCall() { g_control_call = false; }
};

}  // namespace

TEST_CASE("live stress: start, stop, clear and reset race with three real-time pushers") {
    ts::LiveBackend backend;
    ts::LiveVad vad;

    // Set true before start() and false after stop() returns. A sink call from a worker while
    // this is false would be a callback after stop.
    std::atomic<bool> accepting{false};
    std::atomic<std::int64_t> late_callbacks{0};
    std::atomic<std::int64_t> events{0};
    std::atomic<std::int64_t> finals_with_text{0};

    echoir::Session session(backend, vad, SessionConfig{}, [&](const SessionEvent& ev) {
        if (!accepting.load() && !g_control_call) {
            ++late_callbacks;
        }
        ++events;
        if (ev.type == ECHOIR_EVENT_FINAL && !ev.text.empty()) {
            ++finals_with_text;
        }
    });

    std::atomic<bool> done{false};
    std::vector<std::thread> pushers;
    for (int id = 0; id < kPushers; ++id) {
        pushers.emplace_back([&session, &done, id] {
            std::mt19937 rng(static_cast<std::mt19937::result_type>(1000 + id));
            std::bernoulli_distribution toggle(0.3);
            std::size_t phase = 0;
            bool speech = id % 2 == 0;
            const auto began = std::chrono::steady_clock::now();
            for (std::int64_t n = 1; !done.load(); ++n) {
                if (toggle(rng)) speech = !speech;
                const std::vector<float> chunk = ts::tone(kChunkMs * 16, speech, phase);
                session.push(chunk, 16000, 1);
                std::this_thread::sleep_until(began + n * std::chrono::milliseconds(kChunkMs));
            }
        });
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> pick(0, 99);
    std::uniform_int_distribution<int> pause_ms(20, 300);
    bool running = false;
    std::int64_t starts = 0;
    std::int64_t stops = 0;
    std::int64_t clears = 0;
    std::int64_t resets = 0;
    const auto deadline = std::chrono::steady_clock::now() + kRunTime;
    while (std::chrono::steady_clock::now() < deadline) {
        const int op = pick(rng);
        if (op < 35) {
            if (!running) {
                accepting = true;
                session.start();
                running = true;
                ++starts;
            } else {
                session.stop();
                accepting = false;
                running = false;
                ++stops;
            }
        } else if (op < 70) {
            ControlCall call;
            session.clear();
            ++clears;
        } else {
            ControlCall call;
            session.reset();
            ++resets;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms(rng)));
    }
    if (running) {
        session.stop();
        accepting = false;
    }
    done = true;
    for (std::thread& t : pushers) {
        t.join();
    }

    MESSAGE("starts " << starts << ", stops " << stops << ", clears " << clears << ", resets "
                      << resets << ", events " << events.load() << ", finals with text "
                      << finals_with_text.load());
    CHECK(late_callbacks.load() == 0);
    CHECK(starts > 0);
    CHECK(events.load() > 0);
    CHECK(finals_with_text.load() > 0);
}
