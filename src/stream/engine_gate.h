// The gate of an engine: sessions that share one decoder and one VAD take turns on them.
//
// Neither the decoders nor the VADs may run two calls at once, and a session decodes on its own
// thread. So every VAD call and every decode of a session runs inside a turn of its engine's gate.
// Turns are granted in the order they were asked for (a ticket lock), so a session that waits
// gets the next turn after the ones already waiting, and two busy sessions alternate instead of
// one starving the other.
//
// A turn is taken with no other session lock held except the session's decode lock, and nothing
// inside a turn takes a session lock or runs a sink. So a turn never waits on a host, and a call
// such as clear() on one session never waits for another session's decode.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace echoir::stream {

class EngineGate {
public:
    // Holds a turn until it is destroyed. Moves only.
    class Turn {
    public:
        Turn(Turn&& other) noexcept : gate_(other.gate_), wait_ms_(other.wait_ms_) {
            other.gate_ = nullptr;
        }
        Turn& operator=(Turn&&) = delete;
        Turn(const Turn&) = delete;
        Turn& operator=(const Turn&) = delete;
        ~Turn() {
            if (gate_ != nullptr) {
                gate_->leave();
            }
        }

        // How long the turn waited for the gate, in ms.
        double wait_ms() const noexcept { return wait_ms_; }

    private:
        friend class EngineGate;
        Turn(EngineGate* gate, double wait_ms) noexcept : gate_(gate), wait_ms_(wait_ms) {}

        EngineGate* gate_;
        double wait_ms_;
    };

    EngineGate() = default;
    EngineGate(const EngineGate&) = delete;
    EngineGate& operator=(const EngineGate&) = delete;

    // Waits for this caller's turn and returns it. Not reentrant: a thread that holds a turn and
    // asks for another waits forever.
    Turn enter() {
        const auto asked = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mu_);
        const std::uint64_t ticket = next_++;
        cv_.wait(lock, [&] { return serving_ == ticket; });
        lock.unlock();
        const std::chrono::duration<double, std::milli> waited =
            std::chrono::steady_clock::now() - asked;
        return Turn(this, waited.count());
    }

    // The callers that hold a turn or wait for one.
    std::uint64_t queued() {
        std::lock_guard<std::mutex> lock(mu_);
        return next_ - serving_;
    }

private:
    void leave() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++serving_;
        }
        // Every waiter checks whether the next ticket is its own.
        cv_.notify_all();
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t next_ = 0;     // the ticket the next caller gets
    std::uint64_t serving_ = 0;  // the ticket whose turn it is
};

}  // namespace echoir::stream
