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
    const std::string text = echoir::stats_json(TickSummary{0.12, 180, 2600}, "qwen3-asr");
    CHECK(nlohmann::json::parse(text) ==
          nlohmann::json::parse(
              R"({"rtf":0.12,"decode_ms":180,"window_ms":2600,"backend":"qwen3-asr"})"));
    CHECK(text.find('\n') == std::string::npos);
}

TEST_CASE("stats json: rtf keeps four decimals") {
    const nlohmann::json value =
        nlohmann::json::parse(echoir::stats_json(TickSummary{0.123456, 1, 1}, "whisper"));
    CHECK(value["rtf"] == 0.1235);
}
