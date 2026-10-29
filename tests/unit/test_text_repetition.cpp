#include <string>
#include <vector>

#include "doctest.h"
#include "text/repetition.h"

using echoir::text::echoes_history;
using echoir::text::is_repetitive;

TEST_CASE("is_repetitive: phrase repeats (.NET (.{3,40}?)\\1{3,})") {
    // With the leading space the fourth copy is complete.
    CHECK(is_repetitive(U" Thank you. Thank you. Thank you. Thank you."));
    // Without it the last copy is one character short, so there are only three full copies.
    // The regex does not match here, so this is false.
    CHECK_FALSE(is_repetitive(U"Thank you. Thank you. Thank you. Thank you."));
    // Three copies are not enough.
    CHECK_FALSE(is_repetitive(U"yes yes yes"));
}

TEST_CASE("is_repetitive: runaway runs of non-digits") {
    CHECK(is_repetitive(std::u32string(15, U'!')));
    // A newline is excluded from the phrase rule, so only the runaway rule can catch this run.
    CHECK_FALSE(is_repetitive(std::u32string(12, U'\n')));
    CHECK(is_repetitive(std::u32string(15, U'\n')));
}

TEST_CASE("is_repetitive: digit runs are exempt from the runaway rule only") {
    // Sixteen identical digits contain "111" four times, so the phrase rule still fires.
    // The phrase rule matches here, so this asserts true.
    CHECK(is_repetitive(std::u32string(16, U'1')));
    CHECK_FALSE(is_repetitive(U"12345678901234567890"));
}

TEST_CASE("is_repetitive: CJK phrase repeats count code points") {
    CHECK(is_repetitive(U"我爱你我爱你我爱你我爱你"));
    CHECK_FALSE(is_repetitive(U"我们今天去公园散步很开心"));
    // Each of these is one code point (two UTF-16 units), so the phrase is 3 code points.
    std::u32string astral;
    for (int i = 0; i < 4; ++i) {
        astral += U"\U00020000\U00020001\U00020002";
    }
    CHECK(is_repetitive(astral));
}

TEST_CASE("is_repetitive: 20 or more code points with at most 3 distinct") {
    CHECK(is_repetitive(U"aaaabbbbaaaabbbbaaaa"));
    // 19 code points, and no phrase repeats four times.
    CHECK_FALSE(is_repetitive(U"aaaabbbbaaaabbbbaaa"));
}

TEST_CASE("is_repetitive: normal speech and empty text are not repetitive") {
    CHECK_FALSE(is_repetitive(U""));
    CHECK_FALSE(
        is_repetitive(U"And so, my fellow Americans, ask not what your country can do for you."));
}

TEST_CASE("echoes_history: the history copied back, across entries, case and punctuation") {
    const std::vector<std::string> history = {"Or am I wrong?", "Understood, sir."};
    CHECK(echoes_history(U"Or am I wrong? Understood, sir.", history));
    CHECK(echoes_history(U"or am i wrong understood sir", history));
    CHECK(echoes_history(U"Understood, sir.", history));
    CHECK(echoes_history(U"wrong? Understood", history));
    CHECK_FALSE(echoes_history(U"I'm not sure.", history));
    CHECK_FALSE(echoes_history(U"Understood, sir. Okay.", history));
}

TEST_CASE("echoes_history: empty text, punctuation only and no history never match") {
    const std::vector<std::string> history = {"Understood, sir."};
    CHECK_FALSE(echoes_history(U"", history));
    CHECK_FALSE(echoes_history(U" ... ", history));
    CHECK_FALSE(echoes_history(U"Understood, sir.", {}));
}

TEST_CASE("echoes_history: CJK compares code points, and the ideographic space is ignored") {
    const std::vector<std::string> history = {"我们今天去公园", "散步很开心"};
    CHECK(echoes_history(U"公园\u3000散步", history));
    CHECK_FALSE(echoes_history(U"公园跑步", history));
}
