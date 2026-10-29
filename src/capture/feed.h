// Live capture from an audio device into a streaming session.
//
// The device's data callback only moves samples into a lock-free ring. A feeder thread drains the
// ring into Session::push about every 20 ms, so the audio thread never waits on the session.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "capture/devices.h"

namespace echoir {

class Session;

namespace capture {

class CaptureFeed {
public:
    // Opens the device without starting it. An empty device_id is the system default, and the
    // capture follows it when it changes; when there is none, it waits for one. Otherwise device_id is an id from list_devices(). Throws
    // NotSupported for loopback on an audio backend without it, std::invalid_argument for an id
    // that no device has, and std::runtime_error when the device cannot open. The session must
    // outlive the feed.
    CaptureFeed(Source source, const std::string& device_id, Session& session);
    // Stops the capture, as stop() does.
    ~CaptureFeed();
    CaptureFeed(const CaptureFeed&) = delete;
    CaptureFeed& operator=(const CaptureFeed&) = delete;

    // Starts the device and the feeder. Throws StateError when already started, and
    // std::runtime_error when the device cannot start.
    void start();
    // Stops the device, pushes the audio it captured into the session, and joins the feeder.
    // Afterwards the session gets no more audio from this feed. The session is not flushed.
    // Safe to call more than once, and when the capture has ended by itself.
    void stop();

    // The format of the device in use. It changes when a default device is reopened.
    std::uint32_t rate() const;
    std::uint32_t channels() const;
    // The name of the device in use.
    std::string device_name() const;

    // True once the capture has ended by itself: the device was lost, or the feeder failed. An
    // ERROR event has been sent by then. Safe from any thread.
    bool ended() const;
    // True while a lost default device has not come back for a second or more, such as when the
    // system has no device at all. The capture resumes by itself when one appears. Safe from any
    // thread.
    bool waiting() const;
    // Frames pushed into the session, and frames the callback could not store because the feeder
    // fell behind. Safe from any thread.
    std::uint64_t frames_delivered() const;
    std::uint64_t dropped_frames() const;
    // The largest sample magnitude pushed so far. Safe from any thread.
    float peak() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace capture
}  // namespace echoir
