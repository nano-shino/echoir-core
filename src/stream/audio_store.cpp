#include "stream/audio_store.h"

#include <algorithm>
#include <stdexcept>

#include "errors.h"
#include "stream/signal.h"

namespace echoir::stream {
namespace {

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
    scratch_.resize(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const std::int16_t s = to_s16(pcm[i]);
        scratch_[i] = s;
        if (size > 0) {
            ring_[static_cast<std::size_t>(total_ % size)] = s;
        }
        ++total_;
    }
    envelope_.append(scratch_);
}

void AudioStore::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    total_ = 0;
    envelope_.clear();
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
    std::lock_guard<std::mutex> lock(mu_);
    return envelope_.read(start_ms, end_ms, bucket_ms, out, cap);
}

}  // namespace echoir::stream
