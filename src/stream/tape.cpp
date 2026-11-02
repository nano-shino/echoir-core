#include "stream/tape.h"

#include <opus.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "audio/ogg_opus.h"
#include "errors.h"
#include "json.hpp"
#include "stream/envelope.h"
#include "stream/signal.h"

namespace echoir::stream {
namespace {

constexpr std::int32_t kRate = 16000;
// One Opus frame: 20 ms.
constexpr std::int64_t kFrame = 20 * kSamplesPerMs;
// The same frame at 48 kHz, the unit of Ogg Opus granule positions.
constexpr std::int64_t kFrame48k = kFrame * 3;
constexpr std::int64_t kMemorySamples = Tape::kMemoryMs * kSamplesPerMs;
constexpr std::int64_t kPagePackets = Tape::kPageMs / 20;
// Packets decoded and dropped before a read, so the decoder settles first (RFC 7845 asks for
// 80 ms of pre-roll when decoding starts inside a stream).
constexpr std::int64_t kPrerollPackets = 4;
// A run starts on a whole envelope cell, so a session's cells line up with the tape's.
constexpr std::int64_t kRunAlign = Envelope::kCellMs * kSamplesPerMs;
// Large enough for any Opus packet.
constexpr int kMaxPacketBytes = 1500;

struct EncoderDeleter {
    void operator()(OpusEncoder* e) const noexcept { opus_encoder_destroy(e); }
};
struct DecoderDeleter {
    void operator()(OpusDecoder* d) const noexcept { opus_decoder_destroy(d); }
};
using EncoderPtr = std::unique_ptr<OpusEncoder, EncoderDeleter>;
using DecoderPtr = std::unique_ptr<OpusDecoder, DecoderDeleter>;

EncoderPtr make_encoder() {
    int error = OPUS_OK;
    EncoderPtr enc(opus_encoder_create(kRate, 1, OPUS_APPLICATION_AUDIO, &error));
    if (error != OPUS_OK || !enc) {
        throw std::runtime_error(std::string("cannot create the Opus encoder: ") +
                                 opus_strerror(error));
    }
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(Tape::kBitrate));
    return enc;
}

// A copy of an encoder's whole state, so it can go on from where the original is without
// changing it. The state is one position-independent block (see opus.h), and
// opus_encoder_destroy() frees it with free().
EncoderPtr copy_encoder(const OpusEncoder* enc) {
    const auto size = static_cast<std::size_t>(opus_encoder_get_size(1));
    void* copy = std::malloc(size);
    if (copy == nullptr) {
        throw std::bad_alloc();
    }
    std::memcpy(copy, enc, size);
    return EncoderPtr(static_cast<OpusEncoder*>(copy));
}

std::vector<std::uint8_t> encode(OpusEncoder* enc, const std::int16_t* frame) {
    std::vector<std::uint8_t> packet(kMaxPacketBytes);
    const opus_int32 n =
        opus_encode(enc, frame, static_cast<int>(kFrame), packet.data(), kMaxPacketBytes);
    if (n < 0) {
        throw std::runtime_error(std::string("Opus encoding failed: ") + opus_strerror(n));
    }
    packet.resize(static_cast<std::size_t>(n));
    return packet;
}

// The packet of a frame of silence from an encoder that has heard only silence. Padding is
// written as this packet, so a long gap costs no encoding.
const std::vector<std::uint8_t>& silence_packet() {
    static const std::vector<std::uint8_t> packet = [] {
        EncoderPtr enc = make_encoder();
        const std::vector<std::int16_t> zeros(static_cast<std::size_t>(kFrame), 0);
        std::vector<std::uint8_t> last;
        for (int i = 0; i < 25; ++i) {
            last = encode(enc.get(), zeros.data());
        }
        return last;
    }();
    return packet;
}

// A track name as a file name: ASCII letters, digits, '-' and '_' stay, every other byte becomes
// %XX. The result never holds a '.', so it cannot collide with the page files of another track.
std::string file_stem(const std::string& name) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool same_ignoring_ascii_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

std::uint32_t fnv1a(const std::string& text) {
    std::uint32_t h = 2166136261u;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    return h;
}

std::string page_name(const std::string& stem, int index) {
    char number[16];
    std::snprintf(number, sizeof number, "%05d", index);
    return stem + "." + number + ".opus";
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

struct Tape::Track {
    // A file of Ogg Opus pages: a page file, or after close() the joined file.
    struct File {
        std::filesystem::path path;
        std::int64_t first_packet = 0;
        std::int64_t packets = 0;
        std::vector<audio::OggPageRef> pages;
    };

    std::string name;
    std::string label;
    std::string stem;
    std::uint32_t serial = 0;

    mutable std::mutex mu;  // everything below
    bool writing = false;
    bool sealed = false;                                      // closed: no more writes
    std::vector<std::pair<std::int64_t, std::int64_t>> runs;  // in samples
    std::int64_t total = 0;                                   // samples on the track
    Envelope envelope;
    std::vector<std::int16_t> ring;  // sample n at n % ring.size(), allocated on the first write
    std::int64_t ring_size = 0;
    std::vector<std::int16_t> scratch;

    // Pages, with a folder only.
    std::filesystem::path folder;
    EncoderPtr enc;
    std::int64_t lookahead =
        0;                 // encoder delay in samples: input n decodes at output n + lookahead
    std::int64_t fed = 0;  // samples given to the encoder; packet p covers output [p, p+1) frames
    std::deque<std::pair<std::int64_t, std::int64_t>> holes;  // padding, as sample ranges
    std::vector<std::vector<std::uint8_t>> open;              // packets not in a file yet
    std::vector<File> files;
    int pages_written = 0;
    bool joined = false;  // close() joined every packet into one file
    std::string error;

    bool paged() const { return enc != nullptr || joined; }

    // The first sample still in memory.
    std::int64_t memory_from() const {
        if (joined || (sealed && !paged())) return total;
        if (paged()) return std::max<std::int64_t>(0, fed - lookahead);
        return std::max<std::int64_t>(0, total - ring_size);
    }

    void ensure_ring() {
        if (!ring.empty()) return;
        ring_size = enc ? kMemorySamples + 2 * kFrame + lookahead : kMemorySamples;
        ring.assign(static_cast<std::size_t>(ring_size), 0);
    }

    std::int16_t sample_at(std::int64_t n) const {
        return n < total ? ring[static_cast<std::size_t>(n % ring_size)] : std::int16_t{0};
    }

    // Appends at most kFrame samples, then encodes what is older than the memory keeps.
    void append_block(std::span<const std::int16_t> block) {
        ensure_ring();
        for (const std::int16_t s : block) {
            ring[static_cast<std::size_t>(total % ring_size)] = s;
            ++total;
        }
        envelope.append(block);
        if (enc) {
            while (total - fed >= kMemorySamples + kFrame) {
                encode_frame(true);
            }
        }
    }

    void append(std::span<const std::int16_t> samples) {
        for (std::size_t at = 0; at < samples.size(); at += static_cast<std::size_t>(kFrame)) {
            const std::size_t n =
                std::min<std::size_t>(static_cast<std::size_t>(kFrame), samples.size() - at);
            append_block(samples.subspan(at, n));
        }
        // Padding is not part of a run, so only a writer's audio moves the end of its run.
        if (!runs.empty()) runs.back().second = total;
    }

    // Pads the track with n samples of silence. With pages, the padding is a hole: frames inside
    // it are written as the silence packet without running the encoder.
    void pad(std::int64_t n) {
        if (n <= 0) return;
        if (enc) {
            // A hole at the very start also covers the encoder's delay before sample 0.
            const std::int64_t from =
                total == 0 ? std::numeric_limits<std::int64_t>::min() / 4 : total;
            holes.emplace_back(from, total + n);
        }
        static const std::vector<std::int16_t> zeros(static_cast<std::size_t>(kFrame), 0);
        while (n > 0) {
            const std::int64_t step = std::min(n, kFrame);
            append_block(
                std::span<const std::int16_t>(zeros.data(), static_cast<std::size_t>(step)));
            n -= step;
        }
    }

    // True when the frame of input [at, at + kFrame) is padding only. Its packet decodes to output
    // [at, at + kFrame), which is input [at - lookahead, at + kFrame - lookahead), so a hole must
    // cover both ranges.
    static bool in_hole(const std::deque<std::pair<std::int64_t, std::int64_t>>& holes,
                        std::int64_t at, std::int64_t lookahead) {
        for (const auto& [from, to] : holes) {
            if (from <= at - lookahead && at + kFrame <= to) return true;
            if (from > at - lookahead) break;
        }
        return false;
    }

    // Encodes the frame of input [fed, fed + kFrame). Samples past the end are silence (close()).
    void encode_frame(bool write_pages) {
        while (!holes.empty() && holes.front().second <= fed - lookahead) {
            holes.pop_front();
        }
        if (in_hole(holes, fed, lookahead)) {
            open.push_back(silence_packet());
        } else {
            std::int16_t frame[kFrame];
            for (std::int64_t i = 0; i < kFrame; ++i) {
                frame[i] = sample_at(fed + i);
            }
            open.push_back(encode(enc.get(), frame));
        }
        fed += kFrame;
        // A failed write keeps the packets; the next try is when another page has filled.
        if (write_pages && static_cast<std::int64_t>(open.size()) % kPagePackets == 0) {
            write_open_packets();
        }
    }

    std::string vendor() const { return opus_get_version_string(); }

    std::uint16_t pre_skip() const { return static_cast<std::uint16_t>(lookahead * 3); }

    // Writes the packets not in a file yet as the next page file.
    void write_open_packets() {
        const std::filesystem::path path = folder / page_name(stem, pages_written + 1);
        const std::int64_t first = fed / kFrame - static_cast<std::int64_t>(open.size());
        try {
            audio::OggOpusWriter w(path, serial, pre_skip(), kRate, kFrame48k, first, vendor());
            for (const auto& packet : open) {
                w.add_packet(packet);
            }
            w.finish();
            files.push_back(File{path, first, static_cast<std::int64_t>(open.size()), w.pages()});
            open.clear();
            ++pages_written;
            error.clear();
        } catch (const std::exception& e) {
            error = e.what();
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // Writes path as one Ogg Opus file of the whole track: the packets of files, then those of
    // open and tail, with the last page trimmed to end_granule. Returns its audio pages. Runs
    // without the track's lock; the caller keeps the files from being replaced.
    std::vector<audio::OggPageRef> write_joined(
        const std::filesystem::path& path, const std::vector<File>& from,
        const std::vector<std::vector<std::uint8_t>>& open_packets,
        const std::vector<std::vector<std::uint8_t>>& tail, std::int64_t end_granule) const {
        audio::OggOpusWriter w(path, serial, pre_skip(), kRate, kFrame48k, 0, vendor());
        std::vector<std::vector<std::uint8_t>> packets;
        for (const auto& f : from) {
            std::ifstream in(f.path, std::ios::binary);
            if (!in) {
                throw std::runtime_error("cannot open " + f.path.filename().string());
            }
            for (const auto& page : f.pages) {
                packets.clear();
                audio::read_ogg_packets(in, page.offset, packets);
                for (const auto& packet : packets) {
                    w.add_packet(packet);
                }
            }
        }
        for (const auto& packet : open_packets) {
            w.add_packet(packet);
        }
        for (const auto& packet : tail) {
            w.add_packet(packet);
        }
        w.finish(end_granule);
        return w.pages();
    }

    // The packets a read needs, taken under the lock and fetched without it.
    struct Plan {
        std::vector<std::pair<std::filesystem::path, std::vector<audio::OggPageRef>>> files;
        std::int64_t open_first = 0;
        std::vector<std::vector<std::uint8_t>> open;
    };

    Plan plan(std::int64_t q0, std::int64_t q1) const {
        Plan p;
        for (const File& f : files) {
            if (f.first_packet + f.packets <= q0 || f.first_packet >= q1) continue;
            std::vector<audio::OggPageRef> pages;
            for (const auto& page : f.pages) {
                if (page.first_packet + page.packets <= q0 || page.first_packet >= q1) continue;
                pages.push_back(page);
            }
            p.files.emplace_back(f.path, std::move(pages));
        }
        const std::int64_t open_first = fed / kFrame - static_cast<std::int64_t>(open.size());
        p.open_first = std::max(q0, open_first);
        for (std::int64_t q = p.open_first; q < q1; ++q) {
            p.open.push_back(open[static_cast<std::size_t>(q - open_first)]);
        }
        return p;
    }

    // Fetches packets [q0, q1) as planned and decodes them into out, which then holds the
    // decoder's output from packet q0 on. Runs without the track's lock.
    static void decode_plan(const Plan& plan, std::int64_t q0, std::int64_t q1,
                            std::vector<std::int16_t>& out);
};

std::shared_ptr<Tape> Tape::create(const std::filesystem::path& folder) {
    if (!folder.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec || !std::filesystem::is_directory(folder)) {
            throw std::runtime_error("cannot create the tape folder: " + ec.message());
        }
    }
    return std::shared_ptr<Tape>(new Tape(folder));
}

Tape::Tape(std::filesystem::path folder) : folder_(std::move(folder)) {}

Tape::~Tape() = default;

Tape::Track* Tape::find_locked(const std::string& name) const {
    for (const auto& t : tracks_) {
        if (t->name == name) return t.get();
    }
    return nullptr;
}

Tape::Track& Tape::add_locked(const std::string& name, const std::string& label) {
    if (name.empty() || name.size() > kMaxNameBytes) {
        throw std::invalid_argument("a track name must be 1 to 64 bytes");
    }
    for (const auto& t : tracks_) {
        if (same_ignoring_ascii_case(t->name, name)) {
            throw std::invalid_argument("the tape already has a track named " + t->name);
        }
    }
    auto t = std::make_unique<Track>();
    t->name = name;
    t->label = label.empty() ? name : label;
    t->stem = file_stem(name);
    t->serial = fnv1a(t->stem);
    if (!folder_.empty()) {
        t->folder = folder_;
        t->enc = make_encoder();
        opus_int32 lookahead = 0;
        opus_encoder_ctl(t->enc.get(), OPUS_GET_LOOKAHEAD(&lookahead));
        t->lookahead = lookahead;
    }
    tracks_.push_back(std::move(t));
    return *tracks_.back();
}

void Tape::add_track(const std::string& name, const std::string& label) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
        throw StateError("the tape is closed");
    }
    add_locked(name, label);
}

std::int64_t Tape::end_samples_locked() const {
    std::int64_t end = 0;
    for (const auto& t : tracks_) {
        std::lock_guard<std::mutex> lock(t->mu);
        end = std::max(end, t->total);
    }
    return end;
}

std::int64_t Tape::duration_ms() const {
    std::lock_guard<std::mutex> lock(mu_);
    return end_samples_locked() / kSamplesPerMs;
}

bool Tape::closed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return closed_;
}

std::int64_t Tape::begin_run(Track& track) {
    const std::int64_t end = end_samples_locked();
    const std::int64_t start = (end + kRunAlign - 1) / kRunAlign * kRunAlign;
    std::lock_guard<std::mutex> lock(track.mu);
    if (!track.sealed) {
        track.pad(start - track.total);
        track.runs.emplace_back(track.total, track.total);
    }
    return std::max(start, track.total);
}

std::unique_ptr<Tape::Writer> Tape::attach(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
        throw StateError("the tape is closed");
    }
    Track* track = find_locked(name);
    if (track == nullptr) {
        track = &add_locked(name, name);
    }
    {
        std::lock_guard<std::mutex> t(track->mu);
        if (track->writing) {
            throw StateError("the track " + name + " already has a writer");
        }
        track->writing = true;
    }
    try {
        const std::int64_t start = begin_run(*track);
        return std::unique_ptr<Writer>(new Writer(shared_from_this(), track, start));
    } catch (...) {
        std::lock_guard<std::mutex> t(track->mu);
        track->writing = false;
        throw;
    }
}

Tape::Writer::Writer(std::shared_ptr<Tape> tape, Track* track, std::int64_t start)
    : tape_(std::move(tape)), track_(track), start_(start) {}

Tape::Writer::~Writer() {
    std::lock_guard<std::mutex> lock(track_->mu);
    track_->writing = false;
}

const std::string& Tape::Writer::track() const noexcept { return track_->name; }

std::int64_t Tape::Writer::start_samples() const noexcept { return start_.load(); }

void Tape::Writer::append(std::span<const float> pcm) {
    std::lock_guard<std::mutex> lock(track_->mu);
    if (track_->sealed) {
        return;
    }
    track_->scratch.resize(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        track_->scratch[i] = to_s16(pcm[i]);
    }
    track_->append(track_->scratch);
}

void Tape::Writer::restart() {
    std::lock_guard<std::mutex> lock(tape_->mu_);
    start_.store(tape_->begin_run(*track_));
}

void Tape::Writer::seam() {
    std::lock_guard<std::mutex> lock(track_->mu);
    if (track_->sealed || track_->runs.empty()) {
        return;
    }
    if (track_->runs.back().second > track_->runs.back().first) {
        track_->runs.emplace_back(track_->total, track_->total);
    }
}

std::size_t Tape::read_peaks(const std::string& name, std::int64_t start_ms, std::int64_t end_ms,
                             std::int64_t bucket_ms, float* out, std::size_t cap,
                             std::int64_t limit_cells) const {
    Track* track = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        track = find_locked(name);
    }
    if (track == nullptr) {
        throw std::invalid_argument("the tape has no track named " + name);
    }
    std::lock_guard<std::mutex> lock(track->mu);
    return track->envelope.read(start_ms, end_ms, bucket_ms, out, cap, limit_cells);
}

void Tape::Track::decode_plan(const Plan& plan, std::int64_t q0, std::int64_t q1,
                              std::vector<std::int16_t>& out) {
    std::vector<std::vector<std::uint8_t>> packets(static_cast<std::size_t>(q1 - q0));
    std::vector<std::vector<std::uint8_t>> page_packets;
    for (const auto& [path, pages] : plan.files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open the tape file " + path.filename().string());
        }
        for (const auto& page : pages) {
            page_packets.clear();
            audio::read_ogg_packets(in, page.offset, page_packets);
            if (static_cast<std::int32_t>(page_packets.size()) != page.packets) {
                throw std::runtime_error("a tape page holds an unexpected number of packets");
            }
            for (std::int32_t i = 0; i < page.packets; ++i) {
                const std::int64_t q = page.first_packet + i;
                if (q >= q0 && q < q1) {
                    packets[static_cast<std::size_t>(q - q0)] =
                        std::move(page_packets[static_cast<std::size_t>(i)]);
                }
            }
        }
    }
    for (std::size_t i = 0; i < plan.open.size(); ++i) {
        packets[static_cast<std::size_t>(plan.open_first - q0) + i] = plan.open[i];
    }

    int error = OPUS_OK;
    DecoderPtr dec(opus_decoder_create(kRate, 1, &error));
    if (error != OPUS_OK || !dec) {
        throw std::runtime_error(std::string("cannot create the Opus decoder: ") +
                                 opus_strerror(error));
    }
    out.assign(static_cast<std::size_t>((q1 - q0) * kFrame), 0);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        if (packet.empty()) {
            throw std::runtime_error("a tape packet is missing");
        }
        const int n = opus_decode(dec.get(), packet.data(), static_cast<opus_int32>(packet.size()),
                                  out.data() + i * static_cast<std::size_t>(kFrame),
                                  static_cast<int>(kFrame), 0);
        if (n != kFrame) {
            throw std::runtime_error(std::string("Opus decoding failed: ") +
                                     (n < 0 ? opus_strerror(n) : "short frame"));
        }
    }
}

std::size_t Tape::read_audio(const std::string& name, std::int64_t start_ms, std::int64_t end_ms,
                             std::int16_t* out, std::size_t cap,
                             std::int64_t end_limit_samples) const {
    check_range(start_ms, end_ms);
    std::shared_lock<std::shared_mutex> files(files_mu_);
    Track* track = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        track = find_locked(name);
    }
    if (track == nullptr) {
        throw std::invalid_argument("the tape has no track named " + name);
    }
    std::unique_lock<std::mutex> lock(track->mu);
    const std::int64_t total = std::min(track->total, end_limit_samples);
    // Clamped before scaling, so a far end such as INT64_MAX does not overflow.
    const std::int64_t newest_ms = total / kSamplesPerMs + 1;
    if (start_ms >= newest_ms) {
        return 0;
    }
    const std::int64_t start = start_ms * kSamplesPerMs;
    const std::int64_t end = std::min(std::min(end_ms, newest_ms) * kSamplesPerMs, total);
    if (end <= start) {
        return 0;
    }
    const std::int64_t memory_from = track->memory_from();
    const std::int64_t kept_from = track->paged() ? 0 : memory_from;
    if (start < kept_from) {
        throw StateError("the audio of that range is no longer kept");
    }
    const auto count = static_cast<std::size_t>(end - start);
    if (out == nullptr) {
        return count;
    }
    if (cap < count) {
        throw std::invalid_argument("the output buffer is too small for the range");
    }
    for (std::int64_t n = std::max(start, memory_from); n < end; ++n) {
        out[n - start] = track->ring[static_cast<std::size_t>(n % track->ring_size)];
    }
    const std::int64_t paged_end = std::min(end, memory_from);
    if (start >= paged_end) {
        return count;
    }
    // Input n is output n + lookahead of the decoder.
    const std::int64_t lookahead = track->lookahead;
    const std::int64_t q0 =
        std::max<std::int64_t>(0, (start + lookahead) / kFrame - kPrerollPackets);
    const std::int64_t q1 = (paged_end + lookahead + kFrame - 1) / kFrame;
    const Track::Plan plan = track->plan(q0, q1);
    lock.unlock();

    std::vector<std::int16_t> decoded;
    Track::decode_plan(plan, q0, q1, decoded);
    for (std::int64_t n = start; n < paged_end; ++n) {
        out[n - start] = decoded[static_cast<std::size_t>(n + lookahead - q0 * kFrame)];
    }
    return count;
}

void Tape::checkpoint() {
    std::lock_guard<std::mutex> one(checkpoint_mu_);
    // Held shared, so close() cannot replace the page files while they are copied.
    std::shared_lock<std::shared_mutex> files(files_mu_);
    std::vector<Track*> tracks;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || folder_.empty()) {
            return;
        }
        for (const auto& t : tracks_) {
            tracks.push_back(t.get());
        }
    }
    std::string first_error;
    for (Track* t : tracks) {
        // What the file needs, taken under the lock: the page files, the packets not in one yet,
        // the encoder as it is, and the samples it has not encoded. The rest runs without the
        // lock, so a writer appending meanwhile waits only for these copies.
        std::vector<Track::File> from;
        std::vector<std::vector<std::uint8_t>> open_packets;
        EncoderPtr enc;
        std::deque<std::pair<std::int64_t, std::int64_t>> holes;
        std::vector<std::int16_t> unencoded;
        std::int64_t fed = 0;
        std::int64_t total = 0;
        {
            std::lock_guard<std::mutex> tl(t->mu);
            if (!t->enc || t->sealed || t->total == 0) {
                continue;
            }
            from = t->files;
            open_packets = t->open;
            enc = copy_encoder(t->enc.get());
            holes = t->holes;
            fed = t->fed;
            total = t->total;
            unencoded.resize(static_cast<std::size_t>(total - fed));
            for (std::int64_t n = fed; n < total; ++n) {
                unencoded[static_cast<std::size_t>(n - fed)] = t->sample_at(n);
            }
        }
        const std::filesystem::path final_path = folder_ / (t->stem + ".opus");
        const std::filesystem::path part_path = folder_ / (t->stem + ".opus.part");
        try {
            // The unencoded samples, then silence until the encoder's delay is out, as close()
            // ends the track, but on the copy of the encoder.
            std::vector<std::vector<std::uint8_t>> tail;
            for (std::int64_t at = fed; at < total + t->lookahead; at += kFrame) {
                if (Track::in_hole(holes, at, t->lookahead)) {
                    tail.push_back(silence_packet());
                    continue;
                }
                std::int16_t frame[kFrame];
                for (std::int64_t i = 0; i < kFrame; ++i) {
                    const std::int64_t n = at + i;
                    frame[i] = n < total ? unencoded[static_cast<std::size_t>(n - fed)] : 0;
                }
                tail.push_back(encode(enc.get(), frame));
            }
            t->write_joined(part_path, from, open_packets, tail, 3 * (t->lookahead + total));
            std::filesystem::rename(part_path, final_path);
        } catch (const std::exception& e) {
            std::error_code ec;
            std::filesystem::remove(part_path, ec);
            if (first_error.empty()) {
                first_error = t->name + ": " + e.what();
            }
        }
    }
    if (!first_error.empty()) {
        throw std::runtime_error("cannot write the checkpoint file of " + first_error);
    }
}

void Tape::close() {
    std::unique_lock<std::shared_mutex> files(files_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
        return;
    }
    closed_ = true;
    std::string first_error;
    for (const auto& t : tracks_) {
        std::lock_guard<std::mutex> tl(t->mu);
        t->sealed = true;
        if (!t->enc) {
            // Memory only: the audio goes, the envelope stays.
            t->ring.clear();
            t->ring.shrink_to_fit();
            continue;
        }
        if (t->total == 0) {
            t->enc.reset();
            continue;
        }
        const std::filesystem::path final_path = folder_ / (t->stem + ".opus");
        const std::filesystem::path part_path = folder_ / (t->stem + ".opus.part");
        try {
            // The rest of the track, then silence until the encoder's delay is out.
            while (t->fed < t->total + t->lookahead) {
                t->encode_frame(false);
            }
            // The last page trims the stream to the track's own length.
            std::vector<audio::OggPageRef> pages =
                t->write_joined(part_path, t->files, t->open, {}, 3 * (t->lookahead + t->total));
            std::filesystem::rename(part_path, final_path);
            for (const auto& f : t->files) {
                std::error_code ec;
                std::filesystem::remove(f.path, ec);
            }
            t->files = {Track::File{final_path, 0, t->fed / kFrame, std::move(pages)}};
            t->open.clear();
            t->open.shrink_to_fit();
            t->joined = true;
            t->enc.reset();
            t->ring.clear();
            t->ring.shrink_to_fit();
        } catch (const std::exception& e) {
            // The page files and the memory stay, so the track can still be read.
            std::error_code ec;
            std::filesystem::remove(part_path, ec);
            t->error = e.what();
            if (first_error.empty()) {
                first_error = t->name + ": " + e.what();
            }
        }
    }
    if (!first_error.empty()) {
        throw std::runtime_error("cannot write the tape file of " + first_error);
    }
}

std::string Tape::tracks_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json tracks = nlohmann::json::array();
    std::int64_t end = 0;
    for (const auto& t : tracks_) {
        std::lock_guard<std::mutex> tl(t->mu);
        end = std::max(end, t->total);
        nlohmann::json runs = nlohmann::json::array();
        for (const auto& [from, to] : t->runs) {
            runs.push_back({{"start_ms", from / kSamplesPerMs}, {"end_ms", to / kSamplesPerMs}});
        }
        nlohmann::json entry = {{"name", t->name},
                                {"label", t->label},
                                {"file", folder_.empty() ? "" : t->stem + ".opus"},
                                {"duration_ms", t->total / kSamplesPerMs},
                                {"writing", t->writing},
                                {"runs", std::move(runs)}};
        if (!t->error.empty()) {
            entry["error"] = t->error;
        }
        tracks.push_back(std::move(entry));
    }
    nlohmann::json root = {
        {"duration_ms", end / kSamplesPerMs}, {"closed", closed_}, {"tracks", std::move(tracks)}};
    return root.dump();
}

}  // namespace echoir::stream
