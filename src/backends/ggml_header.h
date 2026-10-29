// Reads the header of a ggml model file: the magic, then little-endian int32 fields in file order.
// Whisper and Silero VAD check their headers with this before whisper.cpp trusts them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <system_error>
#include <utility>

#include "errors.h"
#include "ggml.h"

namespace echoir::backends {

// True when path names a regular file. Callers check this before they open a model, because
// opening a named pipe with no writer blocks forever.
inline bool is_regular_model_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

class GgmlHeader {
public:
    // path names the file in errors, and kind names the model there, such as "Whisper".
    GgmlHeader(std::istream& in, std::string path, const char* kind)
        : in_(in), path_(std::move(path)), kind_(kind) {}

    void expect_magic() {
        if (field() != GGML_FILE_MAGIC) {
            reject("bad magic");
        }
    }

    // Each read throws if the file ends first.
    std::int32_t field() {
        unsigned char b[4] = {};
        in_.read(reinterpret_cast<char*>(b), sizeof b);
        if (in_.gcount() != static_cast<std::streamsize>(sizeof b)) {
            reject("the file ends inside the header");
        }
        const std::uint32_t bits = std::uint32_t{b[0]} | (std::uint32_t{b[1]} << 8) |
                                   (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
        return static_cast<std::int32_t>(bits);
    }

    void skip(std::size_t n) {
        in_.ignore(static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(in_.gcount()) != n) {
            reject("the file ends inside the header");
        }
    }

    void require_range(const char* name, std::int32_t value, std::int32_t lo,
                       std::int32_t hi) const {
        if (value < lo || value > hi) {
            reject(std::string(name) + " " + std::to_string(value) + " is outside " +
                   std::to_string(lo) + ".." + std::to_string(hi));
        }
    }

    [[noreturn]] void reject(const std::string& why) const {
        throw ModelLoadError(path_ + " is not a " + kind_ + " model (" + why + ")");
    }

private:
    std::istream& in_;
    std::string path_;
    const char* kind_;
};

}  // namespace echoir::backends
