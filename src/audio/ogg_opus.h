// A minimal Ogg Opus container (RFC 3533 pages, RFC 7845 headers) for one mono Opus stream: a
// writer that lays packets out in pages, and a reader that takes the packets of a page back. Only
// what the session tape needs: one logical stream per file, no chained streams, no seeking table.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace echoir::audio {

// The CRC-32 of an Ogg page (polynomial 0x04c11db7, no reflection, initial value and final XOR 0).
std::uint32_t ogg_crc(std::span<const std::uint8_t> bytes);

// Where an Ogg page of audio packets starts in its file, and which packets it holds. Packets are
// numbered from the start of the stream they belong to.
struct OggPageRef {
    std::int64_t offset = 0;
    std::int64_t first_packet = 0;
    std::int32_t packets = 0;
};

// Writes an Ogg Opus file: the OpusHead and OpusTags pages, then the audio packets, at most
// kMaxPagePackets to a page. Every packet holds kFrameSamples48k of audio at 48 kHz.
class OggOpusWriter {
public:
    // A page holds at most this many packets (one second of 20 ms packets).
    static constexpr int kMaxPagePackets = 50;

    // Creates or replaces path and writes the two header pages. pre_skip is in 48 kHz samples,
    // input_rate is the original sample rate for the header, and first_packet is the number of the
    // first packet added, so the granule positions continue a longer stream. Throws
    // std::runtime_error when the file cannot be written.
    OggOpusWriter(const std::filesystem::path& path, std::uint32_t serial, std::uint16_t pre_skip,
                  std::uint32_t input_rate, std::int64_t frame_samples_48k,
                  std::int64_t first_packet, const std::string& vendor);

    // Adds one packet. Throws std::runtime_error when a page cannot be written.
    void add_packet(std::span<const std::uint8_t> packet);

    // Writes the packets still buffered as the last page, marked end of stream. With end_granule
    // at 0 or more, that page's granule position is end_granule, which trims the end of the
    // stream; otherwise it is the end of its last packet. Closes the file. Throws
    // std::runtime_error when the file cannot be written.
    void finish(std::int64_t end_granule = -1);

    // The audio pages written so far.
    const std::vector<OggPageRef>& pages() const noexcept { return pages_; }

private:
    void write_page(std::span<const std::uint8_t> body, std::span<const std::uint8_t> lacing,
                    std::uint8_t flags, std::int64_t granule);
    void flush_audio(bool last, std::int64_t end_granule);

    std::filesystem::path path_;
    std::ofstream out_;
    std::uint32_t serial_;
    std::uint32_t sequence_ = 0;
    std::int64_t frame_samples_48k_;
    std::int64_t next_packet_;  // number of the next packet added
    std::vector<std::uint8_t> body_;
    std::vector<std::uint8_t> lacing_;
    std::int32_t buffered_ = 0;  // packets in body_
    std::vector<OggPageRef> pages_;
    bool finished_ = false;
};

// Reads the Ogg page at offset of an open file and appends its packets to out. Checks the capture
// pattern and the CRC, and refuses a packet that continues on the next page, which the writer
// never makes. Throws std::runtime_error for a page that cannot be read or does not check out.
void read_ogg_packets(std::ifstream& in, std::int64_t offset,
                      std::vector<std::vector<std::uint8_t>>& out);

}  // namespace echoir::audio
