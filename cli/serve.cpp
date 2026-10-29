#include "serve.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base64.h"
#include "capture/feed.h"
#include "devices_cmd.h"
#include "input.h"
#include "output.h"
#include "stream.h"

namespace echoir::cli {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

constexpr std::int64_t kDefaultMinChunkMs = 500;

constexpr std::int64_t kMaxInt = std::numeric_limits<std::int32_t>::max();

bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\r") == std::string::npos;
}

// The JSON text of a reply or event. Bad UTF-8 is replaced, so a line is always written.
std::string dump(const ordered_json& value) {
    return value.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

// The field of a command, or nullptr when it is missing or null.
const json* field(const json& command, const char* key) {
    const auto it = command.find(key);
    if (it == command.end() || it->is_null()) {
        return nullptr;
    }
    return &*it;
}

std::string required_string(const json& command, const char* key) {
    const json* value = field(command, key);
    if (value == nullptr) {
        throw std::invalid_argument(std::string("missing field '") + key + "'");
    }
    if (!value->is_string()) {
        throw std::invalid_argument(std::string("field '") + key + "' must be a string");
    }
    return value->get<std::string>();
}

std::optional<std::string> optional_string(const json& command, const char* key) {
    const json* value = field(command, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        throw std::invalid_argument(std::string("field '") + key + "' must be a string");
    }
    return value->get<std::string>();
}

std::optional<bool> optional_bool(const json& command, const char* key) {
    const json* value = field(command, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_boolean()) {
        throw std::invalid_argument(std::string("field '") + key + "' must be true or false");
    }
    return value->get<bool>();
}

// The value of an integer field. Throws std::invalid_argument when it is not an integer, or is
// outside [lowest, highest].
std::int64_t int_value(const json& value, const char* key, std::int64_t lowest,
                       std::int64_t highest) {
    if (!value.is_number_integer()) {
        throw std::invalid_argument(std::string("field '") + key + "' must be an integer");
    }
    const auto number = value.get<std::int64_t>();
    if (number < lowest || number > highest) {
        throw std::invalid_argument(std::string("field '") + key + "' is out of range");
    }
    return number;
}

std::int64_t required_int(const json& command, const char* key, std::int64_t lowest,
                          std::int64_t highest) {
    const json* value = field(command, key);
    if (value == nullptr) {
        throw std::invalid_argument(std::string("missing field '") + key + "'");
    }
    return int_value(*value, key, lowest, highest);
}

std::int64_t optional_int(const json& command, const char* key, std::int64_t fallback,
                          std::int64_t lowest, std::int64_t highest) {
    const json* value = field(command, key);
    return value == nullptr ? fallback : int_value(*value, key, lowest, highest);
}

// The devices as the reply lists them. index is the position that `mic:<index>` selects.
ordered_json device_array(const std::vector<capture::Device>& devices) {
    ordered_json out = ordered_json::array();
    for (std::size_t i = 0; i < devices.size(); ++i) {
        out.push_back({{"index", i},
                       {"id", devices[i].id},
                       {"name", devices[i].name},
                       {"default", devices[i].is_default}});
    }
    return out;
}

}  // namespace

StdioService::StdioService(std::istream& in, std::ostream& out, ServeHooks hooks)
    : in_(in), out_(out), hooks_(std::move(hooks)) {}

StdioService::~StdioService() {
    try {
        if (session_) {
            stop_session();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "echoir serve: final flush failed: %s\n", e.what());
    }
}

int StdioService::run() {
    std::string line;
    while (std::getline(in_, line)) {
        if (is_blank(line)) {
            continue;
        }
        if (!handle_line(line)) {
            return 0;
        }
    }
    // The end of input stops a running session the way quit does, but sends no reply. A failed
    // flush has already sent its error event.
    try {
        if (session_) {
            stop_session();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "echoir serve: final flush failed: %s\n", e.what());
    }
    return 0;
}

bool StdioService::handle_line(const std::string& line) {
    json command;
    try {
        command = json::parse(line);
    } catch (const std::exception& e) {
        reply_error("", std::string("invalid JSON: ") + e.what());
        return true;
    }
    if (!command.is_object() || !command.contains("cmd") || !command["cmd"].is_string()) {
        reply_error("", "a command is a JSON object with a string cmd");
        return true;
    }
    const std::string cmd = command["cmd"].get<std::string>();
    try {
        if (cmd == "load") {
            load(command);
        } else if (cmd == "start") {
            start(command);
        } else if (cmd == "audio") {
            audio(command);
        } else if (cmd == "flush") {
            running_session("flush").flush();
        } else if (cmd == "reset") {
            running_session("reset").reset();
        } else if (cmd == "clear") {
            running_session("clear").clear();
        } else if (cmd == "stop") {
            running_session("stop");
            stop_session();
        } else if (cmd == "devices") {
            devices();
            return true;  // the reply is the device list, not an ok
        } else if (cmd == "quit") {
            return quit();
        } else {
            throw std::invalid_argument("unknown command '" + cmd + "'");
        }
        reply_ok(cmd);
    } catch (const std::exception& e) {
        reply_error(cmd, e.what());
    }
    return true;
}

bool StdioService::quit() {
    // Quit exits with status 0 even when the final flush fails, since the error is in the reply.
    try {
        if (session_) {
            stop_session();
        }
        reply_ok("quit");
    } catch (const std::exception& e) {
        reply_error("quit", e.what());
    }
    return false;
}

void StdioService::load(const json& command) {
    TranscribeOptions options;
    options.model = required_string(command, "model");
    options.backend = optional_string(command, "backend").value_or("");
    if (!options.backend.empty() && options.backend != "whisper" && options.backend != "qwen") {
        throw std::invalid_argument("backend must be whisper or qwen");
    }
    options.mmproj = optional_string(command, "mmproj").value_or("");
    options.run.vad = optional_string(command, "vad").value_or("");
    options.run.use_gpu = optional_bool(command, "gpu").value_or(true);
    options.run.threads = static_cast<int>(optional_int(command, "threads", 0, 0, kMaxInt));
    if (session_) {
        throw std::runtime_error("a session is running; send stop first");
    }
    // The held models go first, so two models are never in memory at once. A failed load leaves
    // none loaded.
    models_.reset();
    models_ = hooks_.load(options);
}

void StdioService::start(const json& command) {
    if (session_) {
        throw std::runtime_error("a session is already running; send stop first");
    }
    if (!models_) {
        throw std::runtime_error("no model is loaded; send load first");
    }
    const std::string input = required_string(command, "input");
    if (input != "mic" && input != "loopback" && input != "push") {
        throw std::invalid_argument("input must be mic, loopback or push");
    }
    TranscribeOptions options;
    options.run.language = optional_string(command, "language").value_or("auto");
    options.context = optional_string(command, "context").value_or("");
    options.min_chunk_ms =
        static_cast<int>(optional_int(command, "min_chunk_ms", kDefaultMinChunkMs, 0, kMaxInt));
    const std::string policy = optional_string(command, "policy").value_or("");
    if (!policy.empty() && policy != "agreement" && policy != "rollback") {
        throw std::invalid_argument("policy must be agreement or rollback");
    }
    options.policy = policy;

    // A bad language or context fails here, before the device opens. session_config applies the
    // same policy rules as the CLI.
    check_request(*models_->asr, options);
    const SessionConfig config = session_config(options, *models_);

    auto session =
        std::make_unique<Session>(*models_->asr, *models_->vad, config,
                                  [this](const SessionEvent& event) { write_event(event); });
    // The feed opens its device before the session starts, so a bad device fails cleanly.
    std::unique_ptr<capture::CaptureFeed> feed;
    if (input != "push") {
        const capture::Source source =
            input == "loopback" ? capture::Source::loopback : capture::Source::mic;
        check_source_available(source);
        const std::string device_id =
            device_id_for(source, optional_string(command, "device").value_or(""));
        feed = std::make_unique<capture::CaptureFeed>(source, device_id, *session);
    }
    session->start();
    if (feed) {
        try {
            feed->start();
        } catch (...) {
            session->stop();
            throw;
        }
    }
    session_ = std::move(session);
    feed_ = std::move(feed);
    push_input_ = input == "push";
}

void StdioService::audio(const json& command) {
    if (!session_ || !push_input_) {
        throw std::runtime_error("audio needs a session started with input push");
    }
    const std::string encoded = required_string(command, "pcm_s16le_b64");
    const auto rate = required_int(command, "rate", 1, kMaxInt);
    const auto channels = required_int(command, "channels", 1, kMaxInt);

    const std::vector<std::uint8_t> bytes = base64_decode(encoded);
    if (bytes.size() % 2 != 0) {
        throw std::invalid_argument("pcm_s16le_b64 has an odd number of bytes");
    }
    const std::size_t samples = bytes.size() / 2;
    if (samples % static_cast<std::size_t>(channels) != 0) {
        throw std::invalid_argument("pcm_s16le_b64 is not a whole number of frames");
    }
    session_->push(decode_s16le(bytes), static_cast<std::int32_t>(rate),
                   static_cast<std::int32_t>(channels));
}

void StdioService::devices() {
    const capture::DeviceList list = hooks_.devices();
    write_line(dump({{"type", "devices"},
                     {"capture", device_array(list.capture)},
                     {"loopback", device_array(list.loopback)}}));
}

Session& StdioService::running_session(const char* cmd) {
    if (!session_) {
        throw std::runtime_error(std::string(cmd) + " needs a running session; send start first");
    }
    return *session_;
}

void StdioService::stop_session() {
    std::unique_ptr<Session> session = std::move(session_);
    std::unique_ptr<capture::CaptureFeed> feed = std::move(feed_);
    push_input_ = false;

    std::exception_ptr failure;
    try {
        // The feed stops the device first, so no more audio arrives, and pushes the audio it
        // captured. The flush finalizes the segment in progress.
        if (feed) {
            feed->stop();
        }
        session->flush();
    } catch (...) {
        failure = std::current_exception();
    }
    // The device closes before the worker stops, as the CLI does.
    feed.reset();
    session->stop();
    session.reset();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void StdioService::write_line(const std::string& text) {
    std::lock_guard<std::mutex> lock(out_mu_);
    out_ << text << '\n';
    out_.flush();
}

void StdioService::write_event(const SessionEvent& event) {
    const std::string text = event_json(event);
    if (!text.empty()) {
        write_line(text);
    }
}

void StdioService::reply_ok(const std::string& cmd) {
    write_line(dump({{"type", "ok"}, {"cmd", cmd}}));
}

void StdioService::reply_error(const std::string& cmd, const std::string& message) {
    write_line(dump({{"type", "error"}, {"cmd", cmd}, {"message", message}}));
}

}  // namespace echoir::cli
