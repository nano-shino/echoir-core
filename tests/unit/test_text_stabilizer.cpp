#include <string>

#include "doctest.h"
#include "text/stabilizer.h"

using echoir::text::longest_common_prefix;
using echoir::text::Stabilizer;

TEST_CASE("longest_common_prefix works on code points") {
    CHECK(longest_common_prefix(U"", U"abc") == U"");
    CHECK(longest_common_prefix(U"abcd", U"abxy") == U"ab");
    CHECK(longest_common_prefix(U"abc", U"abc") == U"abc");
    // 你 and 好 are single code points, so a shared first character is one unit.
    CHECK(longest_common_prefix(U"你好", U"你坏") == U"你");
}

TEST_CASE("stabilizer: a prefix under 10 code points never locks") {
    Stabilizer s;
    CHECK(s.stabilize(U"Hi there") == U"Hi there");
    // The common prefix is "Hi there" (8 code points), so nothing locks yet.
    CHECK(s.stabilize(U"Hi there friend") == U"Hi there friend");
    CHECK(s.stabilize(U"Hallo there friend ok") == U"Hallo there friend ok");
}

TEST_CASE("stabilizer: a 10+ code point prefix locks, then drops on contradiction") {
    Stabilizer s;
    CHECK(s.stabilize(U"Hello there my friend") == U"Hello there my friend");
    // The common prefix is "Hello there my friend" (21 code points). The anchor is " my friend".
    CHECK(s.stabilize(U"Hello there my friend how") == U"Hello there my friend how");
    // The decoder changed the first word, but the locked prefix wins.
    CHECK(s.stabilize(U"Hallo there my friend how are") == U"Hello there my friend how are");
    // The anchor is gone from the new text, so the lock drops and the new text is shown.
    CHECK(s.stabilize(U"Completely different now") == U"Completely different now");
    CHECK(s.history().size() == 4);
    CHECK(s.history().back() == U"Completely different now");
}

TEST_CASE("stabilizer: CJK prefixes lock by code point") {
    Stabilizer s;
    CHECK(s.stabilize(U"我们今天去公园散步很开心") == U"我们今天去公园散步很开心");
    // The common prefix is all 12 code points. The anchor is 今天去公园散步很开心.
    CHECK(s.stabilize(U"我们今天去公园散步很开心的") == U"我们今天去公园散步很开心的");
    // The first two characters changed, but the locked prefix wins. The new tail is kept.
    CHECK(s.stabilize(U"咱们今天去公园散步很开心的吗") == U"我们今天去公园散步很开心的吗");
}

TEST_CASE("stabilizer: reset forgets history and the locked prefix") {
    Stabilizer s;
    s.stabilize(U"Hello there my friend");
    s.stabilize(U"Hello there my friend how");
    s.reset();
    CHECK(s.history().empty());
    CHECK(s.stabilize(U"Hallo there my friend how are") == U"Hallo there my friend how are");
}
