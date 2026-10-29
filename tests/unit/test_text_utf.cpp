#include <string>
#include <string_view>

#include "doctest.h"
#include "text/utf.h"

using echoir::text::utf32_to_utf8;
using echoir::text::utf8_to_utf32;

TEST_CASE("utf8 round-trips 1, 2, 3 and 4 byte code points") {
    const std::u32string text = U"Aé你\U0001F600";
    const std::string utf8 = utf32_to_utf8(text);
    CHECK(utf8 == "A\xC3\xA9\xE4\xBD\xA0\xF0\x9F\x98\x80");
    CHECK(utf8_to_utf32(utf8) == text);
}

TEST_CASE("a 4-byte UTF-8 sequence is one code point") {
    CHECK(utf8_to_utf32("\xF0\x9F\x98\x80").size() == 1);
    CHECK(utf8_to_utf32("\xF0\x9F\x98\x80")[0] == U'\U0001F600');
}

TEST_CASE("invalid UTF-8 becomes U+FFFD and keeps the valid text") {
    CHECK(utf8_to_utf32("\xFF") == U"�");                       // never valid
    CHECK(utf8_to_utf32("\x80") == U"�");                       // stray continuation
    CHECK(utf8_to_utf32("\xC0\xAF") == U"��");                  // overlong '/'
    CHECK(utf8_to_utf32("\xE4\xBD") == U"�");                   // truncated: one for the prefix
    CHECK(utf8_to_utf32("\xE4\xBDx") == U"�x");                 // broken sequence keeps the 'x'
    CHECK(utf8_to_utf32("\xED\xA0\x80") == U"���");             // encoded surrogate
    CHECK(utf8_to_utf32("ok\xF4\x90\x80\x80!") == U"ok����!");  // above U+10FFFF
}

TEST_CASE("encoding replaces surrogates and out-of-range values") {
    CHECK(utf32_to_utf8(std::u32string(1, char32_t{0xD800})) == "\xEF\xBF\xBD");
    CHECK(utf32_to_utf8(std::u32string(1, char32_t{0x110000})) == "\xEF\xBF\xBD");
}

TEST_CASE("empty input converts to empty output") {
    CHECK(utf8_to_utf32("").empty());
    CHECK(utf32_to_utf8(U"").empty());
}
