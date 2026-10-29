#include <string>

#include "doctest.h"
#include "text/clean.h"

using echoir::text::clean_segment;
using echoir::text::collapse_runaway;

TEST_CASE("clean_segment removes the BLANK_AUDIO marker with its leading space") {
    CHECK(clean_segment(U" [BLANK_AUDIO]") == U"");
    CHECK(clean_segment(U"Hello [BLANK_AUDIO] world") == U"Hello world");
    CHECK(clean_segment(U"Hi [BLANK_AUDIO] [BLANK_AUDIO]") == U"Hi");
}

TEST_CASE("clean_segment trims only leading U+0020") {
    CHECK(clean_segment(U"   hi there") == U"hi there");
    CHECK(clean_segment(U"   ") == U"");
    // Only the space is trimmed, as C# TrimStart(' ') does. A tab stays.
    CHECK(clean_segment(U"\thi") == U"\thi");
}

TEST_CASE("clean_segment collapses runs of 15 non-digits, but not 14") {
    CHECK(clean_segment(U"ah" + std::u32string(15, U'!')) == U"ah!");
    CHECK(clean_segment(U"ah" + std::u32string(14, U'!')) == U"ah" + std::u32string(14, U'!'));
}

TEST_CASE("collapse_runaway collapses each long run to one code point") {
    CHECK(collapse_runaway(U"a" + std::u32string(15, U'b') + U"c") == U"abc");
    // Runs of different characters are independent.
    CHECK(collapse_runaway(std::u32string(16, U'x') + std::u32string(16, U'y')) == U"xy");
    // A CJK character is non-digit, so it collapses too.
    CHECK(collapse_runaway(std::u32string(15, char32_t{0x6211})) == U"我");
    // Newlines are non-digits as well.
    CHECK(collapse_runaway(std::u32string(16, U'\n')) == U"\n");
}

TEST_CASE("collapse_runaway leaves digit runs alone") {
    CHECK(collapse_runaway(std::u32string(20, U'1')) == std::u32string(20, U'1'));
    CHECK(collapse_runaway(U"0000000000000000000") == U"0000000000000000000");
}

TEST_CASE("collapse_runaway leaves short and empty input unchanged") {
    CHECK(collapse_runaway(U"") == U"");
    CHECK(collapse_runaway(U"aaaabbbbcccc") == U"aaaabbbbcccc");
}
