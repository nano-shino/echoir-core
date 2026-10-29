// The stdio service, driven through string streams. The models are the scripted fakes from
// tests/support, so no model file is needed.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/live_fakes.h"
#include "backends/asr_backend.h"
#include "base64.h"
#include "doctest.h"
#include "json.hpp"
#include "serve.h"

namespace {

namespace tf = echoir::test_support;
using echoir::capture::Device;
using echoir::capture::DeviceList;
using echoir::cli::LoadedModels;
using echoir::cli::ServeHooks;
using echoir::cli::StdioService;
using echoir::cli::TranscribeOptions;
using json = nlohmann::json;

// Standard base64 with padding, the encoding the audio command takes.
std::string base64_encode(const std::vector<std::uint8_t>& in) {
    static const char* const kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t rest = in.size() - i;
        std::uint32_t group = static_cast<std::uint32_t>(in[i]) << 16;
        if (rest > 1) {
            group |= static_cast<std::uint32_t>(in[i + 1]) << 8;
        }
        if (rest > 2) {
            group |= in[i + 2];
        }
        out += kAlphabet[(group >> 18) & 63];
        out += kAlphabet[(group >> 12) & 63];
        out += rest > 1 ? kAlphabet[(group >> 6) & 63] : '=';
        out += rest > 2 ? kAlphabet[group & 63] : '=';
    }
    return out;
}

// Little-endian 16-bit PCM of float samples in [-1, 1].
std::vector<std::uint8_t> s16le_bytes(const std::vector<float>& samples) {
    std::vector<std::uint8_t> out;
    out.reserve(samples.size() * 2);
    for (const float sample : samples) {
        const auto value =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
        const auto bits = static_cast<std::uint16_t>(value);
        out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
        out.push_back(static_cast<std::uint8_t>(bits >> 8));
    }
    return out;
}

json audio_command(const std::vector<float>& samples) {
    return json{{"cmd", "audio"},
                {"pcm_s16le_b64", base64_encode(s16le_bytes(samples))},
                {"rate", 16000},
                {"channels", 1}};
}

// A backend that reports prefix support, so rollback is allowed. It never decodes anything.
class PrefixCapableBackend final : public echoir::IAsrBackend {
public:
    echoir::DecodeResult decode(const echoir::DecodeRequest&) override { return {}; }
    std::string name() const override { return "fake-prefix"; }
    bool supports_prefix() const override { return true; }
};

// Hooks that load the scripted VAD and backend, and list no devices.
ServeHooks fake_hooks() {
    ServeHooks hooks;
    hooks.load = [](const TranscribeOptions&) {
        return LoadedModels{"whisper", std::make_unique<tf::LiveVad>(),
                            std::make_unique<tf::LiveBackend>()};
    };
    hooks.devices = [] { return DeviceList{}; };
    return hooks;
}

struct Run {
    int status = -1;
    std::vector<json> lines;  // every output line, parsed
};

// Feeds the script to a service and parses everything it writes.
Run run_script(const std::string& script, ServeHooks hooks) {
    std::istringstream in(script);
    std::ostringstream out;
    Run run;
    {
        StdioService service(in, out, std::move(hooks));
        run.status = service.run();
    }
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) {
        run.lines.push_back(json::parse(line));
    }
    return run;
}

// Replies are the lines with a type of ok, error or devices. Events have no cmd, except that an
// error reply has one.
bool is_reply(const json& line) {
    const std::string type = line.at("type").get<std::string>();
    return type == "ok" || type == "devices" || (type == "error" && line.contains("cmd"));
}

// The (type, cmd) pair of each reply, in order. A devices reply has no cmd, so it gets "".
std::vector<std::pair<std::string, std::string>> reply_kinds(const std::vector<json>& lines) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& line : lines) {
        if (is_reply(line)) {
            const std::string cmd = line.contains("cmd") ? line.at("cmd").get<std::string>() : "";
            out.emplace_back(line.at("type").get<std::string>(), cmd);
        }
    }
    return out;
}

std::size_t count_type(const std::vector<json>& lines, const std::string& type) {
    return static_cast<std::size_t>(std::count_if(
        lines.begin(), lines.end(), [&](const json& l) { return l.at("type") == type; }));
}

std::string as_text(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

}  // namespace

TEST_CASE("the push script gets one ok per command, and finals before stop's ok") {
    std::vector<std::string> commands;
    std::string script;
    const auto send = [&](const json& command) {
        script += command.dump() + "\n";
        commands.push_back(command.at("cmd").get<std::string>());
    };

    send({{"cmd", "load"}, {"model", "whisper-tiny-en"}});
    send({{"cmd", "start"}, {"input", "push"}});
    // One second of tone, two of silence, one of tone and two of silence, in 100 ms blocks.
    std::size_t phase = 0;
    const std::pair<int, bool> plan[] = {{10, true}, {20, false}, {10, true}, {20, false}};
    for (const auto& [blocks, speech] : plan) {
        for (int b = 0; b < blocks; ++b) {
            send(audio_command(tf::tone(tf::kBlockSamples, speech, phase)));
        }
    }
    send({{"cmd", "flush"}});
    send({{"cmd", "stop"}});
    send({{"cmd", "quit"}});

    const Run run = run_script(script, fake_hooks());
    CHECK(run.status == 0);

    const auto kinds = reply_kinds(run.lines);
    REQUIRE(kinds.size() == commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        CHECK(kinds[i].first == "ok");
        CHECK(kinds[i].second == commands[i]);
    }

    // Every final is written before stop's reply, and there are at least two of them.
    std::size_t stop_reply = 0;
    for (std::size_t i = 0; i < run.lines.size(); ++i) {
        if (is_reply(run.lines[i]) && run.lines[i].at("cmd") == "stop") {
            stop_reply = i;
        }
    }
    std::size_t finals_before_stop = 0;
    std::size_t finals_after_stop = 0;
    for (std::size_t i = 0; i < run.lines.size(); ++i) {
        if (run.lines[i].at("type") == "final") {
            (i < stop_reply ? finals_before_stop : finals_after_stop)++;
        }
    }
    CHECK(finals_before_stop >= 2);
    CHECK(finals_after_stop == 0);
}

TEST_CASE("clear and reset write their events before their replies") {
    std::string script;
    script += R"({"cmd":"load","model":"whisper-tiny-en"})"
              "\n";
    script += R"({"cmd":"start","input":"push"})"
              "\n";
    std::size_t phase = 0;
    for (int b = 0; b < 10; ++b) {
        script += audio_command(tf::tone(tf::kBlockSamples, true, phase)).dump() + "\n";
    }
    script += R"({"cmd":"clear"})"
              "\n";
    script += R"({"cmd":"reset"})"
              "\n";
    script += R"({"cmd":"flush"})"
              "\n";
    script += R"({"cmd":"stop"})"
              "\n";
    script += R"({"cmd":"quit"})"
              "\n";
    const Run run = run_script(script, fake_hooks());
    CHECK(run.status == 0);

    const std::vector<std::pair<std::string, std::string>> tail = {
        {"ok", "clear"}, {"ok", "reset"}, {"ok", "flush"}, {"ok", "stop"}, {"ok", "quit"}};
    const auto kinds = reply_kinds(run.lines);
    REQUIRE(kinds.size() >= tail.size());
    CHECK(std::vector<std::pair<std::string, std::string>>(kinds.end() - 5, kinds.end()) == tail);

    // The cleared event comes before the ok of clear, and the reset's finals before its ok.
    std::size_t cleared = run.lines.size();
    std::size_t clear_reply = run.lines.size();
    std::size_t reset_reply = run.lines.size();
    for (std::size_t i = 0; i < run.lines.size(); ++i) {
        const json& line = run.lines[i];
        if (line.at("type") == "cleared") {
            cleared = i;
        } else if (is_reply(line) && line.at("cmd") == "clear") {
            clear_reply = i;
        } else if (is_reply(line) && line.at("cmd") == "reset") {
            reset_reply = i;
        }
    }
    CHECK(cleared < clear_reply);
    CHECK(clear_reply < reset_reply);
}

TEST_CASE("a session can be started again after stop") {
    std::string script;
    script += R"({"cmd":"load","model":"whisper-tiny-en"})"
              "\n";
    script += R"({"cmd":"start","input":"push"})"
              "\n";
    script += R"({"cmd":"stop"})"
              "\n";
    script += R"({"cmd":"start","input":"push"})"
              "\n";
    script += R"({"cmd":"quit"})"
              "\n";
    const Run run = run_script(script, fake_hooks());
    CHECK(run.status == 0);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"ok", "load"}, {"ok", "start"}, {"ok", "stop"}, {"ok", "start"}, {"ok", "quit"}};
    CHECK(reply_kinds(run.lines) == expected);
}

TEST_CASE("bad commands get error replies, and the service keeps going") {
    // The 3-byte audio is odd; the bad base64 has a '!'; 3 samples cannot be 4-channel frames.
    const std::string script =
        R"({"cmd":"audio","pcm_s16le_b64":"AAAA","rate":16000,"channels":1})"
        "\n"
        R"({"cmd":"start","input":"push"})"
        "\n"
        R"({"cmd":"stop"})"
        "\n"
        R"({"cmd":"nope"})"
        "\n"
        "not json\n"
        R"([1,2,3])"
        "\n"
        R"({"cmd":"load","model":"whisper-tiny-en"})"
        "\n"
        R"({"cmd":"start","input":"push","min_chunk_ms":-1})"
        "\n"
        R"({"cmd":"start","input":"push"})"
        "\n"
        R"({"cmd":"audio","pcm_s16le_b64":"AAA!","rate":16000,"channels":1})"
        "\n"
        R"({"cmd":"audio","pcm_s16le_b64":"AAAA","rate":16000,"channels":1})"
        "\n"
        R"({"cmd":"audio","pcm_s16le_b64":"AAAAAAAA","rate":16000,"channels":4})"
        "\n"
        R"({"cmd":"audio","pcm_s16le_b64":"AAAA","rate":0,"channels":1})"
        "\n"
        R"({"cmd":"quit"})"
        "\n";
    const Run run = run_script(script, fake_hooks());
    CHECK(run.status == 0);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"error", "audio"}, {"error", "start"}, {"error", "stop"},  {"error", "nope"},
        {"error", ""},      {"error", ""},      {"ok", "load"},     {"error", "start"},
        {"ok", "start"},    {"error", "audio"}, {"error", "audio"}, {"error", "audio"},
        {"error", "audio"}, {"ok", "quit"}};
    CHECK(reply_kinds(run.lines) == expected);
    for (const json& line : run.lines) {
        if (line.at("type") == "error") {
            CHECK(!line.at("message").get<std::string>().empty());
        }
    }
}

TEST_CASE("the audio of a push session with a bad base64 or odd byte count is refused") {
    const std::string script = R"({"cmd":"load","model":"whisper-tiny-en"})"
                               "\n"
                               R"({"cmd":"start","input":"push"})"
                               "\n"
                               R"({"cmd":"audio","pcm_s16le_b64":"AAA!","rate":16000,"channels":1})"
                               "\n"
                               R"({"cmd":"audio","pcm_s16le_b64":"AAAA","rate":16000,"channels":1})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n";
    const Run run = run_script(script, fake_hooks());
    const std::vector<json> replies = [&] {
        std::vector<json> out;
        for (const json& line : run.lines) {
            if (is_reply(line)) {
                out.push_back(line);
            }
        }
        return out;
    }();
    REQUIRE(replies.size() == 5);
    CHECK(replies[2].at("message").get<std::string>().find("base64") != std::string::npos);
    CHECK(replies[3].at("message").get<std::string>().find("odd") != std::string::npos);
}

TEST_CASE("a load that throws gets an error reply with its message") {
    ServeHooks hooks = fake_hooks();
    hooks.load = [](const TranscribeOptions&) -> LoadedModels {
        throw std::runtime_error("model file is missing");
    };
    const Run run = run_script(R"({"cmd":"load","model":"whisper-tiny-en"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               hooks);
    CHECK(run.status == 0);
    REQUIRE(run.lines.size() == 2);
    CHECK(run.lines[0].at("type") == "error");
    CHECK(run.lines[0].at("cmd") == "load");
    CHECK(run.lines[0].at("message") == "model file is missing");
    CHECK(run.lines[1].at("type") == "ok");
}

TEST_CASE("load passes its fields on, and refuses an unknown backend before loading") {
    std::vector<TranscribeOptions> seen;
    ServeHooks hooks = fake_hooks();
    hooks.load = [&seen](const TranscribeOptions& options) {
        seen.push_back(options);
        return LoadedModels{"whisper", std::make_unique<tf::LiveVad>(),
                            std::make_unique<tf::LiveBackend>()};
    };
    const Run run = run_script(
        R"({"cmd":"load","backend":"whisper","model":"whisper-tiny-en","vad":"silero-v6.2.0",)"
        R"("gpu":false,"threads":3})"
        "\n"
        R"({"cmd":"load","model":"whisper-tiny-en","backend":"foo"})"
        "\n"
        R"({"cmd":"quit"})"
        "\n",
        hooks);
    CHECK(run.status == 0);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].model == "whisper-tiny-en");
    CHECK(seen[0].backend == "whisper");
    CHECK(seen[0].run.vad == "silero-v6.2.0");
    CHECK(seen[0].run.use_gpu == false);
    CHECK(seen[0].run.threads == 3);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"ok", "load"}, {"error", "load"}, {"ok", "quit"}};
    CHECK(reply_kinds(run.lines) == expected);
}

TEST_CASE("devices replies with the capture and loopback lists") {
    ServeHooks hooks = fake_hooks();
    hooks.devices = [] {
        DeviceList list;
        list.capture = {Device{"0a1b", "Built-in Mic", true}, Device{"2c3d", "USB Audio", false}};
        return list;
    };
    const Run run = run_script(R"({"cmd":"devices"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               hooks);
    CHECK(run.status == 0);
    REQUIRE(run.lines.size() == 2);
    const json expected =
        json::parse(R"({"type":"devices","capture":[)"
                    R"({"index":0,"id":"0a1b","name":"Built-in Mic","default":true},)"
                    R"({"index":1,"id":"2c3d","name":"USB Audio","default":false}],)"
                    R"("loopback":[]})");
    CHECK(run.lines[0] == expected);
    CHECK(run.lines[1] == json::parse(R"({"type":"ok","cmd":"quit"})"));
}

TEST_CASE("end of input stops a running session, writing its finals, with no reply") {
    std::string script;
    script += R"({"cmd":"load","model":"whisper-tiny-en"})"
              "\n";
    script += R"({"cmd":"start","input":"push"})"
              "\n";
    std::size_t phase = 0;
    for (int b = 0; b < 10; ++b) {
        script += audio_command(tf::tone(tf::kBlockSamples, true, phase)).dump() + "\n";
    }
    // No flush, stop or quit: the one second of tone is still the segment in progress.
    const Run run = run_script(script, fake_hooks());
    CHECK(run.status == 0);

    const auto kinds = reply_kinds(run.lines);
    CHECK(kinds.size() == 12);  // load, start and ten audio commands
    for (const auto& kind : kinds) {
        CHECK(kind.first == "ok");
    }
    REQUIRE(!run.lines.empty());
    CHECK(run.lines.back().at("type") == "final");
    CHECK(count_type(run.lines, "final") >= 1);
}

TEST_CASE("rollback is refused on a backend without decoder prefix support") {
    const Run run = run_script(R"({"cmd":"load","model":"whisper-tiny-en"})"
                               "\n"
                               R"({"cmd":"start","input":"push","policy":"rollback"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               fake_hooks());
    CHECK(run.status == 0);
    REQUIRE(run.lines.size() == 3);
    CHECK(run.lines[1].at("type") == "error");
    CHECK(run.lines[1].at("cmd") == "start");
    CHECK(run.lines[1].at("message").get<std::string>().find("rollback") != std::string::npos);
    CHECK(run.lines[2].at("type") == "ok");
}

TEST_CASE("rollback starts on a backend with decoder prefix support") {
    ServeHooks hooks = fake_hooks();
    hooks.load = [](const TranscribeOptions&) {
        return LoadedModels{"qwen", std::make_unique<tf::LiveVad>(),
                            std::make_unique<PrefixCapableBackend>()};
    };
    const Run run = run_script(R"({"cmd":"load","model":"qwen3-asr-0.6b-q8_0"})"
                               "\n"
                               R"({"cmd":"start","input":"push","policy":"rollback"})"
                               "\n"
                               R"({"cmd":"stop"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               hooks);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"ok", "load"}, {"ok", "start"}, {"ok", "stop"}, {"ok", "quit"}};
    CHECK(reply_kinds(run.lines) == expected);
}

TEST_CASE("start rejects a policy name it does not know, and a capture it cannot open") {
    const Run run = run_script(R"({"cmd":"load","model":"whisper-tiny-en"})"
                               "\n"
                               R"({"cmd":"start","input":"push","policy":"fast"})"
                               "\n"
                               R"({"cmd":"start","input":"sideways"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               fake_hooks());
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"ok", "load"}, {"error", "start"}, {"error", "start"}, {"ok", "quit"}};
    CHECK(reply_kinds(run.lines) == expected);
}

#ifndef _WIN32
// Loopback is WASAPI, which exists on Windows only. Elsewhere the start is refused before any
// device opens.
TEST_CASE("loopback is refused off Windows") {
    const Run run = run_script(R"({"cmd":"load","model":"whisper-tiny-en"})"
                               "\n"
                               R"({"cmd":"start","input":"loopback"})"
                               "\n"
                               R"({"cmd":"quit"})"
                               "\n",
                               fake_hooks());
    REQUIRE(run.lines.size() == 3);
    CHECK(run.lines[1].at("type") == "error");
    CHECK(run.lines[1].at("message").get<std::string>().find("WASAPI") != std::string::npos);
}
#endif

TEST_CASE("base64 decodes RFC 4648 vectors and rejects malformed text") {
    using echoir::cli::base64_decode;
    CHECK(as_text(base64_decode("")).empty());
    CHECK(as_text(base64_decode("Zg==")) == "f");
    CHECK(as_text(base64_decode("Zm8=")) == "fo");
    CHECK(as_text(base64_decode("Zm9v")) == "foo");
    CHECK(as_text(base64_decode("Zm9vYg==")) == "foob");
    CHECK(as_text(base64_decode("Zm9vYmE=")) == "fooba");
    CHECK(as_text(base64_decode("Zm9vYmFy")) == "foobar");
    // The padding must be at the end and must not hide non-zero bits.
    CHECK_THROWS_AS(base64_decode("Zg"), std::invalid_argument);        // no padding
    CHECK_THROWS_AS(base64_decode("Zg="), std::invalid_argument);       // one pad where two go
    CHECK_THROWS_AS(base64_decode("Zh=="), std::invalid_argument);      // non-zero pad bits
    CHECK_THROWS_AS(base64_decode("Zm8=Zm8="), std::invalid_argument);  // padding in the middle
    CHECK_THROWS_AS(base64_decode("Zm=v"), std::invalid_argument);      // pad then data
    CHECK_THROWS_AS(base64_decode("=Zm8"), std::invalid_argument);      // pad first
    CHECK_THROWS_AS(base64_decode("Zm9 "), std::invalid_argument);      // whitespace
    CHECK_THROWS_AS(base64_decode("Zm9*"), std::invalid_argument);      // outside the alphabet
}
