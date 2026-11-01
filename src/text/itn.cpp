#include "text/itn.h"

#include <cstddef>
#include <string>
#include <vector>

namespace echoir::text {
namespace {

enum class Kind { Zero, Unit, Teen, Tens, Hundred, Scale, Point, Percent, Dollars };

struct Entry {
    std::string_view word;  // lowercase
    Kind kind;
    long long value;
    bool ordinal;
};

// Unit is one to nine, Teen ten to nineteen, Tens twenty to ninety. Zero is only a digit word, for
// decimals. The ordinal forms are terminal: they end a number, as in "twenty-first".
constexpr Entry kEntries[] = {
    {"zero", Kind::Zero, 0, false},
    {"one", Kind::Unit, 1, false},
    {"two", Kind::Unit, 2, false},
    {"three", Kind::Unit, 3, false},
    {"four", Kind::Unit, 4, false},
    {"five", Kind::Unit, 5, false},
    {"six", Kind::Unit, 6, false},
    {"seven", Kind::Unit, 7, false},
    {"eight", Kind::Unit, 8, false},
    {"nine", Kind::Unit, 9, false},
    {"ten", Kind::Teen, 10, false},
    {"eleven", Kind::Teen, 11, false},
    {"twelve", Kind::Teen, 12, false},
    {"thirteen", Kind::Teen, 13, false},
    {"fourteen", Kind::Teen, 14, false},
    {"fifteen", Kind::Teen, 15, false},
    {"sixteen", Kind::Teen, 16, false},
    {"seventeen", Kind::Teen, 17, false},
    {"eighteen", Kind::Teen, 18, false},
    {"nineteen", Kind::Teen, 19, false},
    {"twenty", Kind::Tens, 20, false},
    {"thirty", Kind::Tens, 30, false},
    {"forty", Kind::Tens, 40, false},
    {"fifty", Kind::Tens, 50, false},
    {"sixty", Kind::Tens, 60, false},
    {"seventy", Kind::Tens, 70, false},
    {"eighty", Kind::Tens, 80, false},
    {"ninety", Kind::Tens, 90, false},
    {"hundred", Kind::Hundred, 100, false},
    {"thousand", Kind::Scale, 1000LL, false},
    {"million", Kind::Scale, 1000000LL, false},
    {"billion", Kind::Scale, 1000000000LL, false},
    {"trillion", Kind::Scale, 1000000000000LL, false},
    {"point", Kind::Point, 0, false},
    {"percent", Kind::Percent, 0, false},
    {"dollars", Kind::Dollars, 0, false},
    {"first", Kind::Unit, 1, true},
    {"second", Kind::Unit, 2, true},
    {"third", Kind::Unit, 3, true},
    {"fourth", Kind::Unit, 4, true},
    {"fifth", Kind::Unit, 5, true},
    {"sixth", Kind::Unit, 6, true},
    {"seventh", Kind::Unit, 7, true},
    {"eighth", Kind::Unit, 8, true},
    {"ninth", Kind::Unit, 9, true},
    {"tenth", Kind::Teen, 10, true},
    {"eleventh", Kind::Teen, 11, true},
    {"twelfth", Kind::Teen, 12, true},
    {"thirteenth", Kind::Teen, 13, true},
    {"fourteenth", Kind::Teen, 14, true},
    {"fifteenth", Kind::Teen, 15, true},
    {"sixteenth", Kind::Teen, 16, true},
    {"seventeenth", Kind::Teen, 17, true},
    {"eighteenth", Kind::Teen, 18, true},
    {"nineteenth", Kind::Teen, 19, true},
    {"twentieth", Kind::Tens, 20, true},
    {"thirtieth", Kind::Tens, 30, true},
    {"fortieth", Kind::Tens, 40, true},
    {"fiftieth", Kind::Tens, 50, true},
    {"sixtieth", Kind::Tens, 60, true},
    {"seventieth", Kind::Tens, 70, true},
    {"eightieth", Kind::Tens, 80, true},
    {"ninetieth", Kind::Tens, 90, true},
    {"hundredth", Kind::Hundred, 100, true},
    {"thousandth", Kind::Scale, 1000LL, true},
    {"millionth", Kind::Scale, 1000000LL, true},
    {"billionth", Kind::Scale, 1000000000LL, true},
    {"trillionth", Kind::Scale, 1000000000000LL, true},
};

const Entry* lookup(const std::string& lower) {
    for (const Entry& e : kEntries) {
        if (e.word == lower) {
            return &e;
        }
    }
    return nullptr;
}

// A maximal run of ASCII letters. The lowercase copy is what the number table is matched against.
struct Word {
    std::size_t begin;
    std::size_t end;
    std::string lower;
};

bool is_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::vector<Word> split_words(std::string_view text) {
    std::vector<Word> words;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!is_ascii_letter(text[i])) {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        std::string lower;
        while (i < text.size() && is_ascii_letter(text[i])) {
            const char c = text[i];
            lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            ++i;
        }
        words.push_back({begin, i, std::move(lower)});
    }
    return words;
}

// Two words belong to one number only when one space or one hyphen separates them. "four-point"
// joins and "build-out" does not, because "out" is not a number word.
bool joins(std::string_view text, const Word& prev, const Word& next) {
    if (next.begin - prev.end != 1) {
        return false;
    }
    const char sep = text[prev.end];
    return sep == ' ' || sep == '-';
}

// The value of a run of number words, built one word at a time. A word is added only where the
// spoken order allows it, so "five six" is two numbers and "twenty five" is one.
struct Number {
    long long total = 0;  // the groups closed by a scale word ("two thousand" -> 2000)
    long long group = 0;  // the part below the last scale word
    long long head = 0;   // the value before the final scale word, for "six point three billion"
    const Entry* scale = nullptr;  // the last scale word added
    bool ends_in_scale = false;    // the last word added was a scale word
    bool multi_scale = false;      // more than one scale word, so no "N billion" form
    bool ordinal = false;

    long long whole() const { return total + group; }
};

// Adds a number word. Returns false, and leaves n unchanged, when the word cannot follow.
bool add(Number& n, const Entry& e) {
    const long long rem = n.group % 100;
    switch (e.kind) {
        case Kind::Unit:
            // A unit follows a fresh hundred or a whole ten: "one hundred five", "twenty three".
            if (rem != 0 && !(rem >= 20 && rem % 10 == 0)) {
                return false;
            }
            n.group += e.value;
            break;
        case Kind::Teen:
        case Kind::Tens:
            if (rem != 0) {
                return false;
            }
            n.group += e.value;
            break;
        case Kind::Hundred:
            if (n.group == 0) {
                n.group = 100;
            } else if (n.group < 10) {
                n.group *= 100;
            } else {
                return false;
            }
            break;
        case Kind::Scale: {
            // Scale words only shrink: "two million five thousand", never "thousand million".
            if (n.scale && e.value >= n.scale->value) {
                return false;
            }
            const long long g = n.group == 0 ? 1 : n.group;
            if (n.total > 0) {
                n.multi_scale = true;
            }
            n.head = n.total + g;
            n.total += g * e.value;
            n.group = 0;
            n.scale = &e;
            break;
        }
        default:
            return false;
    }
    n.ends_in_scale = e.kind == Kind::Scale;
    if (e.ordinal) {
        n.ordinal = true;
    }
    return true;
}

struct Match {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t next = 0;  // the index of the first word after the match
    Number number;
    std::string frac;                    // digits after "point", empty if none
    const Entry* after_scale = nullptr;  // "six point three billion": the scale after the decimal
    bool percent = false;
    bool dollars = false;
};

// Matches a number that starts at word `first`. It returns false when no number starts there.
// A number starts with a unit, teen or ten word, so "hundred" or "billion" alone stay as they are.
bool match_at(std::string_view text, const std::vector<Word>& words, std::size_t first, Match& m) {
    const Entry* start = lookup(words[first].lower);
    if (!start ||
        (start->kind != Kind::Unit && start->kind != Kind::Teen && start->kind != Kind::Tens)) {
        return false;
    }

    Number n;
    std::size_t j = first;
    while (j < words.size()) {
        if (j > first && !joins(text, words[j - 1], words[j])) {
            break;
        }
        const Entry* e = lookup(words[j].lower);
        if (!e || !add(n, *e)) {
            break;
        }
        ++j;
        if (e->ordinal) {
            break;
        }
    }

    m.begin = words[first].begin;
    m.number = n;
    if (!n.ordinal) {
        // A decimal: "four point five". It needs a plain integer part, not "two billion point".
        if (!n.scale && j < words.size() && joins(text, words[j - 1], words[j]) &&
            words[j].lower == "point") {
            std::size_t k = j + 1;
            std::string frac;
            while (k < words.size() && joins(text, words[k - 1], words[k])) {
                const Entry* d = lookup(words[k].lower);
                if (!d || d->ordinal || (d->kind != Kind::Zero && d->kind != Kind::Unit)) {
                    break;
                }
                frac += static_cast<char>('0' + d->value);
                ++k;
            }
            if (!frac.empty()) {
                m.frac = frac;
                j = k;
                if (j < words.size() && joins(text, words[j - 1], words[j])) {
                    const Entry* s = lookup(words[j].lower);
                    if (s && s->kind == Kind::Scale && !s->ordinal) {
                        m.after_scale = s;
                        ++j;
                    }
                }
            }
        }
        // A trailing "percent" or "dollars" makes the number a figure, even when it is small.
        if (j < words.size() && joins(text, words[j - 1], words[j])) {
            const Entry* u = lookup(words[j].lower);
            if (u && u->kind == Kind::Percent) {
                m.percent = true;
                ++j;
            } else if (u && u->kind == Kind::Dollars) {
                m.dollars = true;
                ++j;
            }
        }
    }
    m.end = words[j - 1].end;
    m.next = j;
    return true;
}

// Numbers from ten up become digits. Decimals, scales, percent and dollars always do.
bool should_convert(const Match& m) {
    const Number& n = m.number;
    if (n.ordinal) {
        return n.whole() >= 10;
    }
    return !m.frac.empty() || m.after_scale != nullptr || m.percent || m.dollars ||
           n.ends_in_scale || n.whole() >= 10;
}

std::string ordinal_suffix(long long value) {
    const long long last_two = value % 100;
    if (last_two >= 11 && last_two <= 13) {
        return "th";
    }
    switch (value % 10) {
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

std::string render(const Match& m) {
    const Number& n = m.number;
    if (n.ordinal) {
        return std::to_string(n.whole()) + ordinal_suffix(n.whole());
    }

    std::string out;
    if (m.dollars) {
        out += '$';
    }
    if (!m.frac.empty()) {
        out += std::to_string(n.whole()) + "." + m.frac;
        if (m.after_scale) {
            out += ' ';
            out += m.after_scale->word;
        }
    } else if (n.ends_in_scale && !n.multi_scale && (n.scale->value >= 1000000LL)) {
        // "five hundred billion" -> "500 billion", which is how the figures are written.
        out += std::to_string(n.head) + ' ' + std::string(n.scale->word);
    } else {
        out += std::to_string(n.whole());
    }
    if (m.percent) {
        out += '%';
    }
    return out;
}

}  // namespace

std::string normalize_numbers(std::string_view utf8) {
    const std::vector<Word> words = split_words(utf8);
    std::string out;
    out.reserve(utf8.size());
    std::size_t copied = 0;
    std::size_t i = 0;
    while (i < words.size()) {
        Match m;
        if (match_at(utf8, words, i, m) && should_convert(m)) {
            out.append(utf8.substr(copied, m.begin - copied));
            out += render(m);
            copied = m.end;
            i = m.next;
        } else {
            ++i;
        }
    }
    out.append(utf8.substr(copied));
    return out;
}

}  // namespace echoir::text
