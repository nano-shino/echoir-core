// Reads the audio of a transcription run: a WAV file, a WAV on standard input, or raw samples on
// standard input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "audio/wav.h"

namespace echoir::cli {

// Where the audio of a run comes from. parse_input is the one place that reads the input
// argument, so a new kind only adds a case there.
struct InputSpec {
    enum class Kind { file, standard_input, microphone, loopback };
    Kind kind = Kind::file;
    std::string path;  // file only
    // microphone and loopback: a device index from `echoir devices`, or part of a device name.
    // Empty takes the default device.
    std::string device;
};

// "-" is standard input. "mic" or "mic:<device>" is a capture device, and "loopback" or
// "loopback:<device>" is system audio (Windows only). Any other value is a WAV file path, so a
// file literally named mic is given as ./mic. Throws UsageError for a colon with no device after
// it.
InputSpec parse_input(const std::string& value);

// True for the live kinds, which come from a capture device rather than a file or standard input.
bool is_capture(const InputSpec& input);

// Decodes little-endian 16-bit signed samples to f32 in [-1, 1). The byte count must be even.
std::vector<float> decode_s16le(std::span<const std::uint8_t> bytes);

// Raw samples with no header, read from standard input. sample_format is "s16le" (16-bit signed)
// or "f32le" (32-bit float), both little-endian. rate is in Hz, and channels is samples per frame.
struct RawPcmFormat {
    std::string sample_format;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
};

// The audio of a run, a chunk at a time, as interleaved f32 at the input's own rate and channel
// count. The session resamples it.
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual std::uint32_t rate() const = 0;      // Hz
    virtual std::uint32_t channels() const = 0;  // samples per frame

    // Returns up to max_frames frames. The span is valid until the next call. An empty span means
    // the end of the input.
    virtual std::span<const float> next(std::size_t max_frames) = 0;
};

// Opens the input of a run. A WAV file is read as it is taken. Raw samples are read from standard
// input as they are taken, so a live source is decoded while it is still running. A WAV on standard
// input is read whole, because standard input cannot be read twice. raw is used only for standard
// input. Throws std::runtime_error when the input cannot be read, and UsageError when standard
// input is neither a WAV nor raw samples. A capture device is not a chunk source: it runs through
// its own path (see stream.cpp), so passing one throws std::logic_error.
std::unique_ptr<ChunkSource> open_chunk_source(const InputSpec& input,
                                               const std::optional<RawPcmFormat>& raw);

// Reads the whole input at once, with the same rules as open_chunk_source. A trailing partial frame
// of raw samples is dropped with a warning on standard error.
audio::PcmBuffer read_input(const InputSpec& input, const std::optional<RawPcmFormat>& raw);

// The user context of a run: the text of --context, or the text of --context-file when a file is
// given. Line breaks become spaces, the ends are trimmed, and a byte order mark is dropped. Throws
// std::runtime_error when the file cannot be read or the text is not UTF-8.
std::string read_context(const std::string& text, const std::string& file);

}  // namespace echoir::cli
