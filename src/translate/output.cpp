#include "translate/output.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace echoir::translate {
namespace {

// The length of the valid UTF-8 sequence at the start of text, or 0 when there is none. Overlong
// forms, surrogates and values above U+10FFFF are invalid.
std::size_t sequence_length(std::string_view text) {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = at(0);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;  // the range of the second byte
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
            low = 0xA0;
        } else if (lead == 0xED) {
            high = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
            low = 0x90;
        } else if (lead == 0xF4) {
            high = 0x8F;
        }
    } else {
        return 0;
    }
    if (text.size() < length || at(1) < low || at(1) > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if ((at(i) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return length;
}

}  // namespace

std::string clean_output(std::string_view raw) {
    std::string clean;
    clean.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const std::size_t length = sequence_length(raw.substr(i));
        if (length == 0) {
            ++i;
            continue;
        }
        clean.append(raw.substr(i, length));
        i += length;
    }
    const char* spaces = " \t\r\n";
    const std::size_t begin = clean.find_first_not_of(spaces);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = clean.find_last_not_of(spaces);
    return clean.substr(begin, end - begin + 1);
}

}  // namespace echoir::translate
