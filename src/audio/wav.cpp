#include "audio/wav.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/miniaudio_config.h"
#include "audio/pcm.h"

namespace echoir::audio {
namespace {

// RAII over ma_decoder so every early throw still releases the file handle or the buffer view.
class Decoder {
public:
    // Rate 0 and channels 0 keep the file's own values. WAV only, so no other format is picked
    // up by sniffing.
    static ma_decoder_config config() {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        config.encodingFormat = ma_encoding_format_wav;
        return config;
    }

    explicit Decoder(const std::filesystem::path& path, const std::string& what) {
        const ma_decoder_config cfg = config();
#ifdef _WIN32
        ma_result result = ma_decoder_init_file_w(path.c_str(), &cfg, &decoder_);
#else
        ma_result result = ma_decoder_init_file(path.c_str(), &cfg, &decoder_);
#endif
        if (result != MA_SUCCESS) {
            throw std::runtime_error("cannot open WAV " + what + ": " +
                                     ma_result_description(result));
        }
        ready_ = true;
    }

    // The bytes are read in place, so they must outlive the decoder.
    Decoder(std::span<const std::uint8_t> bytes, const std::string& what) {
        const ma_decoder_config cfg = config();
        ma_result result = ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &decoder_);
        if (result != MA_SUCCESS) {
            throw std::runtime_error("cannot decode WAV " + what + ": " +
                                     ma_result_description(result));
        }
        ready_ = true;
    }

    ~Decoder() {
        if (ready_) ma_decoder_uninit(&decoder_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    ma_decoder& get() { return decoder_; }

private:
    ma_decoder decoder_{};
    bool ready_ = false;
};

void put_u16(std::array<std::uint8_t, 44>& h, std::size_t at, std::uint32_t v) {
    h[at] = static_cast<std::uint8_t>(v & 0xff);
    h[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

void put_u32(std::array<std::uint8_t, 44>& h, std::size_t at, std::uint32_t v) {
    put_u16(h, at, v & 0xffff);
    put_u16(h, at + 2, v >> 16);
}

void put_tag(std::array<std::uint8_t, 44>& h, std::size_t at, const char (&tag)[5]) {
    for (std::size_t i = 0; i < 4; ++i) h[at + i] = static_cast<std::uint8_t>(tag[i]);
}

// Reads every frame the decoder has, as f32 at the decoder's own rate and channel count.
PcmBuffer read_all(Decoder& decoder, const std::string& what) {
    ma_decoder& d = decoder.get();

    PcmBuffer out;
    out.rate = d.outputSampleRate;
    out.channels = d.outputChannels;
    if (out.rate == 0 || out.channels == 0) {
        throw std::runtime_error("WAV has no usable format: " + what);
    }

    // Read in fixed chunks so the vector grows once per chunk, not once per frame.
    constexpr ma_uint64 kChunkFrames = 4096;
    std::vector<float> chunk(kChunkFrames * out.channels);
    for (;;) {
        ma_uint64 frames_read = 0;
        ma_result result = ma_decoder_read_pcm_frames(&d, chunk.data(), kChunkFrames, &frames_read);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            throw std::runtime_error("WAV read failed for " + what + ": " +
                                     ma_result_description(result));
        }
        out.samples.insert(out.samples.end(), chunk.begin(),
                           chunk.begin() + static_cast<std::ptrdiff_t>(frames_read * out.channels));
        if (result == MA_AT_END || frames_read == 0) break;
    }
    return out;
}

}  // namespace

PcmBuffer read_wav(const std::filesystem::path& path) {
    Decoder decoder(path, path.string());
    return read_all(decoder, path.string());
}

PcmBuffer read_wav(std::span<const std::uint8_t> bytes) {
    Decoder decoder(bytes, "from memory");
    return read_all(decoder, "from memory");
}

// The decoder stays open for the reader's life, so each read continues where the last one ended.
class WavReader::Impl {
public:
    explicit Impl(const std::filesystem::path& path)
        : decoder(path, path.string()), what(path.string()) {
        const ma_decoder& d = decoder.get();
        if (d.outputSampleRate == 0 || d.outputChannels == 0) {
            throw std::runtime_error("WAV has no usable format: " + what);
        }
    }

    Decoder decoder;
    std::string what;
};

WavReader::WavReader(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}

WavReader::~WavReader() = default;

std::uint32_t WavReader::rate() const { return impl_->decoder.get().outputSampleRate; }

std::uint32_t WavReader::channels() const { return impl_->decoder.get().outputChannels; }

std::size_t WavReader::read(std::vector<float>& out, std::size_t max_frames) {
    const std::size_t width = channels();
    out.resize(max_frames * width);
    ma_uint64 frames_read = 0;
    const ma_result result = ma_decoder_read_pcm_frames(&impl_->decoder.get(), out.data(),
                                                        max_frames, &frames_read);
    if (result != MA_SUCCESS && result != MA_AT_END) {
        throw std::runtime_error("WAV read failed for " + impl_->what + ": " +
                                 ma_result_description(result));
    }
    out.resize(static_cast<std::size_t>(frames_read) * width);
    return static_cast<std::size_t>(frames_read);
}

void write_wav(const std::filesystem::path& path, std::span<const float> interleaved,
               std::uint32_t rate, std::uint32_t channels) {
    if (rate == 0 || channels == 0 || interleaved.size() % channels != 0) {
        throw std::invalid_argument("write_wav: bad rate or channel layout");
    }
    const std::vector<std::int16_t> pcm = f32_to_s16(interleaved);

    // RIFF size fields are 32-bit, so the data must fit in one file.
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(pcm.size()) * 2;
    if (data_bytes > std::numeric_limits<std::uint32_t>::max() - 36) {
        throw std::invalid_argument("write_wav: data too large for a RIFF file");
    }
    const auto data_size = static_cast<std::uint32_t>(data_bytes);
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);

    std::array<std::uint8_t, 44> header{};
    put_tag(header, 0, "RIFF");
    put_u32(header, 4, 36 + data_size);
    put_tag(header, 8, "WAVE");
    put_tag(header, 12, "fmt ");
    put_u32(header, 16, 16);  // fmt chunk size for PCM
    put_u16(header, 20, 1);   // format tag: PCM
    put_u16(header, 22, static_cast<std::uint16_t>(channels));
    put_u32(header, 24, rate);
    put_u32(header, 28, rate * block_align);
    put_u16(header, 32, block_align);
    put_u16(header, 34, 16);  // bits per sample
    put_tag(header, 36, "data");
    put_u32(header, 40, data_size);

    // Samples are little-endian on disk, whatever the host byte order.
    std::vector<std::uint8_t> body(data_size);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const auto v = static_cast<std::uint16_t>(pcm[i]);
        body[2 * i] = static_cast<std::uint8_t>(v & 0xff);
        body[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot create WAV " + path.string());
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    file.write(reinterpret_cast<const char*>(body.data()),
               static_cast<std::streamsize>(body.size()));
    if (!file) throw std::runtime_error("cannot write WAV " + path.string());
}

void write_wav16k_mono(const std::filesystem::path& path, std::span<const float> mono) {
    write_wav(path, mono, 16000, 1);
}

}  // namespace echoir::audio
