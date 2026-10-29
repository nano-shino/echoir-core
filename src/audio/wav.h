// WAV input and output. Reading goes through miniaudio's WAV decoder, which is dr_wav inside
// miniaudio.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace echoir::audio {

// Interleaved f32 PCM at any rate and channel count.
struct PcmBuffer {
    std::vector<float> samples;
    std::uint32_t rate = 0;      // Hz
    std::uint32_t channels = 0;  // samples per frame
};

// Reads a WAV file at its own rate and channel count, as f32 in [-1, 1]. Integer and float
// PCM are both accepted. Throws std::runtime_error if the file cannot be read or decoded.
PcmBuffer read_wav(const std::filesystem::path& path);

// Decodes a WAV held in memory, such as the bytes read from standard input. Throws
// std::runtime_error if the bytes are not a WAV file.
PcmBuffer read_wav(std::span<const std::uint8_t> bytes);

// Reads a WAV file a chunk at a time, so a long file never sits in memory whole.
class WavReader {
public:
    // Throws std::runtime_error if the file cannot be opened or has no usable format.
    explicit WavReader(const std::filesystem::path& path);
    ~WavReader();
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    std::uint32_t rate() const;      // Hz, as the file has it
    std::uint32_t channels() const;  // samples per frame

    // Reads up to max_frames frames into out as interleaved f32, and returns the number of frames
    // read. Returns 0 at the end of the file. Throws std::runtime_error on a read failure.
    std::size_t read(std::vector<float>& out, std::size_t max_frames);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes interleaved f32 as 16-bit PCM WAV (clamped, see f32_to_s16). Throws on failure.
void write_wav(const std::filesystem::path& path, std::span<const float> interleaved,
               std::uint32_t rate, std::uint32_t channels);

// The core's native file shape: 16-bit mono, 16 kHz.
void write_wav16k_mono(const std::filesystem::path& path, std::span<const float> mono);

}  // namespace echoir::audio
