#include "base64.h"

#include <stdexcept>

namespace echoir::cli {
namespace {

// The 6-bit value of one alphabet character. Throws for anything outside the alphabet.
std::uint32_t sextet(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<std::uint32_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<std::uint32_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    throw std::invalid_argument("invalid base64 character");
}

}  // namespace

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        throw std::invalid_argument("base64 length is not a multiple of 4");
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const char c0 = text[i];
        const char c1 = text[i + 1];
        const char c2 = text[i + 2];
        const char c3 = text[i + 3];
        // Padding ends the data, so it may only close the last group. "xx==" is one byte, "xxx="
        // is two.
        if (c0 == '=' || c1 == '=' || (c2 == '=' && c3 != '=')) {
            throw std::invalid_argument("misplaced base64 padding");
        }
        const int pad = c3 == '=' ? (c2 == '=' ? 2 : 1) : 0;
        if (pad > 0 && !last) {
            throw std::invalid_argument("base64 padding is only allowed at the end");
        }
        const std::uint32_t six2 = pad >= 2 ? 0 : sextet(c2);
        const std::uint32_t six3 = pad >= 1 ? 0 : sextet(c3);
        // The bits that padding replaces must be zero.
        if ((pad == 2 && (sextet(c1) & 0x0F) != 0) || (pad == 1 && (six2 & 0x03) != 0)) {
            throw std::invalid_argument("non-zero bits in base64 padding");
        }
        const std::uint32_t group = (sextet(c0) << 18) | (sextet(c1) << 12) | (six2 << 6) | six3;
        out.push_back(static_cast<std::uint8_t>(group >> 16));
        if (pad < 2) {
            out.push_back(static_cast<std::uint8_t>(group >> 8));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::uint8_t>(group));
        }
    }
    return out;
}

}  // namespace echoir::cli
