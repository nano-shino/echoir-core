#include "audio/ogg_opus.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace echoir::audio {
namespace {

constexpr std::size_t kHeaderBytes = 27;
constexpr std::uint8_t kFlagBos = 0x02;
constexpr std::uint8_t kFlagEos = 0x04;
// A page has at most 255 lacing values.
constexpr std::size_t kMaxLacing = 255;

std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t r = i << 24;
        for (int bit = 0; bit < 8; ++bit) {
            r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04c11db7u : r << 1;
        }
        table[i] = r;
    }
    return table;
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// The lacing values of one packet: 255 for each whole 255 bytes, then the remainder, which is 0
// when the size is a multiple of 255.
void lace(std::vector<std::uint8_t>& lacing, std::size_t size) {
    while (size >= 255) {
        lacing.push_back(255);
        size -= 255;
    }
    lacing.push_back(static_cast<std::uint8_t>(size));
}

std::size_t lacing_count(std::size_t size) { return size / 255 + 1; }

}  // namespace

std::uint32_t ogg_crc(std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = make_crc_table();
    std::uint32_t crc = 0;
    for (const std::uint8_t b : bytes) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xffu) ^ b];
    }
    return crc;
}

OggOpusWriter::OggOpusWriter(const std::filesystem::path& path, std::uint32_t serial,
                             std::uint16_t pre_skip, std::uint32_t input_rate,
                             std::int64_t frame_samples_48k, std::int64_t first_packet,
                             const std::string& vendor)
    : path_(path),
      out_(path, std::ios::binary | std::ios::trunc),
      serial_(serial),
      frame_samples_48k_(frame_samples_48k),
      first_packet_(first_packet),
      next_packet_(first_packet) {
    if (!out_) {
        throw std::runtime_error("cannot create " + path.filename().string());
    }
    // OpusHead: version 1, one channel, the pre-skip, the input rate, no gain, mapping family 0.
    std::vector<std::uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1};
    put_u16(head, pre_skip);
    put_u32(head, input_rate);
    put_u16(head, 0);
    head.push_back(0);
    std::vector<std::uint8_t> lacing;
    lace(lacing, head.size());
    write_page(head, lacing, kFlagBos, 0);

    // OpusTags: the vendor string and no comments.
    std::vector<std::uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    put_u32(tags, static_cast<std::uint32_t>(vendor.size()));
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    put_u32(tags, 0);
    lacing.clear();
    lace(lacing, tags.size());
    write_page(tags, lacing, 0, 0);
}

void OggOpusWriter::add_packet(std::span<const std::uint8_t> packet) {
    if (finished_) {
        throw std::logic_error("the Ogg stream is finished");
    }
    if (buffered_ > 0 && (buffered_ == kMaxPagePackets ||
                          lacing_.size() + lacing_count(packet.size()) > kMaxLacing)) {
        flush_audio(false, -1);
    }
    if (lacing_count(packet.size()) > kMaxLacing) {
        throw std::invalid_argument("an Opus packet is too large for one Ogg page");
    }
    body_.insert(body_.end(), packet.begin(), packet.end());
    lace(lacing_, packet.size());
    ++buffered_;
}

void OggOpusWriter::finish(std::int64_t end_granule) {
    if (finished_) {
        return;
    }
    flush_audio(true, end_granule);
    finished_ = true;
    out_.close();
    if (out_.fail()) {
        throw std::runtime_error("cannot write " + path_.filename().string());
    }
}

void OggOpusWriter::flush_audio(bool last, std::int64_t end_granule) {
    const std::int64_t first = next_packet_;
    next_packet_ += buffered_;
    std::int64_t granule = (next_packet_ - first_packet_) * frame_samples_48k_;
    if (last && end_granule >= 0) {
        granule = end_granule;
    }
    const auto offset = static_cast<std::int64_t>(out_.tellp());
    write_page(body_, lacing_, last ? kFlagEos : 0, granule);
    if (buffered_ > 0) {
        pages_.push_back(OggPageRef{offset, first, buffered_});
    }
    body_.clear();
    lacing_.clear();
    buffered_ = 0;
}

void OggOpusWriter::write_page(std::span<const std::uint8_t> body,
                               std::span<const std::uint8_t> lacing, std::uint8_t flags,
                               std::int64_t granule) {
    std::vector<std::uint8_t> page = {'O', 'g', 'g', 'S', 0, flags};
    put_u64(page, static_cast<std::uint64_t>(granule));
    put_u32(page, serial_);
    put_u32(page, sequence_++);
    put_u32(page, 0);  // CRC, filled in below
    page.push_back(static_cast<std::uint8_t>(lacing.size()));
    page.insert(page.end(), lacing.begin(), lacing.end());
    page.insert(page.end(), body.begin(), body.end());
    const std::uint32_t crc = ogg_crc(page);
    for (int i = 0; i < 4; ++i) {
        page[22 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
    out_.write(reinterpret_cast<const char*>(page.data()),
               static_cast<std::streamsize>(page.size()));
    if (!out_) {
        throw std::runtime_error("cannot write " + path_.filename().string());
    }
}

void read_ogg_packets(std::ifstream& in, std::int64_t offset,
                      std::vector<std::vector<std::uint8_t>>& out) {
    in.clear();
    in.seekg(offset);
    std::vector<std::uint8_t> page(kHeaderBytes);
    in.read(reinterpret_cast<char*>(page.data()), kHeaderBytes);
    if (!in || std::memcmp(page.data(), "OggS", 4) != 0) {
        throw std::runtime_error("an Ogg page is missing or damaged");
    }
    const std::size_t segments = page[26];
    page.resize(kHeaderBytes + segments);
    in.read(reinterpret_cast<char*>(page.data() + kHeaderBytes),
            static_cast<std::streamsize>(segments));
    std::size_t body_size = 0;
    for (std::size_t i = 0; i < segments; ++i) {
        body_size += page[kHeaderBytes + i];
    }
    const std::size_t body_at = page.size();
    page.resize(body_at + body_size);
    in.read(reinterpret_cast<char*>(page.data() + body_at),
            static_cast<std::streamsize>(body_size));
    if (!in) {
        throw std::runtime_error("an Ogg page is cut short");
    }
    const std::uint32_t stored = get_u32(page.data() + 22);
    std::memset(page.data() + 22, 0, 4);
    if (ogg_crc(page) != stored) {
        throw std::runtime_error("an Ogg page fails its CRC check");
    }
    std::size_t at = body_at;
    std::size_t size = 0;
    for (std::size_t i = 0; i < segments; ++i) {
        const std::uint8_t value = page[kHeaderBytes + i];
        size += value;
        if (value < 255) {
            out.emplace_back(page.begin() + static_cast<std::ptrdiff_t>(at),
                             page.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
            size = 0;
        }
    }
    if (size != 0) {
        throw std::runtime_error("an Ogg page ends inside a packet");
    }
}

}  // namespace echoir::audio
