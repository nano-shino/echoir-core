#include <cstddef>
#include <string>

#include "doctest.h"
#include "text/score.h"
#include "text/utf.h"

using echoir::text::error_rate;
using echoir::text::normalize_for_score;
using echoir::text::utf32_to_utf8;

namespace {

// Case folding as the spec defines it: ASCII and Latin-1 capitals only.
char32_t fold_ascii_latin1(char32_t c) {
    if (c >= U'A' && c <= U'Z') {
        return c + 32;
    }
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
        return c + 32;
    }
    return c;
}

}  // namespace

TEST_CASE("normalize_for_score lowercases and drops punctuation") {
    CHECK(normalize_for_score("And so, my fellow Americans") == "and so my fellow americans");
    CHECK(normalize_for_score("don't stop!") == "dont stop");
    CHECK(normalize_for_score("  a \t b\n\nc  ") == "a b c");
    // Latin-1 capitals fold to lowercase.
    CHECK(normalize_for_score("ÉCOLE Ça") == "école ça");
    // CJK punctuation is dropped, and the characters stay as they are.
    CHECK(normalize_for_score("你好，世界。") == "你好世界");
}

TEST_CASE("error_rate: identical or normalized-equal text scores zero") {
    const auto same = error_rate("one two three", "one two three");
    CHECK(same.rate == doctest::Approx(0.0));
    CHECK(same.ref_len == 3);
    CHECK(error_rate("And so, my fellow Americans", "and so my fellow americans").rate ==
          doctest::Approx(0.0));
    CHECK(error_rate("École", "ÉCOLE").rate == doctest::Approx(0.0));
}

TEST_CASE("error_rate: one substituted word in four is 0.25") {
    const auto r = error_rate("one two three five", "one two three four");
    CHECK(r.subs == 1);
    CHECK(r.dels == 0);
    CHECK(r.ins == 0);
    CHECK(r.ref_len == 4);
    CHECK(r.rate == doctest::Approx(0.25));
}

TEST_CASE("error_rate: an empty hypothesis is 1.0") {
    const auto r = error_rate("", "one two");
    CHECK(r.dels == 2);
    CHECK(r.rate == doctest::Approx(1.0));
}

TEST_CASE("error_rate: insertions and deletions are counted apart") {
    const auto ins = error_rate("one two three four five", "one two three four");
    CHECK(ins.ins == 1);
    CHECK(ins.dels == 0);
    CHECK(ins.rate == doctest::Approx(0.25));

    const auto del = error_rate("one two three", "one two three four");
    CHECK(del.dels == 1);
    CHECK(del.ins == 0);
    CHECK(del.rate == doctest::Approx(0.25));
}

TEST_CASE("error_rate: CJK scores per code point") {
    // The reference is 你好世界 (4 units), and the hypothesis lost the last character.
    CHECK(error_rate("你好世", "你好世界").rate == doctest::Approx(0.25));
    // The reverse adds one character to a 3-unit reference.
    const auto r = error_rate("你好世界", "你好世");
    CHECK(r.ins == 1);
    CHECK(r.rate == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("error_rate: mixed text counts words and CJK units together") {
    // "hello" is one word, and each of 你 and 好 is one unit. The last unit is substituted.
    const auto r = error_rate("hello 你好", "hello 你们");
    CHECK(r.subs == 1);
    CHECK(r.ref_len == 3);
    CHECK(r.rate == doctest::Approx(1.0 / 3.0));
    // CJK punctuation drops out before the alignment.
    CHECK(error_rate("你好，世界。", "你好世界").rate == doctest::Approx(0.0));
}

TEST_CASE("error_rate: each Han, Hangul and kana code point is one unit") {
    // One code point from each block, between two Latin letters. Each must make three units.
    const char32_t samples[] = {
        0x3005,   // IDEOGRAPHIC ITERATION MARK
        0x3041,   // HIRAGANA LETTER SMALL A
        0x3099,   // COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
        0x30A2,   // KATAKANA LETTER A
        0x30FC,   // KATAKANA-HIRAGANA PROLONGED SOUND MARK
        0x31F0,   // KATAKANA LETTER SMALL KU (phonetic extensions)
        0x3131,   // HANGUL LETTER KIYEOK (compatibility jamo)
        0x3164,   // HANGUL FILLER
        0x3400,   // CJK UNIFIED IDEOGRAPH-3400 (extension A)
        0x4E00,   // CJK UNIFIED IDEOGRAPH-4E00
        0x1100,   // HANGUL CHOSEONG KIYEOK (jamo)
        0xA960,   // HANGUL CHOSEONG TIKEUT-MIEUM (jamo extended-A)
        0xAC00,   // HANGUL SYLLABLE GA
        0xD7B0,   // HANGUL JUNGSEONG O-YEO (jamo extended-B)
        0xF900,   // CJK COMPATIBILITY IDEOGRAPH-F900
        0xFF66,   // HALFWIDTH KATAKANA LETTER WO
        0xFF70,   // HALFWIDTH KATAKANA-HIRAGANA PROLONGED SOUND MARK
        0xFF9E,   // HALFWIDTH KATAKANA VOICED SOUND MARK
        0xFFA0,   // HALFWIDTH HANGUL FILLER
        0xFFDC,   // HALFWIDTH HANGUL LETTER I
        0x1AFF0,  // KATAKANA LETTER MINNAN TONE-2 (kana extended-B)
        0x1B000,  // KATAKANA LETTER ARCHAIC E (kana supplement)
        0x1B100,  // HENTAIGANA LETTER RE-3 (kana extended-A)
        0x1B132,  // HIRAGANA LETTER SMALL KO (small kana extension)
        0x20000,  // CJK UNIFIED IDEOGRAPH-20000 (extension B)
        0x2EBF0,  // CJK UNIFIED IDEOGRAPH-2EBF0 (extension I)
        0x2F800,  // CJK COMPATIBILITY IDEOGRAPH-2F800
        0x31350,  // CJK UNIFIED IDEOGRAPH-31350 (extension H, the first code point)
        0x323AF,  // CJK UNIFIED IDEOGRAPH-323AF (extension H, the last code point)
    };
    for (char32_t cp : samples) {
        INFO("code point (decimal) " << static_cast<unsigned long>(cp));
        CHECK(error_rate("", utf32_to_utf8(std::u32string{U'a', cp, U'b'})).ref_len == 3);
    }
    // Punctuation is dropped, and Bopomofo is not one of the scripts above, so neither is a unit.
    CHECK(error_rate("", utf32_to_utf8(std::u32string{U'a', 0x3001, U'b'})).ref_len == 1);  // 、
    CHECK(error_rate("", utf32_to_utf8(std::u32string{U'a', 0x3105, U'b'})).ref_len == 1);  // ㄅ
}

TEST_CASE("error_rate: CJK unit count over all code points is pinned") {
    // A code point is a unit when "a" + cp + "b" has three units. The count covers the ranges in
    // score.cpp for Unicode 16.0.0, including unassigned code points inside them. It changes only
    // when those ranges change.
    std::size_t units = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;  // surrogates are not valid UTF-32 text
        }
        if (error_rate("", utf32_to_utf8(std::u32string{U'a', cp, U'b'})).ref_len == 3) {
            ++units;
        }
    }
    CHECK(units == 115107);
}

TEST_CASE("error_rate: empty reference uses a denominator of 1") {
    const auto both = error_rate("", "");
    CHECK(both.ref_len == 0);
    CHECK(both.rate == doctest::Approx(0.0));

    const auto ins_only = error_rate("two words", "");
    CHECK(ins_only.ref_len == 0);
    CHECK(ins_only.ins == 2);
    CHECK(ins_only.rate == doctest::Approx(2.0));
}

TEST_CASE("normalize_for_score drops punctuation and symbols from other scripts") {
    CHECK(normalize_for_score("a\u060Cb") == "ab");        // ARABIC COMMA (Po)
    CHECK(normalize_for_score("a\u0964b") == "ab");        // DEVANAGARI DANDA (Po)
    CHECK(normalize_for_score("a\u037Eb") == "ab");        // GREEK QUESTION MARK (Po)
    CHECK(normalize_for_score("a\u2122b") == "ab");        // TRADE MARK SIGN (So)
    CHECK(normalize_for_score("a\u2116b") == "ab");        // NUMERO SIGN (So)
    CHECK(normalize_for_score("a\uFFE5b") == "ab");        // FULLWIDTH YEN SIGN (Sc)
    CHECK(normalize_for_score("a\u20ACb") == "ab");        // EURO SIGN (Sc)
    CHECK(normalize_for_score("a\u00B0b") == "ab");        // DEGREE SIGN (So)
    CHECK(normalize_for_score("a\U0001F600b") == "ab");    // GRINNING FACE (So)
    CHECK(normalize_for_score("a\u2014b") == "ab");        // EM DASH (Pd)
    CHECK(normalize_for_score("a\u201Cb\u201D") == "ab");  // curly quotes (Pi, Pf)
}

TEST_CASE("normalize_for_score keeps letters and digits") {
    CHECK(normalize_for_score("\u00AA\u00B5\u00BA") ==
          "\u00AA\u00B5\u00BA");  // Latin-1 letters: ordinal indicators and micro sign
    CHECK(normalize_for_score("123") == "123");
    CHECK(normalize_for_score("\u0928\u092E\u0938\u094D\u0924\u0947") ==  // Devanagari word
          "\u0928\u092E\u0938\u094D\u0924\u0947");
    CHECK(normalize_for_score("\u3053\u3093\u306B\u3061\u306F") ==  // Hiragana greeting
          "\u3053\u3093\u306B\u3061\u306F");
}

TEST_CASE("normalize_for_score drops the byte-order mark and other format characters") {
    // A BOM at the start of a reference file must not stick to the first word.
    CHECK(normalize_for_score("\xEF\xBB\xBF"
                              "hello world") == "hello world");
    CHECK(normalize_for_score("\u200Bhello") == "hello");  // ZERO WIDTH SPACE (Cf)
    CHECK(normalize_for_score("he\u202Allo") == "hello");  // LEFT-TO-RIGHT EMBEDDING (Cf)
    CHECK(normalize_for_score("he\u00ADllo") == "hello");  // SOFT HYPHEN (Cf)
    CHECK(normalize_for_score("he\uFE0Fllo") == "hello");  // VARIATION SELECTOR-16 (Mn)
    CHECK(error_rate("\xEF\xBB\xBF"
                     "hello world",
                     "hello world")
              .rate == doctest::Approx(0.0));
}

TEST_CASE("error_rate: punctuation from other scripts does not count") {
    CHECK(error_rate("\u0928\u092E\u0938\u094D\u0924\u0947\u0964",
                     "\u0928\u092E\u0938\u094D\u0924\u0947")
              .rate == doctest::Approx(0.0));
    CHECK(error_rate("hello\u060C world", "hello world").rate == doctest::Approx(0.0));
}

TEST_CASE("normalize_for_score: every code point is dropped, spaced, or kept") {
    // Each code point is checked in the context "a" + cp + "b". The counts are for Unicode 16.0.0
    // (tools/gen-score-tables.py). Update them if the table is regenerated.
    std::size_t dropped = 0;
    std::size_t spaces = 0;
    char32_t first_bad = 0;
    std::size_t bad = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;  // surrogates are not valid UTF-32 text
        }
        const std::string out = normalize_for_score(utf32_to_utf8(std::u32string{U'a', cp, U'b'}));
        if (out == "ab") {
            ++dropped;
        } else if (out == "a b") {
            ++spaces;
        } else if (out != utf32_to_utf8(std::u32string{U'a', fold_ascii_latin1(cp), U'b'})) {
            if (bad == 0) {
                first_bad = cp;
            }
            ++bad;
        }
    }
    CHECK(bad == 0);
    CHECK(first_bad == 0);
    CHECK(spaces == 25);
    CHECK(dropped == 9854);
}
