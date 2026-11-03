// Sessions that share one decoder and one VAD through an EngineGate: turns in arrival order, never
// two calls on the models at once, and each session's events the same as when it runs alone. Runs
// on the scripted backend and VAD from tests/support/live_fakes.h, so no model is needed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../support/live_fakes.h"
#include "doctest.h"
#include "json.hpp"
#include "stream/engine_gate.h"
#include "stream/session.h"

namespace {

namespace ts = echoir::test_support;
using echoir::SessionConfig;
using echoir::SessionEvent;
using echoir::stream::EngineGate;
using namespace std::chrono_literals;

// Counts the calls that run on the shared models at the same time, and records which thread made
// each call. Thread-safe.
class Models {
public:
    void enter(std::chrono::milliseconds hold) {
        const int now = ++in_flight_;
        int seen = max_in_flight_.load();
        while (now > seen && !max_in_flight_.compare_exchange_weak(seen, now)) {
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            callers_.push_back(std::this_thread::get_id());
        }
        if (hold.count() > 0) {
            std::this_thread::sleep_for(hold);
        }
        --in_flight_;
    }

    int max_in_flight() const { return max_in_flight_.load(); }

    std::vector<std::thread::id> callers() const {
        std::lock_guard<std::mutex> lock(mu_);
        return callers_;
    }

private:
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_in_flight_{0};
    mutable std::mutex mu_;
    std::vector<std::thread::id> callers_;
};

// The fake VAD, with every call counted in models.
class CountingVad final : public echoir::vad::IVad {
public:
    CountingVad(Models& models, std::chrono::milliseconds hold) : models_(models), hold_(hold) {}

    std::vector<echoir::vad::Range> detect(std::span<const float> pcm) override {
        models_.enter(hold_);
        return inner_.detect(pcm);
    }

    std::string name() const override { return "fake"; }

private:
    Models& models_;
    std::chrono::milliseconds hold_;
    ts::LiveVad inner_;
};

// Speech or silence of `ms` milliseconds at 16 kHz.
std::vector<float> audio(std::size_t ms, bool speech, std::size_t& phase) {
    return ts::tone(ms * 16, speech, phase);
}

// Speech segments of the given lengths, each followed by 1 s of silence.
std::vector<float> speech_of(const std::vector<std::size_t>& lengths_ms) {
    std::vector<float> out;
    std::size_t phase = 0;
    for (const std::size_t ms : lengths_ms) {
        const std::vector<float> s = audio(ms, true, phase);
        const std::vector<float> q = audio(1000, false, phase);
        out.insert(out.end(), s.begin(), s.end());
        out.insert(out.end(), q.begin(), q.end());
    }
    return out;
}

// The PARTIAL and FINAL events, without STATS, as "type id start end text" lines.
std::vector<std::string> text_events(const ts::EventLog& log) {
    std::vector<std::string> out;
    for (const auto& entry : log.snapshot()) {
        const SessionEvent& ev = entry.event;
        if (ev.type != ECHOIR_EVENT_PARTIAL && ev.type != ECHOIR_EVENT_FINAL) {
            continue;
        }
        out.push_back(std::to_string(ev.type) + " " + std::to_string(ev.segment_id) + " " +
                      std::to_string(ev.start_ms) + " " + std::to_string(ev.end_ms) + " " +
                      ev.text);
    }
    return out;
}

// The text of the first FINAL, or "" when there is none.
std::string first_final(const ts::EventLog& log) {
    for (const auto& entry : log.snapshot()) {
        if (entry.event.type == ECHOIR_EVENT_FINAL) {
            return entry.event.text;
        }
    }
    return {};
}

// Pushes pcm in 100 ms blocks and runs process() after each, then flush(): the same ticks every
// time, so the events do not depend on timing.
void pull_through(echoir::Session& session, const std::vector<float>& pcm) {
    for (std::size_t at = 0; at < pcm.size(); at += ts::kBlockSamples) {
        const std::size_t n = std::min(ts::kBlockSamples, pcm.size() - at);
        session.push(std::span<const float>(pcm.data() + at, n), 16000, 1);
        session.process();
    }
    session.flush();
}

}  // namespace

TEST_CASE("engine gate: turns go in the order they were asked for") {
    EngineGate gate;
    std::vector<int> order;
    std::mutex order_mu;
    std::vector<std::thread> threads;
    {
        const EngineGate::Turn held = gate.enter();
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&, i] {
                const EngineGate::Turn turn = gate.enter();
                std::lock_guard<std::mutex> lock(order_mu);
                order.push_back(i);
            });
            // Thread i waits before thread i + 1 asks.
            const auto queued = static_cast<std::uint64_t>(i + 2);
            CHECK(ts::wait_for([&] { return gate.queued() == queued; }));
        }
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(order == std::vector<int>{0, 1, 2});
    CHECK(gate.queued() == 0);
}

TEST_CASE("engine gate: a turn reports how long it waited") {
    EngineGate gate;
    double waited = -1.0;
    std::thread other;
    {
        const EngineGate::Turn held = gate.enter();
        CHECK(held.wait_ms() < 50.0);
        other = std::thread([&] {
            const EngineGate::Turn turn = gate.enter();
            waited = turn.wait_ms();
        });
        CHECK(ts::wait_for([&] { return gate.queued() == 2; }));
        std::this_thread::sleep_for(60ms);
    }
    other.join();
    CHECK(waited >= 50.0);
}

TEST_CASE("shared engine: VAD runs and decodes of two live sessions never overlap") {
    Models models;
    ts::LiveBackend backend;
    backend.before_decode = [&models] { models.enter(5ms); };
    CountingVad vad(models, 2ms);
    auto gate = std::make_shared<EngineGate>();
    ts::EventLog log_a;
    ts::EventLog log_b;
    echoir::Session a(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_a.add(ev); }, gate);
    echoir::Session b(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_b.add(ev); }, gate);
    a.start();
    b.start();
    std::thread pusher_b([&b] {
        std::size_t phase = 0;
        for (int i = 0; i < 30; ++i) {
            b.push(audio(100, i < 15, phase), 16000, 1);
            std::this_thread::sleep_for(2ms);
        }
    });
    std::size_t phase = 0;
    for (int i = 0; i < 30; ++i) {
        a.push(audio(100, i < 20, phase), 16000, 1);
        std::this_thread::sleep_for(2ms);
    }
    pusher_b.join();
    REQUIRE(ts::wait_for([&] {
        return log_a.count(ECHOIR_EVENT_FINAL) > 0 && log_b.count(ECHOIR_EVENT_FINAL) > 0;
    }));
    a.stop();
    b.stop();

    CHECK(models.max_in_flight() == 1);
    // Each session got its own text: the length of its own speech.
    CHECK(first_final(log_a) == "w2000");
    CHECK(first_final(log_b) == "w1500");
}

TEST_CASE("shared engine: two busy sessions take turns") {
    Models models;
    ts::LiveBackend backend;
    backend.before_decode = [&models] { models.enter(20ms); };
    CountingVad vad(models, 0ms);
    auto gate = std::make_shared<EngineGate>();
    ts::EventLog log_a;
    ts::EventLog log_b;
    echoir::Session a(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_a.add(ev); }, gate);
    echoir::Session b(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_b.add(ev); }, gate);
    // Many short segments, pushed before start, keep both workers busy for many turns.
    const std::vector<float> pcm = speech_of(std::vector<std::size_t>(12, 300));
    a.push(pcm, 16000, 1);
    b.push(pcm, 16000, 1);
    a.start();
    b.start();
    REQUIRE(ts::wait_for([&] {
        return log_a.count(ECHOIR_EVENT_FINAL) >= 12 && log_b.count(ECHOIR_EVENT_FINAL) >= 12;
    }));
    a.stop();
    b.stop();

    // While both wait, neither gets more than a few turns in a row. A session can get two when
    // the other is between ticks, outside the gate.
    const std::vector<std::thread::id> callers = models.callers();
    REQUIRE(callers.size() > 20);
    std::size_t run = 1;
    std::size_t longest = 1;
    for (std::size_t i = 1; i < callers.size(); ++i) {
        run = callers[i] == callers[i - 1] ? run + 1 : 1;
        longest = std::max(longest, run);
    }
    CHECK(longest <= 3);
    CHECK(models.max_in_flight() == 1);
}

TEST_CASE("shared engine: each session's events are the same as when it runs alone") {
    const std::vector<float> pcm_a = speech_of({1500, 400, 2200});
    const std::vector<float> pcm_b = speech_of({700, 3000});

    const auto solo = [](const std::vector<float>& pcm) {
        ts::LiveBackend backend;
        ts::LiveVad vad;
        ts::EventLog log;
        echoir::Session session(backend, vad, SessionConfig{},
                                [&log](const SessionEvent& ev) { log.add(ev); });
        pull_through(session, pcm);
        return text_events(log);
    };
    const std::vector<std::string> alone_a = solo(pcm_a);
    const std::vector<std::string> alone_b = solo(pcm_b);
    REQUIRE(!alone_a.empty());
    REQUIRE(!alone_b.empty());

    Models models;
    ts::LiveBackend backend;
    backend.before_decode = [&models] { models.enter(1ms); };
    CountingVad vad(models, 1ms);
    auto gate = std::make_shared<EngineGate>();
    ts::EventLog log_a;
    ts::EventLog log_b;
    echoir::Session a(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_a.add(ev); }, gate);
    echoir::Session b(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_b.add(ev); }, gate);
    std::thread thread_b([&] { pull_through(b, pcm_b); });
    pull_through(a, pcm_a);
    thread_b.join();

    CHECK(text_events(log_a) == alone_a);
    CHECK(text_events(log_b) == alone_b);
    CHECK(models.max_in_flight() == 1);
}

TEST_CASE("shared engine: clear and stop on one session do not wait for another's decode") {
    std::atomic<bool> release{false};
    std::atomic<bool> first{true};
    ts::LiveBackend backend;
    // The first decode, B's, holds the gate until released (or for 5 s at most).
    backend.before_decode = [&] {
        if (first.exchange(false)) {
            for (int i = 0; i < 5000 && !release.load(); ++i) {
                std::this_thread::sleep_for(1ms);
            }
        }
    };
    ts::LiveVad vad;
    auto gate = std::make_shared<EngineGate>();
    ts::EventLog log_a;
    ts::EventLog log_b;
    auto a = std::make_unique<echoir::Session>(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_a.add(ev); }, gate);
    echoir::Session b(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) { log_b.add(ev); }, gate);
    std::size_t phase_b = 0;
    b.start();
    b.push(audio(1000, true, phase_b), 16000, 1);
    REQUIRE(ts::wait_for([&] { return backend.decodes.load() == 1; }));

    // A waits for the gate that B's decode holds.
    std::size_t phase_a = 0;
    a->start();
    a->push(audio(1000, true, phase_a), 16000, 1);
    CHECK(ts::wait_for([&] { return gate->queued() == 2; }));

    const auto began = std::chrono::steady_clock::now();
    a->clear();
    CHECK(std::chrono::steady_clock::now() - began < 1s);
    CHECK(log_a.count(ECHOIR_EVENT_CLEARED) == 1);

    // stop() on A waits for A's own tick, which waits for B's decode. B is released shortly after.
    std::thread releaser([&] {
        std::this_thread::sleep_for(50ms);
        release = true;
    });
    a->stop();
    releaser.join();
    // After stop() returns, A sends nothing more, and B goes on decoding after A is destroyed.
    const std::size_t a_events = log_a.count();
    a.reset();
    b.push(audio(500, true, phase_b), 16000, 1);
    b.push(audio(1000, false, phase_b), 16000, 1);
    REQUIRE(ts::wait_for([&] { return log_b.count(ECHOIR_EVENT_FINAL) > 0; }));
    b.stop();
    CHECK(log_a.count() == a_events);
}

TEST_CASE("shared engine: STATS reports the wait for the gate apart from the decode time") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    auto gate = std::make_shared<EngineGate>();
    ts::EventLog log;
    SessionConfig config;
    config.stats_every_ticks = 1;
    echoir::Session session(
        backend, vad, config, [&log](const SessionEvent& ev) { log.add(ev); }, gate);
    std::size_t phase = 0;
    session.push(audio(1000, true, phase), 16000, 1);

    std::thread ticker;
    {
        // Another user of the engine holds the gate while the session's tick asks for it.
        const EngineGate::Turn held = gate->enter();
        ticker = std::thread([&session] { session.process(); });
        CHECK(ts::wait_for([&] { return gate->queued() == 2; }));
        std::this_thread::sleep_for(100ms);
    }
    ticker.join();

    nlohmann::json stats;
    for (const auto& entry : log.snapshot()) {
        if (entry.event.type == ECHOIR_EVENT_STATS) {
            stats = nlohmann::json::parse(entry.event.detail);
        }
    }
    REQUIRE(stats.is_object());
    CHECK(stats["wait_ms"].get<std::int64_t>() >= 80);
    CHECK(stats["decode_ms"].get<std::int64_t>() < 50);
}
