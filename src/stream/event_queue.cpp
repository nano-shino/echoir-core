#include "stream/event_queue.h"

#include <algorithm>
#include <utility>

namespace echoir {

void EventQueue::push(SessionEvent event) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool partial = event.type == ECHOIR_EVENT_PARTIAL;
    if (partial && !events_.empty() && events_.back().type == ECHOIR_EVENT_PARTIAL &&
        events_.back().segment_id == event.segment_id) {
        events_.back() = std::move(event);
        return;
    }
    if (events_.size() >= kCapacity) {
        if (partial) {
            return;
        }
        const auto oldest_partial =
            std::find_if(events_.begin(), events_.end(),
                         [](const SessionEvent& e) { return e.type == ECHOIR_EVENT_PARTIAL; });
        if (oldest_partial != events_.end()) {
            events_.erase(oldest_partial);
        }
    }
    events_.push_back(std::move(event));
}

bool EventQueue::pop(SessionEvent& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (events_.empty()) {
        return false;
    }
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

}  // namespace echoir
