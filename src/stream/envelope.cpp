#include "stream/envelope.h"

#include <algorithm>
#include <stdexcept>

#include "stream/signal.h"

namespace echoir::stream {
namespace {

constexpr std::int64_t kCellSamples = Envelope::kCellMs * kSamplesPerMs;

}  // namespace

void Envelope::append(std::span<const std::int16_t> samples) {
    for (const std::int16_t s : samples) {
        if (cell_fill_ == 0) {
            cell_min_ = s;
            cell_max_ = s;
        } else {
            cell_min_ = std::min(cell_min_, s);
            cell_max_ = std::max(cell_max_, s);
        }
        if (++cell_fill_ == kCellSamples) {
            cells_.push_back(cell_min_);
            cells_.push_back(cell_max_);
            cell_fill_ = 0;
        }
    }
}

void Envelope::append_silence(std::int64_t n) {
    // The cell being filled takes silence one sample at a time; whole cells of silence are added
    // in one step, so a long gap costs little.
    static constexpr std::int16_t kZero[1] = {0};
    while (n > 0 && cell_fill_ != 0) {
        append(kZero);
        --n;
    }
    const std::int64_t whole = n / kCellSamples;
    cells_.insert(cells_.end(), static_cast<std::size_t>(whole * 2), std::int16_t{0});
    for (std::int64_t i = whole * kCellSamples; i < n; ++i) {
        append(kZero);
    }
}

void Envelope::clear() {
    cells_.clear();
    cell_fill_ = 0;
}

std::size_t Envelope::read(std::int64_t start_ms, std::int64_t end_ms, std::int64_t bucket_ms,
                           float* out, std::size_t cap, std::int64_t limit_cells) const {
    if (bucket_ms <= 0 || bucket_ms % kCellMs != 0) {
        throw std::invalid_argument("bucket_ms must be a positive multiple of 10");
    }
    if (start_ms < 0) {
        throw std::invalid_argument("start_ms must not be negative");
    }
    if (end_ms < start_ms) {
        throw std::invalid_argument("end_ms must not be before start_ms");
    }
    const std::int64_t cells = std::min(this->cells(), limit_cells);
    const std::int64_t first = start_ms / kCellMs;
    // Rounded up without adding to end_ms, which may be INT64_MAX.
    const std::int64_t last = std::min(end_ms / kCellMs + (end_ms % kCellMs != 0 ? 1 : 0), cells);
    if (last <= first) {
        return 0;
    }
    const std::int64_t per_bucket = bucket_ms / kCellMs;
    const auto count = static_cast<std::size_t>((last - first + per_bucket - 1) / per_bucket);
    if (out == nullptr) {
        return count;
    }
    if (cap < count) {
        throw std::invalid_argument("the output buffer is too small for the range");
    }
    for (std::size_t b = 0; b < count; ++b) {
        const std::int64_t from = first + static_cast<std::int64_t>(b) * per_bucket;
        const std::int64_t to = std::min(from + per_bucket, last);
        std::int16_t lo = cells_[static_cast<std::size_t>(from * 2)];
        std::int16_t hi = cells_[static_cast<std::size_t>(from * 2 + 1)];
        for (std::int64_t c = from + 1; c < to; ++c) {
            lo = std::min(lo, cells_[static_cast<std::size_t>(c * 2)]);
            hi = std::max(hi, cells_[static_cast<std::size_t>(c * 2 + 1)]);
        }
        out[b * 2] = static_cast<float>(lo) / 32768.0f;
        out[b * 2 + 1] = static_cast<float>(hi) / 32768.0f;
    }
    return count;
}

}  // namespace echoir::stream
