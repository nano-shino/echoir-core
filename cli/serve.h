// The stdio service, `echoir serve --stdio`. It reads one JSON command per line and writes the
// session events and one reply per command, each as one JSON object per line. docs/protocol.md
// describes the commands and the output.
#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "capture/devices.h"
#include "capture/feed.h"
#include "json.hpp"
#include "models.h"
#include "options.h"
#include "stream/session.h"

namespace echoir::cli {

// What the service needs from its host. The command line fills both with the real loader and the
// real device listing. Tests fill them with fakes.
struct ServeHooks {
    // Loads the speech model and the VAD for the options. Throws on failure.
    std::function<LoadedModels(const TranscribeOptions&)> load;
    // Lists the capture and loopback devices. Throws when the audio system cannot be opened.
    std::function<capture::DeviceList()> devices;
};

// The protocol handler. The streams are plain parameters, so a test can drive it through strings.
// Writes go under one lock: events come from the session's worker thread and from the capture
// feed, and replies come from the thread that runs run().
class StdioService {
public:
    StdioService(std::istream& in, std::ostream& out, ServeHooks hooks);
    // Stops a running session, so its final events are written before the service is gone.
    ~StdioService();
    StdioService(const StdioService&) = delete;
    StdioService& operator=(const StdioService&) = delete;

    // Handles lines until quit or the end of input. Returns the process exit status, which is 0.
    int run();

private:
    // Handles one line. Returns false when the line was quit.
    bool handle_line(const std::string& line);
    bool quit();

    // Each command throws std::exception when it fails. handle_line sends the error reply.
    void load(const nlohmann::json& command);
    void start(const nlohmann::json& command);
    void audio(const nlohmann::json& command);
    void devices();

    // The running session. Throws when none runs.
    Session& running_session(const char* cmd);

    // Ends the session: the capture stops and pushes the last captured audio, the session flushes,
    // and its worker stops. The session is gone afterwards, even when the flush throws. The flush
    // error is rethrown.
    void stop_session();

    void write_line(const std::string& text);
    void write_event(const SessionEvent& event);
    void reply_ok(const std::string& cmd);
    void reply_error(const std::string& cmd, const std::string& message);

    std::istream& in_;
    std::ostream& out_;
    ServeHooks hooks_;
    std::mutex out_mu_;  // one line at a time, from either thread

    // Replaced by load, and only when no session runs. A session refers to them.
    std::optional<LoadedModels> models_;
    std::unique_ptr<Session> session_;
    // Set for mic and loopback sessions. Declared after the session, so the capture stops before
    // the session is destroyed.
    std::unique_ptr<capture::CaptureFeed> feed_;
    bool push_input_ = false;  // the session takes audio from audio commands
};

}  // namespace echoir::cli
