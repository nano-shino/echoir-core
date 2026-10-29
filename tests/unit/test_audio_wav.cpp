#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/pcm.h"
#include "audio/wav.h"
#include "doctest.h"

namespace ea = echoir::audio;
namespace fs = std::filesystem;

namespace {

fs::path temp_wav(const std::string& tag) {
    return fs::temp_directory_path() / ("echoir_test_" + tag + ".wav");
}

void put_u16(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xff));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xff));
}

void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    put_u16(v, x & 0xffff);
    put_u16(v, x >> 16);
}

void put_tag(std::vector<std::uint8_t>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

// Writes a WAV with any format tag and width, for the layouts write_wav never makes.
void write_raw_wav(const fs::path& path, std::uint32_t format_tag, std::uint32_t bits,
                   std::uint32_t channels, std::uint32_t rate,
                   const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> file;
    put_tag(file, "RIFF");
    put_u32(file, static_cast<std::uint32_t>(4 + 24 + 8 + data.size()));
    put_tag(file, "WAVE");
    put_tag(file, "fmt ");
    put_u32(file, 16);
    put_u16(file, format_tag);
    put_u16(file, channels);
    put_u32(file, rate);
    put_u32(file, rate * channels * bits / 8);
    put_u16(file, channels * bits / 8);
    put_u16(file, bits);
    put_tag(file, "data");
    put_u32(file, static_cast<std::uint32_t>(data.size()));
    file.insert(file.end(), data.begin(), data.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
}

std::vector<std::uint8_t> read_all_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("s16_to_f32 and f32_to_s16 invert each other on every int16 value") {
    std::vector<std::int16_t> all;
    for (int v = std::numeric_limits<std::int16_t>::min();
         v <= std::numeric_limits<std::int16_t>::max(); ++v) {
        all.push_back(static_cast<std::int16_t>(v));
    }
    const std::vector<float> f = ea::s16_to_f32(all);
    REQUIRE(f.size() == all.size());
    CHECK(f.front() == -1.0f);  // -32768 / 32768
    CHECK(f[32768] == 0.0f);

    const std::vector<std::int16_t> back = ea::f32_to_s16(f);
    bool same = back == all;
    CHECK(same);
}

TEST_CASE("f32_to_s16 clamps out-of-range input and maps NaN to zero") {
    const float in[] = {2.0f, -2.0f, 1.0f, 0.5f, std::nanf(""), 0.0f};
    const std::vector<std::int16_t> out = ea::f32_to_s16(in);
    REQUIRE(out.size() == 6);
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32768);
    CHECK(out[2] == 32767);
    CHECK(out[3] == 16384);
    CHECK(out[4] == 0);
    CHECK(out[5] == 0);
}

TEST_CASE("WAV 16-bit mono round trip is bit-exact") {
    std::vector<std::int16_t> src = {0, 1, -1, 32767, -32768, 12345, -23456};
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist(-32768, 32767);
    for (int i = 0; i < 16000; ++i) src.push_back(static_cast<std::int16_t>(dist(rng)));

    const std::vector<float> f = ea::s16_to_f32(src);
    const fs::path path = temp_wav("roundtrip_mono");
    ea::write_wav16k_mono(path, f);

    const ea::PcmBuffer back = ea::read_wav(path);
    CHECK(back.rate == 16000);
    CHECK(back.channels == 1);
    REQUIRE(back.samples.size() == f.size());
    bool same = ea::f32_to_s16(back.samples) == src;
    CHECK(same);
    fs::remove(path);
}

TEST_CASE("WAV file layout is a canonical 44-byte header followed by s16le data") {
    const float mono[] = {0.5f, -0.5f, 0.0f};
    const fs::path path = temp_wav("layout");
    ea::write_wav16k_mono(path, mono);

    const std::vector<std::uint8_t> b = read_all_bytes(path);
    REQUIRE(b.size() == 44 + 6);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(b.data() + 12, "fmt ", 4) == 0);
    CHECK(std::memcmp(b.data() + 36, "data", 4) == 0);
    // RIFF size is file size - 8; data size is the sample bytes.
    CHECK(b[4] == 44 + 6 - 8);
    CHECK(b[40] == 6);
    // Rate 16000 = 0x3E80 little-endian at offset 24; one channel; 16 bits.
    CHECK(b[24] == 0x80);
    CHECK(b[25] == 0x3E);
    CHECK(b[22] == 1);
    CHECK(b[34] == 16);
    // 0.5 -> 16384 = 0x4000, -0.5 -> -16384 = 0xC000, both little-endian.
    CHECK(b[44] == 0x00);
    CHECK(b[45] == 0x40);
    CHECK(b[46] == 0x00);
    CHECK(b[47] == 0xC0);
    fs::remove(path);
}

TEST_CASE("WAV stereo 48 kHz keeps its rate and channel count on read") {
    std::vector<float> interleaved;
    for (int i = 0; i < 480; ++i) {
        interleaved.push_back(static_cast<float>(i - 240) / 512.0f);  // left
        interleaved.push_back(static_cast<float>(240 - i) / 512.0f);  // right
    }
    const fs::path path = temp_wav("stereo48k");
    ea::write_wav(path, interleaved, 48000, 2);

    const ea::PcmBuffer back = ea::read_wav(path);
    CHECK(back.rate == 48000);
    CHECK(back.channels == 2);
    REQUIRE(back.samples.size() == interleaved.size());
    bool same = ea::f32_to_s16(back.samples) == ea::f32_to_s16(interleaved);
    CHECK(same);
    fs::remove(path);
}

TEST_CASE("read_wav decodes IEEE float WAV exactly") {
    const float values[] = {0.25f, -0.5f, 1.0f, 0.0f};
    std::vector<std::uint8_t> data(sizeof(values));
    std::memcpy(data.data(), values, sizeof(values));

    const fs::path path = temp_wav("float32");
    write_raw_wav(path, 3, 32, 2, 44100, data);  // format tag 3 = IEEE float

    const ea::PcmBuffer back = ea::read_wav(path);
    CHECK(back.rate == 44100);
    CHECK(back.channels == 2);
    REQUIRE(back.samples.size() == 4);
    bool same = std::memcmp(back.samples.data(), values, sizeof(values)) == 0;
    CHECK(same);
    fs::remove(path);
}

TEST_CASE("read_wav throws on a missing file") {
    CHECK_THROWS_AS(ea::read_wav(temp_wav("does_not_exist_0001")), std::runtime_error);
}

TEST_CASE("write_wav rejects a partial frame") {
    const float odd[] = {0.0f, 0.0f, 0.0f};
    CHECK_THROWS_AS(ea::write_wav(temp_wav("partial"), odd, 16000, 2), std::invalid_argument);
}
