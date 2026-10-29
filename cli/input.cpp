#include "input.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "audio/pcm.h"
#include "options.h"
#include "text/utf.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// Standard input is binary on Windows, so no byte is translated.
void set_stdin_binary() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

// Reads all of standard input as bytes.
std::vector<std::uint8_t> read_stdin() {
    set_stdin_binary();
    std::vector<std::uint8_t> bytes;
    std::uint8_t chunk[1 << 16];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    if (std::ferror(stdin)) {
        throw std::runtime_error("cannot read standard input");
    }
    return bytes;
}

// A WAV starts with RIFF. RF64, the form for files over 4 GB, is read by miniaudio too.
bool has_wav_header(std::span<const std::uint8_t> bytes) {
    const auto starts_with = [&](const char* tag) {
        return bytes.size() >= 4 && std::memcmp(bytes.data(), tag, 4) == 0;
    };
    return starts_with("RIFF") || starts_with("RF64");
}

// The WAV on standard input. Other bytes are a usage error: they are either not audio, or raw
// samples that were not given a format.
audio::PcmBuffer decode_stdin_wav(const std::vector<std::uint8_t>& bytes) {
    if (!has_wav_header(bytes)) {
        throw UsageError(
            "standard input is not a WAV file; for raw samples pass --pcm s16le|f32le with --rate "
            "and --channels");
    }
    return audio::read_wav(bytes);
}

// Bytes per sample of a raw format.
std::size_t sample_size(const RawPcmFormat& format) {
    return format.sample_format == "s16le" ? 2 : 4;
}

// Bytes per frame of a raw format.
std::size_t frame_size(const RawPcmFormat& format) { return sample_size(format) * format.channels; }

// A trailing partial frame cannot be decoded, so it is dropped with a warning.
void warn_partial_frame(std::size_t bytes) {
    if (bytes > 0) {
        std::fprintf(stderr,
                     "warning: dropped %zu bytes of an incomplete frame at the end of standard "
                     "input\n",
                     bytes);
    }
}

// Converts whole frames of little-endian raw samples to f32, and appends them to out. The bytes are
// assembled explicitly, so the result does not depend on the host's byte order.
void append_frames(std::span<const std::uint8_t> bytes, const RawPcmFormat& format,
                   std::vector<float>& out) {
    if (format.sample_format == "s16le") {
        const std::vector<float> converted = decode_s16le(bytes);
        out.insert(out.end(), converted.begin(), converted.end());
        return;
    }
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const auto bits = static_cast<std::uint32_t>(bytes[i]) |
                          static_cast<std::uint32_t>(bytes[i + 1]) << 8 |
                          static_cast<std::uint32_t>(bytes[i + 2]) << 16 |
                          static_cast<std::uint32_t>(bytes[i + 3]) << 24;
        out.push_back(std::bit_cast<float>(bits));
    }
}

// The whole of some raw bytes, as f32.
audio::PcmBuffer decode_raw(const std::vector<std::uint8_t>& bytes, const RawPcmFormat& format) {
    const std::size_t whole = bytes.size() - bytes.size() % frame_size(format);
    audio::PcmBuffer out;
    out.rate = format.rate;
    out.channels = format.channels;
    append_frames({bytes.data(), whole}, format, out.samples);
    warn_partial_frame(bytes.size() - whole);
    return out;
}

// A WAV file, read a chunk at a time.
class WavFileSource : public ChunkSource {
public:
    explicit WavFileSource(const fs::path& path) : reader_(path) {}

    std::uint32_t rate() const override { return reader_.rate(); }
    std::uint32_t channels() const override { return reader_.channels(); }

    std::span<const float> next(std::size_t max_frames) override {
        const std::size_t frames = reader_.read(buffer_, max_frames);
        return std::span<const float>(buffer_.data(), frames * channels());
    }

private:
    audio::WavReader reader_;
    std::vector<float> buffer_;
};

// A whole input that is already in memory, handed out a chunk at a time.
class WholeInputSource : public ChunkSource {
public:
    explicit WholeInputSource(audio::PcmBuffer buffer)
        : buffer_(std::move(buffer)), total_frames_(buffer_.samples.size() / buffer_.channels) {}

    std::uint32_t rate() const override { return buffer_.rate; }
    std::uint32_t channels() const override { return buffer_.channels; }

    std::span<const float> next(std::size_t max_frames) override {
        const std::size_t frames = std::min(max_frames, total_frames_ - next_frame_);
        const std::span<const float> all(buffer_.samples);
        const std::span<const float> chunk =
            all.subspan(next_frame_ * channels(), frames * channels());
        next_frame_ += frames;
        return chunk;
    }

private:
    audio::PcmBuffer buffer_;
    std::size_t total_frames_;
    std::size_t next_frame_ = 0;
};

// Raw samples from standard input, a chunk at a time. fread waits until a whole chunk has arrived
// or the input ends, so a live source is decoded chunk by chunk while it runs.
class RawStdinSource : public ChunkSource {
public:
    explicit RawStdinSource(RawPcmFormat format) : format_(std::move(format)) {
        set_stdin_binary();
    }

    std::uint32_t rate() const override { return format_.rate; }
    std::uint32_t channels() const override { return format_.channels; }

    std::span<const float> next(std::size_t max_frames) override {
        const std::size_t frame = frame_size(format_);
        const std::size_t want = max_frames * frame;
        for (;;) {
            // Bytes of an incomplete frame from the last read come first.
            bytes_.resize(carried_ + want);
            const std::size_t got = std::fread(bytes_.data() + carried_, 1, want, stdin);
            if (std::ferror(stdin)) {
                throw std::runtime_error("cannot read standard input");
            }
            const std::size_t total = carried_ + got;
            const std::size_t whole = total - total % frame;
            if (whole == 0) {
                if (got == 0) {  // the end of the input
                    warn_partial_frame(total);
                    carried_ = 0;
                    return {};
                }
                carried_ = total;  // not a whole frame yet
                continue;
            }
            samples_.clear();
            append_frames({bytes_.data(), whole}, format_, samples_);
            carried_ = total - whole;
            std::copy(bytes_.begin() + static_cast<std::ptrdiff_t>(whole),
                      bytes_.begin() + static_cast<std::ptrdiff_t>(total), bytes_.begin());
            return samples_;
        }
    }

private:
    RawPcmFormat format_;
    std::vector<std::uint8_t> bytes_;
    std::size_t carried_ = 0;  // bytes at the front of bytes_ that start an incomplete frame
    std::vector<float> samples_;
};

// The device part of "name" or "name:device". Returns false when value is neither form. A colon
// with no device after it is a usage error.
bool split_capture(const std::string& value, std::string_view name, std::string& device) {
    if (value == name) {
        device.clear();
        return true;
    }
    if (value.size() <= name.size() || value.compare(0, name.size(), name) != 0 ||
        value[name.size()] != ':') {
        return false;
    }
    device = value.substr(name.size() + 1);
    if (device.empty()) {
        throw UsageError(std::string(name) + ": needs a device index or part of a name, such as " +
                         std::string(name) + ":1");
    }
    return true;
}

}  // namespace

std::vector<float> decode_s16le(std::span<const std::uint8_t> bytes) {
    std::vector<std::int16_t> samples(bytes.size() / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto bits = static_cast<std::uint16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        samples[i] = static_cast<std::int16_t>(bits);
    }
    return audio::s16_to_f32(samples);
}

InputSpec parse_input(const std::string& value) {
    InputSpec input;
    if (value == "-") {
        input.kind = InputSpec::Kind::standard_input;
    } else if (split_capture(value, "mic", input.device)) {
        input.kind = InputSpec::Kind::microphone;
    } else if (split_capture(value, "loopback", input.device)) {
        input.kind = InputSpec::Kind::loopback;
    } else {
        input.path = value;
    }
    return input;
}

bool is_capture(const InputSpec& input) {
    return input.kind == InputSpec::Kind::microphone || input.kind == InputSpec::Kind::loopback;
}

std::unique_ptr<ChunkSource> open_chunk_source(const InputSpec& input,
                                               const std::optional<RawPcmFormat>& raw) {
    if (input.kind == InputSpec::Kind::file) {
        return std::make_unique<WavFileSource>(text::path_from_utf8(input.path));
    }
    if (is_capture(input)) {
        throw std::logic_error("a capture device is not a chunk source");
    }
    if (raw) {
        return std::make_unique<RawStdinSource>(*raw);
    }
    return std::make_unique<WholeInputSource>(decode_stdin_wav(read_stdin()));
}

audio::PcmBuffer read_input(const InputSpec& input, const std::optional<RawPcmFormat>& raw) {
    if (input.kind == InputSpec::Kind::file) {
        return audio::read_wav(text::path_from_utf8(input.path));
    }
    if (is_capture(input)) {
        throw std::logic_error("a capture device cannot be read whole");
    }
    const std::vector<std::uint8_t> bytes = read_stdin();
    return raw ? decode_raw(bytes, *raw) : decode_stdin_wav(bytes);
}

std::string read_context(const std::string& text, const std::string& file) {
    std::string raw = text;
    if (!file.empty()) {
        // A directory opens as an empty stream, so only a regular file is read.
        const std::filesystem::path path = text::path_from_utf8(file);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            throw std::runtime_error("cannot read context file " + file);
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read context file " + file);
        }
        raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // Windows editors write this byte order mark at the start of a file.
    if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        raw.erase(0, 3);
    }

    // A CRLF pair is one line break. The text is kept on one line, as the decoders take it.
    std::string line;
    line.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            continue;
        }
        line += (c == '\n' || c == '\r') ? ' ' : c;
    }
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    line = line.substr(first, line.find_last_not_of(" \t") - first + 1);

    // Decoding replaces each invalid byte with U+FFFD, so the text survives a round trip only when
    // it was valid UTF-8 to begin with.
    if (text::utf32_to_utf8(text::utf8_to_utf32(line)) != line) {
        throw std::runtime_error("the context is not UTF-8");
    }
    return line;
}

}  // namespace echoir::cli
