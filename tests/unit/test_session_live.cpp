// Live mode of the session: start and stop, the worker thread, and the rules that tie them to the
// other calls. Runs on the scripted backend and VAD from tests/support/live_fakes.h, so no model
// is needed.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../support/live_fakes.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "errors.h"
#include "stream/session.h"
#include "vad/vad.h"

namespace {

namespace ts = echoir::test_support;
using echoir::SessionConfig;
using echoir::SessionEvent;
using namespace std::chrono_literals;

// Speech or silence of `ms` milliseconds at 16 kHz.
std::vector<float> audio(std::size_t ms, bool speech, std::size_t& phase) {
    return ts::tone(ms * 16, speech, phase);
}

}  // namespace

TEST_CASE("live: start is refused after process, and process is refused after start") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    echoir::Session pulled(backend, vad, SessionConfig{},
                           [&log](const SessionEvent& ev) { log.add(ev); });
    pulled.process();
    CHECK_THROWS_AS(pulled.start(), echoir::StateError);

    echoir::Session live(backend, vad, SessionConfig{},
                         [&log](const SessionEvent& ev) { log.add(ev); });
    live.start();
    CHECK_THROWS_AS(live.start(), echoir::StateError);
    CHECK_THROWS_AS(live.process(), echoir::StateError);
    live.stop();
    live.stop();  // a second stop has nothing to stop
}

TEST_CASE("live: the worker decodes pushed audio on its own thread") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    // 1.5 s of speech, then 1 s of silence, which ends the segment as a final.
    session.push(audio(1500, true, phase), 16000, 1);
    session.push(audio(1000, false, phase), 16000, 1);
    REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_FINAL) > 0; }));
    session.stop();

    const auto entries = log.snapshot();
    REQUIRE(!entries.empty());
    for (const auto& entry : entries) {
        CHECK(entry.thread != std::this_thread::get_id());
    }
    for (const auto& entry : entries) {
        if (entry.event.type == ECHOIR_EVENT_FINAL) {
            CHECK(entry.event.text == "w1500");
            CHECK(entry.event.start_ms == 0);
            CHECK(entry.event.end_ms == 1500);
            break;
        }
    }
}

TEST_CASE("live: push returns while a decode is blocked") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    std::atomic<bool> gate{false};
    // The gate opens on its own after 5 s, so a push that waits for the decode fails the timing
    // check below instead of hanging the test.
    backend.before_decode = [&gate] {
        for (int i = 0; i < 5000 && !gate.load(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
    };
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    session.push(audio(1000, true, phase), 16000, 1);
    REQUIRE(ts::wait_for([&backend] { return backend.decodes.load() == 1; }));

    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        session.push(audio(100, true, phase), 16000, 1);
    }
    CHECK(std::chrono::steady_clock::now() - began < 1s);

    gate = true;
    session.stop();
}

TEST_CASE("live: no callback runs after stop returns") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    session.push(audio(1000, true, phase), 16000, 1);
    REQUIRE(ts::wait_for([&log] { return log.count() > 0; }));
    session.stop();

    const std::size_t after_stop = log.count();
    // Audio pushed while stopped is kept, but nothing decodes it until the next start.
    session.push(audio(1500, true, phase), 16000, 1);
    session.push(audio(1000, false, phase), 16000, 1);
    std::this_thread::sleep_for(200ms);
    CHECK(log.count() == after_stop);
}

TEST_CASE("live: audio pushed while stopped is decoded after the next start") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    session.stop();

    session.push(audio(1500, true, phase), 16000, 1);
    session.push(audio(1000, false, phase), 16000, 1);
    CHECK(log.count() == 0);

    session.start();
    REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_FINAL) > 0; }));
    session.stop();
}

TEST_CASE("live: stop is refused from a sink, which runs on the worker") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    std::unique_ptr<echoir::Session> session;
    std::atomic<bool> refused{false};
    std::atomic<bool> tried{false};
    session = std::make_unique<echoir::Session>(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) {
            // Joining the worker from the worker would never return, so stop must refuse.
            if (ev.type != ECHOIR_EVENT_FINAL || tried.exchange(true)) return;
            try {
                session->stop();
            } catch (const echoir::StateError&) {
                refused = true;
            }
        });
    std::size_t phase = 0;
    session->start();
    session->push(audio(1500, true, phase), 16000, 1);
    session->push(audio(1000, false, phase), 16000, 1);
    REQUIRE(ts::wait_for([&refused] { return refused.load(); }));
    session->stop();
}

TEST_CASE("live: a failed tick reports an error and is not retried without new audio") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    backend.before_decode = [] { throw std::runtime_error("decode failed in test"); };
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    session.push(audio(1000, true, phase), 16000, 1);
    REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_ERROR) > 0; }));
    std::this_thread::sleep_for(300ms);
    // One attempt: the worker waits for the next push instead of retrying in a loop.
    CHECK(backend.decodes.load() == 1);
    session.stop();
}

TEST_CASE("live: clear sends CLEARED on the caller's thread") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    session.start();
    session.clear();
    session.stop();
    const auto entries = log.snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.type == ECHOIR_EVENT_CLEARED);
    CHECK(entries[0].thread == std::this_thread::get_id());
}

TEST_CASE("live: destroying a started session stops its worker first") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    std::size_t phase = 0;
    {
        echoir::Session session(backend, vad, SessionConfig{},
                                [&log](const SessionEvent& ev) { log.add(ev); });
        session.start();
        session.push(audio(1500, true, phase), 16000, 1);
        session.push(audio(1000, false, phase), 16000, 1);
        REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_FINAL) > 0; }));
    }
    // The sink's log outlives the session, and no sink call runs after the destructor returns.
    const std::size_t at_destruction = log.count();
    std::this_thread::sleep_for(100ms);
    CHECK(log.count() == at_destruction);
}

TEST_CASE("live: a failed tick drops its audio, and the next push decodes only new audio") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    std::atomic<bool> fail_next{true};
    backend.before_decode = [&fail_next] {
        if (fail_next.exchange(false)) {
            throw std::runtime_error("decode failed in test");
        }
    };
    echoir::Session session(backend, vad, SessionConfig{},
                            [&log](const SessionEvent& ev) { log.add(ev); });
    std::size_t phase = 0;
    session.start();
    session.push(audio(1000, true, phase), 16000, 1);
    REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_ERROR) > 0; }));
    // The failed window is gone, so this window holds only the second second of audio.
    session.push(audio(1000, true, phase), 16000, 1);
    REQUIRE(ts::wait_for([&log] { return log.count(ECHOIR_EVENT_PARTIAL) > 0; }));
    session.stop();

    CHECK(backend.decodes.load() == 2);
    for (const auto& entry : log.snapshot()) {
        if (entry.event.type == ECHOIR_EVENT_PARTIAL) {
            // The range runs into the window end, so its decode stops one sample short of 1 s.
            CHECK(entry.event.text == "w999");
            CHECK(entry.event.start_ms == 1000);
        }
    }
}

TEST_CASE("sink calls that would wait on their own call are refused, not deadlocked") {
    ts::LiveBackend backend;
    ts::LiveVad vad;
    std::unique_ptr<echoir::Session> session;
    std::atomic<int> refused{0};
    session = std::make_unique<echoir::Session>(
        backend, vad, SessionConfig{}, [&](const SessionEvent& ev) {
            // flush() delivers this FINAL on the calling thread, with its own lock held.
            if (ev.type != ECHOIR_EVENT_FINAL) return;
            try {
                session->flush();
            } catch (const echoir::StateError&) {
                ++refused;
            }
            try {
                session->stop();
            } catch (const echoir::StateError&) {
                ++refused;
            }
            try {
                session->clear();
            } catch (const echoir::StateError&) {
                ++refused;
            }
        });
    std::size_t phase = 0;
    session->push(audio(1000, true, phase), 16000, 1);
    session->flush();
    CHECK(refused.load() == 3);
}
