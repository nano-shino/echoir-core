#include "text/utf.h"

#include <cstddef>

namespace echoir::text {
namespace {

constexpr char32_t kReplacement = U'�';

}  // namespace

std::u32string utf8_to_utf32(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    const std::size_t n = utf8.size();
    std::size_t i = 0;
    while (i < n) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80) {
            out.push_back(static_cast<char32_t>(lead));
            ++i;
            continue;
        }
        // The lead byte sets the length. The second byte's range rules out overlongs (E0, F0),
        // surrogates (ED) and values above U+10FFFF (F4).
        std::size_t len = 0;
        char32_t cp = 0;
        unsigned char second_lo = 0x80;
        unsigned char second_hi = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            len = 2;
            cp = static_cast<char32_t>(lead & 0x1F);
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            len = 3;
            cp = static_cast<char32_t>(lead & 0x0F);
            if (lead == 0xE0) second_lo = 0xA0;
            if (lead == 0xED) second_hi = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            len = 4;
            cp = static_cast<char32_t>(lead & 0x07);
            if (lead == 0xF0) second_lo = 0x90;
            if (lead == 0xF4) second_hi = 0x8F;
        } else {
            // A stray continuation byte, or C0, C1 and F5..FF, which never start a sequence.
            out.push_back(kReplacement);
            ++i;
            continue;
        }

        std::size_t j = i + 1;
        bool complete = true;
        for (std::size_t k = 1; k < len; ++k, ++j) {
            if (j >= n) {
                complete = false;
                break;
            }
            const auto c = static_cast<unsigned char>(utf8[j]);
            const unsigned char lo = (k == 1) ? second_lo : 0x80;
            const unsigned char hi = (k == 1) ? second_hi : 0xBF;
            if (c < lo || c > hi) {
                complete = false;
                break;
            }
            cp = (cp << 6) | static_cast<char32_t>(c & 0x3F);
        }
        if (!complete) {
            // One replacement for the valid prefix. The byte at j is decoded on the next pass.
            out.push_back(kReplacement);
            i = j;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string utf32_to_utf8(std::u32string_view utf32) {
    std::string out;
    out.reserve(utf32.size());
    for (char32_t cp : utf32) {
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            cp = kReplacement;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

}  // namespace echoir::text
