// Bounded queue of session events, for hosts that poll instead of taking callbacks.
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

#include "stream/session.h"

namespace echoir {

class EventQueue {
public:
    // The most events the queue holds before it starts to drop PARTIALs.
    static constexpr std::size_t kCapacity = 1024;

    // Thread-safe. A PARTIAL for the same segment as the newest queued event replaces it, because
    // the newer text supersedes the older. When the queue is full, a PARTIAL is dropped, and an
    // event of another kind drops the oldest queued PARTIAL to make room. FINAL, CLEARED and ERROR
    // are never dropped, so the queue can grow past kCapacity if the host stops polling.
    void push(SessionEvent event);

    // Thread-safe. Moves the oldest event into out. Returns false when the queue is empty.
    bool pop(SessionEvent& out);

private:
    std::mutex mu_;
    std::deque<SessionEvent> events_;
};

}  // namespace echoir
