// The speed figures of a STATS event: the sums over ticks, and the JSON object that carries them.
#include <cstdint>
#include <string>

#include "doctest.h"
#include "json.hpp"
#include "stream/tick_stats.h"

using echoir::TickStats;
using echoir::TickSummary;

TEST_CASE("tick stats: rtf is decode time over window audio, the others are per-tick means") {
    TickStats stats;
    stats.add(10.0, 1000.0);
    stats.add(30.0, 3000.0);
    CHECK(stats.ticks() == 2);
    const TickSummary summary = stats.summary();
    CHECK(summary.rtf == doctest::Approx(0.01));
    CHECK(summary.decode_ms == 20);
    CHECK(summary.window_ms == 2000);
}

TEST_CASE("tick stats: load is decode time over new audio, speech rtf only over speech ticks") {
    TickStats stats;
    // A silent tick: 500 ms new, nothing decoded as speech, 5 ms of VAD.
    stats.add(5.0, 1000.0, 500.0, 0.0);
    // Two speech ticks: 300 ms of decode for 500 ms of new audio each.
    stats.add(300.0, 3000.0, 500.0, 2500.0);
    stats.add(300.0, 3500.0, 500.0, 3000.0);
    const TickSummary summary = stats.summary();
    CHECK(summary.ticks == 3);
    CHECK(summary.new_ms == 500);
    CHECK(summary.speech_ms == 1833);
    CHECK(summary.decode_max_ms == 300);
    CHECK(summary.load == doctest::Approx(605.0 / 1500.0));
    CHECK(summary.speech_rtf == doctest::Approx(600.0 / 6500.0));
}

TEST_CASE("tick stats: no new audio or no speech gives load and speech rtf 0") {
    TickStats stats;
    stats.add(20.0, 1000.0, 0.0, 0.0);
    CHECK(stats.summary().load == 0.0);
    CHECK(stats.summary().speech_rtf == 0.0);
}

TEST_CASE("tick stats: means round to the nearest millisecond") {
    TickStats stats;
    stats.add(1.4, 2.6);
    const TickSummary summary = stats.summary();
    CHECK(summary.decode_ms == 1);
    CHECK(summary.window_ms == 3);
}

TEST_CASE("tick stats: an empty window gives rtf 0, and reset starts a new run") {
    TickStats stats;
    stats.add(5.0, 0.0);
    CHECK(stats.summary().rtf == 0.0);
    stats.reset();
    CHECK(stats.ticks() == 0);
    stats.add(4.0, 8.0);
    CHECK(stats.summary().rtf == doctest::Approx(0.5));
    CHECK(stats.summary().decode_ms == 4);
}

TEST_CASE("stats json: one object with rtf, decode_ms, window_ms and backend") {
    const std::string text = echoir::stats_json(
        TickSummary{0.12, 180, 2600, 10, 1000, 800, 400, 0.18, 0.2, 35}, "qwen3-asr");
    CHECK(nlohmann::json::parse(text) ==
          nlohmann::json::parse(
              R"({"rtf":0.12,"decode_ms":180,"window_ms":2600,"ticks":10,"new_ms":1000,)"
              R"("speech_ms":800,"decode_max_ms":400,"load":0.18,"speech_rtf":0.2,)"
              R"("wait_ms":35,"backend":"qwen3-asr"})"));
    CHECK(text.find('\n') == std::string::npos);
}

TEST_CASE("tick stats: wait_ms is the mean wait per tick and stays out of rtf") {
    TickStats stats;
    stats.add(100.0, 1000.0, 500.0, 0.0, 30.0);
    stats.add(100.0, 1000.0, 500.0, 0.0, 0.0);
    const TickSummary summary = stats.summary();
    CHECK(summary.wait_ms == 15);
    CHECK(summary.rtf == doctest::Approx(0.1));
    CHECK(summary.decode_ms == 100);
    stats.reset();
    stats.add(100.0, 1000.0);
    CHECK(stats.summary().wait_ms == 0);
}

TEST_CASE("stats json: rtf keeps four decimals") {
    const nlohmann::json value =
        nlohmann::json::parse(echoir::stats_json(TickSummary{0.123456, 1, 1}, "whisper"));
    CHECK(value["rtf"] == 0.1235);
}
