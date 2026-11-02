// Round trip: spell numbers out in the ways people say them, and check normalize_numbers reads each
// one back to the same figure. Every number below ITN_SWEEP_LIMIT is checked in every phrasing, and
// a fixed pseudo-random sample covers the rest up to the trillions. Build with a larger
// ITN_SWEEP_LIMIT for an exhaustive run.

#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "text/itn.h"

#ifndef ITN_SWEEP_LIMIT
#define ITN_SWEEP_LIMIT 12000
#endif

using echoir::text::normalize_numbers;

namespace {

// Phrasings, as bit flags.
constexpr unsigned kAnd = 1;          // "two hundred and forty", "one thousand and five"
constexpr unsigned kHyphen = 2;       // "forty-two"
constexpr unsigned kArticle = 4;      // "a hundred", "a thousand" for a leading "one"
constexpr unsigned kTeenHundred = 8;  // "twelve hundred fifty" for 1100 to 9999
constexpr unsigned kFiller = 16;      // "uh" after the first word
constexpr unsigned kSpaces = 32;      // two spaces between words
constexpr unsigned kAllStyles = 64;

const char* const kUnits[] = {"",        "one",     "two",       "three",    "four",
                              "five",    "six",     "seven",     "eight",    "nine",
                              "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                              "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char* const kTens[] = {"",      "",      "twenty",  "thirty", "forty",
                             "fifty", "sixty", "seventy", "eighty", "ninety"};
const char* const kScales[] = {"", "thousand", "million", "billion", "trillion"};

void below_hundred(int n, unsigned style, std::vector<std::string>& out) {
    if (n < 20) {
        out.push_back(kUnits[n]);
    } else if (n % 10 == 0) {
        out.push_back(kTens[n / 10]);
    } else if (style & kHyphen) {
        out.push_back(std::string(kTens[n / 10]) + "-" + kUnits[n % 10]);
    } else {
        out.push_back(kTens[n / 10]);
        out.push_back(kUnits[n % 10]);
    }
}

void below_thousand(int n, unsigned style, std::vector<std::string>& out) {
    if (n >= 100) {
        out.push_back(kUnits[n / 100]);
        out.push_back("hundred");
        if (n % 100 != 0 && (style & kAnd)) {
            out.push_back("and");
        }
    }
    if (n % 100 != 0) {
        below_hundred(n % 100, style, out);
    }
}

std::vector<std::string> spell(long long n, unsigned style) {
    std::vector<std::string> out;
    if ((style & kTeenHundred) && n >= 1100 && n < 10000 && (n / 100) % 10 != 0) {
        below_hundred(static_cast<int>(n / 100), style, out);
        out.push_back("hundred");
        if (n % 100 != 0) {
            if (style & kAnd) {
                out.push_back("and");
            }
            below_hundred(static_cast<int>(n % 100), style, out);
        }
        return out;
    }
    int groups[5] = {};
    long long rest = n;
    for (int& g : groups) {
        g = static_cast<int>(rest % 1000);
        rest /= 1000;
    }
    bool higher = false;
    for (int s = 4; s >= 0; --s) {
        const int g = groups[s];
        if (g == 0) {
            continue;
        }
        if (s == 0 && higher && g < 100 && (style & kAnd)) {
            out.push_back("and");
        }
        below_thousand(g, style, out);
        if (s > 0) {
            out.push_back(kScales[s]);
        }
        higher = true;
    }
    if ((style & kArticle) && out.size() >= 2 && out[0] == "one" &&
        (out[1] == "hundred" || out[1] == "thousand" || out[1] == "million" ||
         out[1] == "billion" || out[1] == "trillion")) {
        out[0] = "a";
    }
    return out;
}

std::string join(const std::vector<std::string>& words, unsigned style) {
    std::string s;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            s += (style & kSpaces) ? "  " : " ";
        }
        s += words[i];
        if (i == 0 && words.size() > 1 && (style & kFiller)) {
            s += " uh";
        }
    }
    return s;
}

// The figure for n: digits, or "1.25 billion" when the top scale is a million or more and the
// value is a whole number of thousandths of it.
std::string figure(long long n) {
    const long long scales[] = {1000000000000LL, 1000000000LL, 1000000LL};
    const char* const names[] = {"trillion", "billion", "million"};
    for (int k = 0; k < 3; ++k) {
        const long long s = scales[k];
        if (n < s) {
            continue;
        }
        if (n % (s / 1000) != 0) {
            break;
        }
        std::string out = std::to_string(n / s);
        long long frac = (n % s) / (s / 1000);
        if (frac != 0) {
            std::string d = std::to_string(frac);
            d.insert(0, 3 - d.size(), '0');
            while (d.back() == '0') {
                d.pop_back();
            }
            out += "." + d;
        }
        return out + " " + names[k];
    }
    return std::to_string(n);
}

const char* ordinal_of(const std::string& word) {
    static const char* const kPairs[][2] = {{"one", "first"},
                                            {"two", "second"},
                                            {"three", "third"},
                                            {"four", "fourth"},
                                            {"five", "fifth"},
                                            {"six", "sixth"},
                                            {"seven", "seventh"},
                                            {"eight", "eighth"},
                                            {"nine", "ninth"},
                                            {"ten", "tenth"},
                                            {"eleven", "eleventh"},
                                            {"twelve", "twelfth"},
                                            {"thirteen", "thirteenth"},
                                            {"fourteen", "fourteenth"},
                                            {"fifteen", "fifteenth"},
                                            {"sixteen", "sixteenth"},
                                            {"seventeen", "seventeenth"},
                                            {"eighteen", "eighteenth"},
                                            {"nineteen", "nineteenth"},
                                            {"twenty", "twentieth"},
                                            {"thirty", "thirtieth"},
                                            {"forty", "fortieth"},
                                            {"fifty", "fiftieth"},
                                            {"sixty", "sixtieth"},
                                            {"seventy", "seventieth"},
                                            {"eighty", "eightieth"},
                                            {"ninety", "ninetieth"},
                                            {"hundred", "hundredth"},
                                            {"thousand", "thousandth"},
                                            {"million", "millionth"},
                                            {"billion", "billionth"},
                                            {"trillion", "trillionth"}};
    for (const auto& p : kPairs) {
        if (word == p[0]) {
            return p[1];
        }
    }
    return nullptr;
}

std::string suffix(long long n) {
    if (n % 100 >= 11 && n % 100 <= 13) {
        return "th";
    }
    switch (n % 10) {
        case 1:
            return "st";
        case 2:
            return "nd";
        case 3:
            return "rd";
        default:
            return "th";
    }
}

// Whether the phrasing is "a hundred", "a thousand" and so on alone, which stays as words.
bool lone_article(const std::vector<std::string>& words) {
    return words.size() == 2 && words[0] == "a";
}

// Checks one number in one phrasing, as a cardinal, an ordinal and in a sentence. Returns false
// and reports the first failure.
bool check(long long n, unsigned style) {
    std::vector<std::string> words = spell(n, style);
    // A hyphen joins only two words. "twenty-five" with a filler inside it is not a phrasing.
    const std::string spoken = join(words, style);
    const std::string expect = lone_article(words) ? spoken : figure(n);
    const std::string in = "we saw " + spoken + " today.";
    const std::string want = "we saw " + expect + " today.";
    const std::string got = normalize_numbers(in);
    if (got != want) {
        FAIL_CHECK("cardinal " << n << " style " << style << ": \"" << in << "\" -> \"" << got
                               << "\", want \"" << want << "\"");
        return false;
    }

    // The ordinal: the last word, or the last half of a hyphenated word, takes its ordinal form.
    std::string& last = words.back();
    const std::size_t dash = last.find('-');
    const std::string tail = dash == std::string::npos ? last : last.substr(dash + 1);
    const char* ord = ordinal_of(tail);
    if (ord == nullptr) {
        return true;
    }
    last = (dash == std::string::npos ? std::string() : last.substr(0, dash + 1)) + ord;
    const std::string ord_in = "the " + join(words, style) + " time";
    const std::string ord_want =
        lone_article(words) ? ord_in : "the " + std::to_string(n) + suffix(n) + " time";
    const std::string ord_got = normalize_numbers(ord_in);
    if (ord_got != ord_want) {
        FAIL_CHECK("ordinal " << n << " style " << style << ": \"" << ord_in << "\" -> \""
                              << ord_got << "\", want \"" << ord_want << "\"");
        return false;
    }
    return true;
}

// Whether a phrasing applies to n, so the sweep does not test one phrasing twice.
bool applies(long long n, unsigned style) {
    if ((style & kTeenHundred) && !(n >= 1100 && n < 10000 && (n / 100) % 10 != 0)) {
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("normalize_numbers reads back every number from ten up, in every phrasing") {
    int failures = 0;
    for (long long n = 10; n < ITN_SWEEP_LIMIT && failures < 20; ++n) {
        for (unsigned style = 0; style < kAllStyles && failures < 20; ++style) {
            if (applies(n, style) && !check(n, style)) {
                ++failures;
            }
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("normalize_numbers reads back large numbers, in every phrasing") {
    // A fixed linear congruential sequence, so the sample is the same on every run.
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    int failures = 0;
    for (int k = 0; k < 20000 && failures < 20; ++k) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const int digits = 2 + static_cast<int>((state >> 59) % 14);  // 2 to 15 digits
        long long n = static_cast<long long>((state >> 8) % 1000000000000000ULL);
        long long cap = 1;
        for (int d = 0; d < digits; ++d) {
            cap *= 10;
        }
        n %= cap;
        // Round some to whole thousands or millions, the figures news reads out most.
        if (k % 3 == 1) {
            n -= n % 1000;
        } else if (k % 3 == 2) {
            n -= n % 1000000;
        }
        if (n < 10) {
            continue;
        }
        const unsigned style = static_cast<unsigned>(state >> 40) % kAllStyles;
        if (applies(n, style) && !check(n, style)) {
            ++failures;
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("normalize_numbers reads back every year from 1300 to 2099") {
    for (int y = 1300; y < 2100; ++y) {
        const int century = y / 100;
        const int rest = y % 100;
        std::vector<std::string> words;
        below_hundred(century, 0, words);
        if (rest == 0) {
            words.push_back("hundred");
        } else if (rest < 10) {
            words.push_back("oh");
            words.push_back(kUnits[rest]);
        } else {
            below_hundred(rest, 0, words);
        }
        const std::string in = "back in " + join(words, 0) + ", it rained";
        const std::string want = "back in " + std::to_string(y) + ", it rained";
        CHECK_MESSAGE(normalize_numbers(in) == want, in);
    }
}
