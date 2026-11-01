// Percentiles and the latency of streamed text, on times given by hand.
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "doctest.h"
#include "latency.h"

using echoir::cli::PushTime;
using echoir::cli::SegmentLatencies;
using echoir::cli::TextTime;

TEST_CASE("percentile: nearest rank over the sorted values") {
    const std::vector<double> ten{9, 1, 10, 2, 8, 3, 7, 4, 6, 5};  // unsorted on purpose
    CHECK(echoir::cli::percentile(ten, 50.0) == 5.0);
    CHECK(echoir::cli::percentile(ten, 95.0) == 10.0);
    CHECK(echoir::cli::percentile(ten, 0.0) == 1.0);
    CHECK(echoir::cli::percentile(ten, 100.0) == 10.0);
    CHECK(echoir::cli::percentile(ten, 10.0) == 1.0);
    CHECK(echoir::cli::percentile(ten, 11.0) == 2.0);
}

TEST_CASE("percentile: one value is every percentile, and no values is no percentile") {
    CHECK(echoir::cli::percentile({42.0}, 95.0) == 42.0);
    CHECK(echoir::cli::percentile({42.0}, 0.0) == 42.0);
    const std::optional<double> none = echoir::cli::percentile({}, 50.0);
    CHECK(!none.has_value());
}

TEST_CASE("percentile: a percentile outside 0 to 100 is refused") {
    CHECK_THROWS_AS(echoir::cli::percentile({1.0}, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(echoir::cli::percentile({1.0}, 100.5), std::invalid_argument);
}

TEST_CASE("segment latency: a text is measured from the first push that reaches its end") {
    // Audio up to 500 ms was pushed at 1000 ms, up to 1000 ms at 1600 ms, up to 1500 ms at 2100 ms.
    const std::vector<PushTime> pushes{{500, 1000.0}, {1000, 1600.0}, {1500, 2100.0}};
    // Segment 1's first partial covers 500 ms and is delivered at 1100 ms: 100 ms. Its second
    // partial is not a first one. Segment 2's first partial covers 1500 ms and comes at 2150 ms.
    const std::vector<TextTime> partials{{1, 500, 1100.0}, {1, 1000, 1650.0}, {2, 1500, 2150.0}};
    // Segment 1 ends at 900 ms, which the push at 1600 ms reaches: delivered at 1700 ms, 100 ms.
    // Segment 2 ends past the last push, so that push counts: delivered at 2200 ms, 100 ms.
    const std::vector<TextTime> finals{{1, 900, 1700.0}, {2, 2000, 2200.0}};
    const SegmentLatencies got = echoir::cli::segment_latencies(pushes, partials, finals);
    CHECK(got.first_partial_ms == std::vector<double>{100.0, 50.0});
    CHECK(got.final_ms == std::vector<double>{100.0, 100.0});
}

TEST_CASE("segment latency: a text that arrives before its audio's push counts as zero") {
    const std::vector<PushTime> pushes{{1000, 1600.0}};
    const SegmentLatencies got = echoir::cli::segment_latencies(pushes, {{7, 1000, 1500.0}}, {});
    CHECK(got.first_partial_ms == std::vector<double>{0.0});
    CHECK(got.final_ms.empty());
}

TEST_CASE("segment latency: no pushes or no texts give no latencies") {
    const SegmentLatencies got = echoir::cli::segment_latencies({}, {}, {});
    CHECK(got.first_partial_ms.empty());
    CHECK(got.final_ms.empty());
}
