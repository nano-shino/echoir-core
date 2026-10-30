#include "stream/audio_store.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "errors.h"
#include "stream/signal.h"

namespace echoir::stream {
namespace {

constexpr std::int64_t kCellSamples = AudioStore::kPeakCellMs * kSamplesPerMs;

std::int16_t to_s16(float sample) {
    const float scaled = std::round(sample * 32768.0f);
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

void check_range(std::int64_t start_ms, std::int64_t end_ms) {
    if (start_ms < 0) {
        throw std::invalid_argument("start_ms must not be negative");
    }
    if (end_ms < start_ms) {
        throw std::invalid_argument("end_ms must not be before start_ms");
    }
}

}  // namespace

AudioStore::AudioStore(std::int64_t retain_ms) {
    if (retain_ms < 0) {
        throw std::invalid_argument("retain_audio_ms must not be negative");
    }
    ring_.resize(static_cast<std::size_t>(retain_ms * kSamplesPerMs));
}

void AudioStore::append(std::span<const float> pcm) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto size = static_cast<std::int64_t>(ring_.size());
    for (const float sample : pcm) {
        const std::int16_t s = to_s16(sample);
        if (size > 0) {
            ring_[static_cast<std::size_t>(total_ % size)] = s;
        }
        ++total_;
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

void AudioStore::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    total_ = 0;
    cells_.clear();
    cell_fill_ = 0;
}

std::size_t AudioStore::read_audio(std::int64_t start_ms, std::int64_t end_ms, std::int16_t* out,
                                   std::size_t cap) const {
    check_range(start_ms, end_ms);
    std::lock_guard<std::mutex> lock(mu_);
    if (ring_.empty()) {
        throw StateError("the session keeps no audio (retain_audio_ms is 0)");
    }
    // Clamped before scaling, so a far end such as INT64_MAX does not overflow.
    const std::int64_t newest_ms = total_ / kSamplesPerMs + 1;
    if (start_ms >= newest_ms) {
        return 0;
    }
    const std::int64_t start = start_ms * kSamplesPerMs;
    const std::int64_t end = std::min(std::min(end_ms, newest_ms) * kSamplesPerMs, total_);
    if (end <= start) {
        return 0;
    }
    const auto size = static_cast<std::int64_t>(ring_.size());
    if (start < total_ - size) {
        throw StateError("the audio of that range is no longer kept");
    }
    const auto count = static_cast<std::size_t>(end - start);
    if (out == nullptr) {
        return count;
    }
    if (cap < count) {
        throw std::invalid_argument("the output buffer is too small for the range");
    }
    for (std::int64_t n = start; n < end; ++n) {
        out[static_cast<std::size_t>(n - start)] = ring_[static_cast<std::size_t>(n % size)];
    }
    return count;
}

std::size_t AudioStore::read_peaks(std::int64_t start_ms, std::int64_t end_ms,
                                   std::int64_t bucket_ms, float* out, std::size_t cap) const {
    if (bucket_ms <= 0 || bucket_ms % kPeakCellMs != 0) {
        throw std::invalid_argument("bucket_ms must be a positive multiple of 10");
    }
    check_range(start_ms, end_ms);
    std::lock_guard<std::mutex> lock(mu_);
    const auto cells = static_cast<std::int64_t>(cells_.size() / 2);
    const std::int64_t first = start_ms / kPeakCellMs;
    // Rounded up without adding to end_ms, which may be INT64_MAX.
    const std::int64_t last =
        std::min(end_ms / kPeakCellMs + (end_ms % kPeakCellMs != 0 ? 1 : 0), cells);
    if (last <= first) {
        return 0;
    }
    const std::int64_t per_bucket = bucket_ms / kPeakCellMs;
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
