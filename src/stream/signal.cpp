#include "stream/signal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace echoir::stream {

std::size_t find_quietest_point(std::span<const float> samples, std::size_t lo, std::size_t hi) {
    if (lo > hi || hi > samples.size() || hi - lo < kQuietBlock) {
        throw std::invalid_argument(
            "find_quietest_point: need lo <= hi <= size and hi - lo >= kQuietBlock");
    }

    // Separate statements stop the compiler fusing multiply and add (FMA), which rounds differently
    // and could flip near-equal energy comparisons.
    double energy = 0.0;
    for (std::size_t k = lo; k < lo + kQuietBlock; ++k) {
        const double v = samples[k];
        const double sq = v * v;
        energy += sq;
    }

    double min_energy = energy;
    std::size_t min_start = lo;
    // A block starting at s covers [s, s + kQuietBlock). The last block start that fits is
    // hi - kQuietBlock, so the loop runs while s + kQuietBlock <= hi.
    for (std::size_t s = lo + 1; s + kQuietBlock <= hi; ++s) {
        const double outgoing = samples[s - 1];
        const double incoming = samples[s + kQuietBlock - 1];
        const double out_sq = outgoing * outgoing;
        const double in_sq = incoming * incoming;
        energy += in_sq - out_sq;
        // Strict <: on a tie the earliest block wins.
        if (energy < min_energy) {
            min_energy = energy;
            min_start = s;
        }
    }
    return min_start + kQuietBlock / 2;
}

namespace {

// The cut at the latest pause of [start, stop) that reaches into [lo, hi], if there is one.
std::optional<std::size_t> find_pause_cut(std::span<const float> samples, std::size_t start,
                                          std::size_t stop, std::size_t lo, std::size_t hi) {
    if (lo > hi || stop <= start) {
        return std::nullopt;
    }
    const std::size_t frames = (stop - start) / kPauseFrame;
    if (frames == 0) {
        return std::nullopt;
    }
    std::vector<double> energy(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (std::size_t k = start + f * kPauseFrame; k < start + (f + 1) * kPauseFrame; ++k) {
            const double v = samples[k];
            const double sq = v * v;
            sum += sq;
        }
        energy[f] = sum;
    }
    std::vector<double> sorted = energy;
    const auto loud_at = sorted.begin() + static_cast<std::ptrdiff_t>(frames * 95 / 100);
    std::nth_element(sorted.begin(), loud_at, sorted.end());
    const double loud = *loud_at;

    std::optional<std::size_t> cut;
    std::size_t run = 0;  // quiet frames that end at frame f
    for (std::size_t f = 0; f <= frames; ++f) {
        // Strict <: when the loud level is 0, nothing is quiet below it.
        if (f < frames && energy[f] * kPauseDepth < loud) {
            ++run;
            continue;
        }
        if (run * kPauseFrame >= kMinPause) {
            const std::size_t first = start + (f - run) * kPauseFrame;
            const std::size_t last = start + f * kPauseFrame;
            if (last > lo && first <= hi) {
                cut = std::clamp(first + (last - first) / 2, lo, hi);
            }
        }
        run = 0;
    }
    return cut;
}

}  // namespace

std::size_t find_cut(std::span<const float> samples, std::size_t start, std::size_t cap,
                     std::size_t end, std::size_t search) {
    // cap - search can underflow when the cap is near the start of the input, so it is
    // compared first. A search that would begin before start begins at start.
    const std::size_t from = cap > search ? cap - search : 0;
    const std::size_t lo = std::max(start, from);
    const std::size_t hi = end >= kQuietBlock ? std::min(end - kQuietBlock, cap) : 0;
    if (lo <= hi && hi <= samples.size()) {
        const std::size_t stop = std::min({cap, end, samples.size()});
        if (auto cut = find_pause_cut(samples, start, stop, start + kMinCutLead, hi)) {
            return *cut;
        }
    }
    return find_quietest_point(samples, lo, hi);
}

double rms(std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const float x : samples) {
        const double v = x;
        const double sq = v * v;
        sum += sq;
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

}  // namespace echoir::stream
