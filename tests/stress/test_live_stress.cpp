// Stress test of live mode. Three threads push audio at the real-time rate, while another thread
// starts, stops, clears and resets the session at random for a minute. Run it under
// ThreadSanitizer (the macos-tsan preset) to check for data races. It fails when any sink call
// reaches a worker after stop() has returned. Clear and reset run their sink on the calling thread,
// which the test accounts for. A second case runs three sessions on one shared decoder and VAD,
// each with its own pusher and its own controller, and checks that no two model calls overlap.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "../support/live_fakes.h"
#include "doctest.h"
#include "stream/engine_gate.h"
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

TEST_CASE("live stress: three sessions on one engine race start, stop, clear and reset") {
    constexpr int kSessions = 3;
    ts::LiveBackend backend;
    ts::LiveVad vad;
    auto gate = std::make_shared<echoir::stream::EngineGate>();
    // Decodes of different sessions must never overlap: they share the backend.
    std::atomic<int> in_flight{0};
    std::atomic<int> overlaps{0};
    backend.before_decode = [&] {
        if (++in_flight > 1) {
            ++overlaps;
        }
        std::this_thread::sleep_for(2ms);
        --in_flight;
    };

    struct Run {
        std::atomic<bool> accepting{false};
        std::atomic<std::int64_t> late_callbacks{0};
        std::atomic<std::int64_t> finals_with_text{0};
        std::unique_ptr<echoir::Session> session;
    };
    std::vector<std::unique_ptr<Run>> runs;
    for (int i = 0; i < kSessions; ++i) {
        auto run = std::make_unique<Run>();
        Run* r = run.get();
        r->session = std::make_unique<echoir::Session>(
            backend, vad, SessionConfig{},
            [r](const SessionEvent& ev) {
                if (!r->accepting.load() && !g_control_call) {
                    ++r->late_callbacks;
                }
                if (ev.type == ECHOIR_EVENT_FINAL && !ev.text.empty()) {
                    ++r->finals_with_text;
                }
            },
            gate);
        runs.push_back(std::move(run));
    }

    std::atomic<bool> done{false};
    std::vector<std::thread> threads;
    for (int id = 0; id < kSessions; ++id) {
        Run& run = *runs[static_cast<std::size_t>(id)];
        // The pusher of this session, at the real-time rate.
        threads.emplace_back([&run, &done, id] {
            std::mt19937 rng(static_cast<std::mt19937::result_type>(2000 + id));
            std::bernoulli_distribution toggle(0.3);
            std::size_t phase = 0;
            bool speech = true;
            const auto began = std::chrono::steady_clock::now();
            for (std::int64_t n = 1; !done.load(); ++n) {
                if (toggle(rng)) speech = !speech;
                run.session->push(ts::tone(kChunkMs * 16, speech, phase), 16000, 1);
                std::this_thread::sleep_until(began + n * std::chrono::milliseconds(kChunkMs));
            }
        });
        // The controller of this session.
        threads.emplace_back([&run, id] {
            std::mt19937 rng(static_cast<std::mt19937::result_type>(3000 + id));
            std::uniform_int_distribution<int> pick(0, 99);
            std::uniform_int_distribution<int> pause_ms(20, 300);
            bool running = false;
            const auto deadline = std::chrono::steady_clock::now() + kRunTime;
            while (std::chrono::steady_clock::now() < deadline) {
                const int op = pick(rng);
                if (op < 50) {
                    if (!running) {
                        run.accepting = true;
                        run.session->start();
                    } else {
                        run.session->stop();
                        run.accepting = false;
                    }
                    running = !running;
                } else if (op < 80) {
                    ControlCall call;
                    run.session->clear();
                } else {
                    ControlCall call;
                    run.session->reset();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms(rng)));
            }
            if (running) {
                run.session->stop();
                run.accepting = false;
            }
        });
    }
    // The controllers end after kRunTime. Then the pushers stop.
    for (std::size_t i = 1; i < threads.size(); i += 2) {
        threads[i].join();
    }
    done = true;
    for (std::size_t i = 0; i < threads.size(); i += 2) {
        threads[i].join();
    }

    CHECK(overlaps.load() == 0);
    for (const auto& run : runs) {
        CHECK(run->late_callbacks.load() == 0);
        CHECK(run->finals_with_text.load() > 0);
    }
}
