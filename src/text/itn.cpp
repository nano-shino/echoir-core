#include "text/itn.h"

#include <cstddef>
#include <string>
#include <vector>

namespace echoir::text {
namespace {

// Word classes. Unit is one to nine, Teen ten to nineteen, Tens twenty to ninety, Decade the
// plural tens ("eighties"), used only after a century ("nineteen eighties"). Zero and Oh are digit
// words for decimals and years. A is the article in "a hundred". The rest are the words that can
// sit around a number: "point", "and a half", "percent", "dollars", "cents", "minus".
enum class Kind {
    Zero,
    Oh,
    Unit,
    Teen,
    Tens,
    Decade,
    Hundred,
    Scale,
    Point,
    And,
    A,
    Half,
    Percent,
    Per,
    Cent,
    Percentage,
    Dollar,
    Minus,
};

struct Entry {
    std::string_view word;  // lowercase
    Kind kind;
    long long value;
    bool ordinal;
};

constexpr long long kThousand = 1000LL;
constexpr long long kMillion = 1000000LL;

// The ordinal forms are terminal: they end a number, as in "twenty-first".
constexpr Entry kEntries[] = {
    {"zero", Kind::Zero, 0, false},
    {"oh", Kind::Oh, 0, false},
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
    {"tens", Kind::Decade, 10, false},
    {"twenties", Kind::Decade, 20, false},
    {"thirties", Kind::Decade, 30, false},
    {"forties", Kind::Decade, 40, false},
    {"fifties", Kind::Decade, 50, false},
    {"sixties", Kind::Decade, 60, false},
    {"seventies", Kind::Decade, 70, false},
    {"eighties", Kind::Decade, 80, false},
    {"nineties", Kind::Decade, 90, false},
    {"hundred", Kind::Hundred, 100, false},
    {"thousand", Kind::Scale, kThousand, false},
    {"million", Kind::Scale, kMillion, false},
    {"billion", Kind::Scale, 1000000000LL, false},
    {"trillion", Kind::Scale, 1000000000000LL, false},
    {"point", Kind::Point, 0, false},
    {"and", Kind::And, 0, false},
    {"a", Kind::A, 1, false},
    {"half", Kind::Half, 0, false},
    {"percent", Kind::Percent, 0, false},
    {"per", Kind::Per, 0, false},
    {"cent", Kind::Cent, 0, false},
    {"cents", Kind::Cent, 0, false},
    {"percentage", Kind::Percentage, 0, false},
    {"dollar", Kind::Dollar, 0, false},
    {"dollars", Kind::Dollar, 0, false},
    {"minus", Kind::Minus, 0, false},
    {"negative", Kind::Minus, 0, false},
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
    {"thousandth", Kind::Scale, kThousand, true},
    {"millionth", Kind::Scale, kMillion, true},
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

// A maximal run of ASCII letters, with its table entry (null when it is not a number word).
struct Word {
    std::size_t begin;
    std::size_t end;
    const Entry* entry;
    bool capitalized;  // Title case, "Ten" but not "TEN" or "ten"
};

bool is_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

// Hesitations that a speaker may put inside a number: "two hundred uh forty thousand".
bool is_filler(const std::string& lower) {
    return lower == "uh" || lower == "um" || lower == "uhm" || lower == "umm" || lower == "er" ||
           lower == "erm" || lower == "ah" || lower == "hmm" || lower == "mm";
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Whether a UTF-8 sequence starting with lead byte `c` is a Latin letter, U+00C0 to U+027F, such
// as an accented e.
bool is_latin_lead(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0xC3 && u <= 0xC9;
}

// Whether the ASCII letters from `begin` to `end` are glued to a digit or an accented letter, as
// in "twenty5", or "ten" after an accented e. Then they are part of a longer token, not a number
// word.
bool glued(std::string_view t, std::size_t begin, std::size_t end) {
    if (end < t.size() && (is_digit(t[end]) || is_latin_lead(t[end]))) {
        return true;
    }
    if (begin == 0) {
        return false;
    }
    std::size_t p = begin - 1;
    if (is_digit(t[p])) {
        return true;
    }
    while (p > 0 && (static_cast<unsigned char>(t[p]) & 0xC0) == 0x80) {
        --p;  // back over UTF-8 continuation bytes to the lead byte
    }
    return p + 1 < begin && is_latin_lead(t[p]);
}

// The words of the text, without fillers. A filler stays in the text: it is copied unless it sits
// inside a number.
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
            lower += is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
            ++i;
        }
        const bool stuck = glued(text, begin, i);
        if (!stuck && is_filler(lower)) {
            continue;
        }
        const bool capitalized =
            is_upper(text[begin]) && (i - begin == 1 || !is_upper(text[begin + 1]));
        words.push_back({begin, i, stuck ? nullptr : lookup(lower), capitalized});
    }
    return words;
}

// The length of a space at `p`: ASCII space or tab, or a UTF-8 Unicode space (U+00A0, U+2000 to
// U+200A, U+202F, U+205F, U+3000). 0 if none. Not a line break: a number does not span lines.
std::size_t space_at(std::string_view t, std::size_t p) {
    if (t[p] == ' ' || t[p] == '\t') {
        return 1;
    }
    if (t.compare(p, 2, "\xC2\xA0") == 0) {
        return 2;
    }
    if (p + 3 > t.size()) {
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(t[p]);
    const auto b1 = static_cast<unsigned char>(t[p + 1]);
    const auto b2 = static_cast<unsigned char>(t[p + 2]);
    if (b0 == 0xE2 && b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xAF)) {
        return 3;  // U+2000 to U+200A, U+202F
    }
    if ((b0 == 0xE2 && b1 == 0x81 && b2 == 0x9F) || (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80)) {
        return 3;  // U+205F, U+3000
    }
    return 0;
}

// The length of a hyphen at `p`: ASCII, U+2010 or the no-break U+2011. 0 if none. An en dash is
// handled in Words::joined, since "ten", en dash, "twenty" is a range.
std::size_t hyphen_at(std::string_view t, std::size_t p) {
    if (t[p] == '-') {
        return 1;
    }
    if (t.compare(p, 3, "\xE2\x80\x90") == 0 || t.compare(p, 3, "\xE2\x80\x91") == 0) {
        return 3;
    }
    return 0;
}

// The words of one text, and whether word i is joined to word i - 1. Two words belong to one
// number when only spaces, at most one hyphen and fillers separate them: "four-point-five",
// "twenty  five" and "two hundred uh forty" join; "five, six" does not. An en dash joins only a
// tens word and a unit: "twenty", en dash, "five" is 25, and "ten", en dash, "twenty" a range.
class Words {
public:
    explicit Words(std::string_view text) : text_(text), words_(split_words(text)) {}

    std::size_t size() const { return words_.size(); }
    const Word& operator[](std::size_t i) const { return words_[i]; }

    bool joined(std::size_t i) const {
        if (i == 0 || i >= words_.size()) {
            return false;
        }
        std::size_t p = words_[i - 1].end;
        const std::size_t end = words_[i].begin;
        int hyphens = 0;
        bool dash = false;       // an en dash (U+2013) was seen
        bool separated = false;  // a space or hyphen was seen
        while (p < end) {
            if (const std::size_t s = space_at(text_, p)) {
                p += s;
                separated = true;
            } else if (const std::size_t h = hyphen_at(text_, p)) {
                p += h;
                separated = true;
                if (++hyphens > 1) {
                    return false;
                }
            } else if (text_.compare(p, 3, "\xE2\x80\x93") == 0) {
                p += 3;
                separated = true;
                dash = true;
                if (++hyphens > 1) {
                    return false;
                }
            } else if (is_ascii_letter(text_[p])) {
                // A filler: split_words dropped it, so any letters here are one.
                while (p < end && is_ascii_letter(text_[p])) {
                    ++p;
                }
            } else {
                return false;
            }
        }
        if (dash) {
            // A tens word and a unit across a dash is one number: "twenty", dash, "five" is never
            // a range, while "ten", dash, "twenty" is.
            const Entry* a = words_[i - 1].entry;
            const Entry* b = words_[i].entry;
            return a && b && a->kind == Kind::Tens && !a->ordinal && b->kind == Kind::Unit;
        }
        return separated;
    }

    // The entry of word i when it is joined to word i - 1, else null.
    const Entry* next(std::size_t i) const { return joined(i) ? words_[i].entry : nullptr; }

    bool is(std::size_t i, Kind kind) const {
        const Entry* e = next(i);
        return e && e->kind == kind && !e->ordinal;
    }

    std::string_view text() const { return text_; }

    // Whether word i is `lower`, ignoring case.
    bool spells(std::size_t i, std::string_view lower) const {
        if (i >= words_.size() || words_[i].end - words_[i].begin != lower.size()) {
            return false;
        }
        for (std::size_t k = 0; k < lower.size(); ++k) {
            char c = text_[words_[i].begin + k];
            c = is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
            if (c != lower[k]) {
                return false;
            }
        }
        return true;
    }

private:
    std::string_view text_;
    std::vector<Word> words_;
};

bool is_count_kind(Kind k) { return k == Kind::Unit || k == Kind::Teen || k == Kind::Tens; }

bool is_number_word_kind(Kind k) {
    return k == Kind::Zero || is_count_kind(k) || k == Kind::Hundred || k == Kind::Scale;
}

// A cardinal or ordinal integer, built one word at a time. A word is added only where the spoken
// order allows it, so "five six" is two numbers and "twenty five" is one.
struct Integer {
    long long total = 0;         // the groups closed by a scale word ("two thousand" -> 2000)
    long long group = 0;         // the part below the last scale word
    long long top_scale = 0;     // the first (largest) scale word, 0 if none
    long long last_scale = 0;    // the last scale word added
    bool group_hundred = false;  // the group already has a hundred ("two hundred")
    bool ends_in_scale = false;  // the last word added was a scale word
    bool ordinal = false;
    std::size_t words = 0;  // the number of number words in it (not "and")

    long long value() const { return total + group; }
};

// Adds a number word. Returns false, and leaves n unchanged, when the word cannot follow.
bool add(Integer& n, const Entry& e) {
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
            // "five hundred", and the US "twelve hundred" and "twenty five hundred".
            if (n.group < 1 || n.group > 99 || n.group_hundred) {
                return false;
            }
            n.group *= 100;
            n.group_hundred = true;
            break;
        case Kind::Scale:
            // Scale words only shrink: "two million five thousand", never "thousand million".
            if (n.group == 0 || (n.last_scale != 0 && e.value >= n.last_scale)) {
                return false;
            }
            n.total += n.group * e.value;
            n.group = 0;
            n.group_hundred = false;
            if (n.top_scale == 0) {
                n.top_scale = e.value;
            }
            n.last_scale = e.value;
            break;
        default:
            return false;
    }
    n.ends_in_scale = e.kind == Kind::Scale;
    n.ordinal = e.ordinal;
    ++n.words;
    return true;
}

// Parses an integer starting at word `i`. It returns the index after it, or `i` if none starts
// there. A number starts with a unit, teen or ten word, or with "a" before "hundred", "thousand",
// "million" and so on: "a hundred and twenty", or with "hundred and": "the hundred and fiftieth".
// "hundred" or "billion" alone stays a word.
std::size_t parse_integer(const Words& w, std::size_t i, Integer& n, bool& article) {
    n = Integer{};
    article = false;
    const Entry* first = w[i].entry;
    if (!first) {
        return i;
    }
    std::size_t j = i;
    if (first->kind == Kind::A) {
        const Entry* e = w.next(i + 1);
        if (!e || (e->kind != Kind::Hundred && e->kind != Kind::Scale)) {
            return i;
        }
        n.group = 1;
        article = true;
        j = i + 1;
    } else if (first->kind == Kind::Hundred && !first->ordinal) {
        const Entry* after = w.next(i + 2);
        if (!w.is(i + 1, Kind::And) || !after || !is_count_kind(after->kind)) {
            return i;
        }
        n.group = 1;
    } else if (!is_count_kind(first->kind)) {
        return i;
    }

    // "and" may follow "hundred" or a scale word, once in each group: "two hundred and forty
    // thousand three hundred and six", "one thousand and five". If what comes after it does not
    // continue the number, the number ends before it: "two hundred and five hundred" is 200 and
    // 500.
    std::size_t and_at = 0;     // the index of the "and" in this group, 0 if none
    Integer before_and;         // the number before that "and"
    bool last_was_big = false;  // the last word was "hundred" or a scale word
    while (j < w.size()) {
        if (j > i && !w.joined(j)) {
            break;
        }
        const Entry* e = w[j].entry;
        if (!e) {
            break;
        }
        if (e->kind == Kind::And) {
            const Entry* after = w.next(j + 1);
            if (!last_was_big || !after || !is_count_kind(after->kind)) {
                break;
            }
            before_and = n;
            and_at = j;
            ++j;
            continue;
        }
        Integer next = n;
        if (!add(next, *e)) {
            // "a hundred thousand and five hundred thousand": the part after "and" wanted to
            // grow into a number of its own, so it is one.
            if (and_at != 0 && (e->kind == Kind::Hundred || e->kind == Kind::Scale)) {
                n = before_and;
                j = and_at;
            }
            break;
        }
        n = next;
        last_was_big = e->kind == Kind::Hundred || e->kind == Kind::Scale;
        if (e->kind == Kind::Scale) {
            and_at = 0;  // the group closed, so its "and" is final
        }
        ++j;
        if (e->ordinal) {
            break;
        }
    }
    // "a" alone, or "a" before a word that did not join, is not a number.
    if (article && n.words == 0) {
        return i;
    }
    return j;
}

// A two-digit number word sequence at word `j`, 10 to 99: "ninety nine", "fifteen", "forty".
// Returns the index after it, or `j` if none.
std::size_t parse_two_digits(const Words& w, std::size_t j, long long& value, bool& ordinal) {
    const Entry* e = w.next(j);
    if (!e || (e->kind != Kind::Teen && e->kind != Kind::Tens)) {
        return j;
    }
    value = e->value;
    ordinal = e->ordinal;
    if (!e->ordinal && e->kind == Kind::Tens) {
        const Entry* u = w.next(j + 1);
        if (u && u->kind == Kind::Unit) {
            value += u->value;
            ordinal = u->ordinal;
            return j + 2;
        }
    }
    return j + 1;
}

struct Match {
    std::size_t begin = 0;  // the word index where the match starts
    std::size_t next = 0;   // the word index after the match
    Integer number;
    bool article = false;  // it starts with "a": "a hundred and twenty"
    bool year = false;     // "nineteen ninety nine"
    std::string time;      // "nine forty five a.m." -> "9:45", empty if not a time
    bool decade = false;   // "nineteen eighties" -> "1980s"
    long long year_value = 0;
    std::string frac;           // digits after the point, empty if none
    long long after_scale = 0;  // "six point three billion": the scale after the decimal
    const Entry* after_scale_word = nullptr;
    bool percent = false;
    bool percentage = false;  // "percentage points": a figure, and the words stay
    bool dollars = false;
    bool dollar_singular = false;  // "a hundred dollar bill": the article stays
    std::string price;             // "nineteen ninety nine dollars" -> "19.99", empty if not
    bool cents = false;
    std::string cents_digits;  // "five dollars and fifty cents" -> "50"
    bool negative = false;
};

// Digit words after "point": "four point five", "three point one four", "point oh five", and the
// spoken "three point fourteen". Returns the index after them, or `j` if none.
std::size_t parse_fraction(const Words& w, std::size_t j, std::string& frac) {
    long long two = 0;
    bool ordinal = false;
    const std::size_t k = parse_two_digits(w, j, two, ordinal);
    if (k != j && !ordinal) {
        frac = std::to_string(two);
        return k;
    }
    std::size_t m = j;
    while (true) {
        const Entry* d = w.next(m);
        if (!d || d->ordinal ||
            (d->kind != Kind::Zero && d->kind != Kind::Oh && d->kind != Kind::Unit)) {
            break;
        }
        frac += static_cast<char>('0' + d->value);
        ++m;
    }
    return m;
}

// A year spoken in two halves: "nineteen ninety nine", "twenty twenty four", "nineteen oh five",
// "nineteen eighties". The first half is a single word from thirteen to twenty, so the times
// "eleven thirty" and "twelve fifteen" are not years.
bool parse_year(const Words& w, std::size_t i, Match& m) {
    const Entry* century = w[i].entry;
    if (!century || century->ordinal || century->value < 13 || century->value > 20 ||
        (century->kind != Kind::Teen && century->kind != Kind::Tens)) {
        return false;
    }
    const std::size_t j = i + 1;
    const Entry* e = w.next(j);
    if (!e) {
        return false;
    }
    if (e->kind == Kind::Oh) {
        const Entry* u = w.next(j + 1);
        if (!u || u->kind != Kind::Unit || u->ordinal) {
            return false;
        }
        m.year_value = century->value * 100 + u->value;
        m.next = j + 2;
    } else if (e->kind == Kind::Decade) {
        m.year_value = century->value * 100 + e->value;
        m.decade = true;
        m.next = j + 1;
    } else {
        long long two = 0;
        bool ordinal = false;
        const std::size_t k = parse_two_digits(w, j, two, ordinal);
        if (k == j || ordinal) {
            return false;
        }
        // "twenty twenty million" is not a year.
        const Entry* after = w.next(k);
        if (after && (after->kind == Kind::Hundred || after->kind == Kind::Scale)) {
            return false;
        }
        m.year_value = century->value * 100 + two;
        m.next = k;
        // Before "dollars" the two halves are a price: "nineteen ninety nine dollars" -> "$19.99".
        if (w.is(k, Kind::Dollar)) {
            m.price = std::to_string(century->value) + "." + std::to_string(two);
            m.next = k + 1;
        }
    }
    m.year = true;
    m.begin = i;
    return true;
}

// Whether "a.m.", "am", "p.m." or "pm" follows offset `p`, after spaces.
bool meridiem_at(std::string_view t, std::size_t p) {
    while (p < t.size() && space_at(t, p) != 0) {
        p += space_at(t, p);
    }
    if (p >= t.size()) {
        return false;
    }
    const char c = t[p];
    if (c != 'a' && c != 'A' && c != 'p' && c != 'P') {
        return false;
    }
    std::size_t q = p + 1;
    if (q < t.size() && t[q] == '.') {
        ++q;
    }
    if (q >= t.size() || (t[q] != 'm' && t[q] != 'M')) {
        return false;
    }
    ++q;
    return q >= t.size() || !is_ascii_letter(t[q]);
}

// A clock time: "nine forty five a.m." -> "9:45", "at ten thirty" -> "at 10:30", "seven oh five
// p.m." -> "7:05", "nine a.m." -> "9". Without "a.m." or "p.m." after it, or "at" before it, two
// numbers side by side are left alone, since "twelve fifteen-year-olds" is not a time.
bool parse_time(const Words& w, std::size_t i, Match& m) {
    const Entry* hour = w[i].entry;
    if (!hour || hour->ordinal || (hour->kind != Kind::Unit && hour->kind != Kind::Teen) ||
        hour->value > 12) {
        return false;
    }
    std::size_t j = i + 1;
    long long minutes = -1;
    if (w.is(j, Kind::Oh)) {
        const Entry* u = w.next(j + 1);
        if (u && u->kind == Kind::Unit && !u->ordinal) {
            minutes = u->value;
            j += 2;
        }
    } else {
        long long two = 0;
        bool ordinal = false;
        const std::size_t k = parse_two_digits(w, j, two, ordinal);
        if (k != j && !ordinal && two < 60) {
            minutes = two;
            j = k;
        }
    }
    const bool meridiem = meridiem_at(w.text(), w[j - 1].end);
    const bool at = i > 0 && w.spells(i - 1, "at");
    if (minutes < 0) {
        if (!meridiem) {
            return false;
        }
        m.time = std::to_string(hour->value);
    } else {
        // "at" alone is not enough when another number word follows: "at twelve fifteen
        // hundred".
        const Entry* after = w.next(j);
        if (!meridiem && (!at || (after && is_number_word_kind(after->kind)))) {
            return false;
        }
        m.time =
            std::to_string(hour->value) + ":" + (minutes < 10 ? "0" : "") + std::to_string(minutes);
    }
    m.begin = i;
    m.next = j;
    return true;
}

// Matches a number that starts at word `i`. It returns false when no number starts there.
bool match_at(const Words& w, std::size_t i, Match& m) {
    m = Match{};
    m.begin = i;
    const Entry* first = w[i].entry;
    if (!first) {
        return false;
    }

    if (parse_time(w, i, m)) {
        return true;
    }

    std::size_t j = i;
    if (first->kind == Kind::Point) {
        // "point five percent" -> "0.5%". A bare "point" is a decimal only before "percent".
        std::string frac;
        const std::size_t k = parse_fraction(w, i + 1, frac);
        if (frac.empty() || !w.is(k, Kind::Percent)) {
            return false;
        }
        m.frac = frac;
        m.percent = true;
        m.next = k + 1;
        return true;
    }
    if (first->kind == Kind::Zero) {
        // "zero point five", "zero percent": zero starts a number only as a decimal or a figure.
        if (!w.is(i + 1, Kind::Point) && !w.is(i + 1, Kind::Percent)) {
            return false;
        }
        j = i + 1;
        m.number.words = 1;
    } else {
        j = parse_integer(w, i, m.number, m.article);
        if (j == i) {
            return false;
        }
        // A single century word followed by two digits is a year, not two numbers.
        if (m.number.words == 1 && !m.article && parse_year(w, i, m)) {
            return true;
        }
    }

    const Integer& n = m.number;
    if (n.ordinal) {
        m.next = j;
        return true;
    }

    // A decimal: "four point five". It needs a plain whole part, not "two billion point".
    if (n.top_scale == 0 && !m.article && w.is(j, Kind::Point)) {
        std::string frac;
        const std::size_t k = parse_fraction(w, j + 1, frac);
        if (!frac.empty()) {
            m.frac = frac;
            j = k;
        }
    }
    // "two and a half percent", "one and a half million", "twelve and a half". Below ten it needs a
    // scale, percent or dollars after it: "two and a half years" stays words.
    if (n.top_scale == 0 && !m.article && m.frac.empty() && w.is(j, Kind::And) &&
        w.is(j + 1, Kind::A) && w.is(j + 2, Kind::Half)) {
        const Entry* after = w.next(j + 3);
        const bool figure = after && !after->ordinal &&
                            (after->kind == Kind::Scale || after->kind == Kind::Percent ||
                             after->kind == Kind::Per || after->kind == Kind::Dollar);
        if (figure || n.value() >= 10) {
            m.frac = "5";
            j += 3;
        }
    }
    // A scale after a decimal: "six point three billion".
    if (!m.frac.empty()) {
        const Entry* s = w.next(j);
        if (s && s->kind == Kind::Scale && !s->ordinal) {
            m.after_scale = s->value;
            m.after_scale_word = s;
            ++j;
        }
    }

    // A trailing unit makes the number a figure, even when it is small.
    if (w.is(j, Kind::Percent)) {
        m.percent = true;
        ++j;
    } else if (w.is(j, Kind::Per) && w.is(j + 1, Kind::Cent) &&
               w[j + 1].end - w[j + 1].begin == 4) {
        m.percent = true;  // "per cent"
        j += 2;
    } else if (w.is(j, Kind::Percentage)) {
        m.percentage = true;  // "two percentage points": the words stay
    } else if (w.is(j, Kind::Dollar) && !(n.value() == 1 && m.frac.empty() && m.article)) {
        m.dollars = true;
        m.dollar_singular = w[j].end - w[j].begin == 6;
        ++j;
        // "five dollars and fifty cents" -> "$5.50".
        if (m.frac.empty() && m.after_scale == 0 && n.top_scale == 0 && w.is(j, Kind::And)) {
            Integer c;
            bool article = false;
            const std::size_t k = parse_integer(w, j + 1, c, article);
            if (k != j + 1 && !article && !c.ordinal && c.value() < 100 && w.is(k, Kind::Cent)) {
                m.cents_digits = (c.value() < 10 ? "0" : "") + std::to_string(c.value());
                j = k + 1;
            }
        }
    } else if (w.is(j, Kind::Cent) && m.frac.empty()) {
        m.cents = true;  // "five cents" -> "5 cents": the word stays
    }
    m.next = j;
    return true;
}

// Numbers from ten up become digits. Decimals, scales, years, percent and money always do. "a
// hundred" or "a million" alone stays as words, as prose writes it.
bool should_convert(const Match& m) {
    const Integer& n = m.number;
    if (m.year || !m.time.empty() || m.negative) {
        return true;  // a sign makes a figure: "minus five degrees" -> "-5 degrees"
    }
    // "a hundred people", "a million", and the fraction "a hundredth".
    if (m.article && m.frac.empty() && !m.percent && !m.dollars && m.next - m.begin == 2) {
        return false;
    }
    if (n.ordinal) {
        return n.value() >= 10;
    }
    return !m.frac.empty() || m.after_scale != 0 || m.percent || m.percentage || m.dollars ||
           m.cents || n.top_scale != 0 || n.value() >= 10;
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

std::string_view scale_word(long long scale) {
    for (const Entry& e : kEntries) {
        if (e.kind == Kind::Scale && !e.ordinal && e.value == scale) {
            return e.word;
        }
    }
    return {};
}

// `value / scale` as a short decimal, "1.25", or empty when it needs more than three places.
std::string scaled(long long value, long long scale) {
    const long long whole = value / scale;
    long long rest = value % scale;
    if (rest == 0) {
        return std::to_string(whole);
    }
    if (rest % (scale / 1000) != 0) {
        return {};
    }
    rest /= scale / 1000;
    std::string digits = std::to_string(rest);
    digits.insert(0, 3 - digits.size(), '0');
    while (digits.back() == '0') {
        digits.pop_back();
    }
    return std::to_string(whole) + "." + digits;
}

std::string render(const Match& m) {
    const Integer& n = m.number;
    if (!m.time.empty()) {
        return m.time;
    }
    if (!m.price.empty()) {
        return "$" + m.price;
    }
    if (m.year) {
        return std::to_string(m.year_value) + (m.decade ? "s" : "");
    }
    if (n.ordinal) {
        return std::to_string(n.value()) + ordinal_suffix(n.value());
    }

    std::string figure;
    if (!m.frac.empty()) {
        figure = std::to_string(n.value()) + "." + m.frac;
        if (m.after_scale == kThousand && m.frac.size() <= 3) {
            // "two point five thousand" -> "2500".
            std::string digits = m.frac;
            digits.append(3 - digits.size(), '0');
            figure = std::to_string(n.value() * kThousand + std::stoll(digits));
        } else if (m.after_scale != 0) {
            figure += ' ';
            figure += m.after_scale_word->word;
        }
    } else if (n.top_scale >= kMillion && !scaled(n.value(), n.top_scale).empty()) {
        // "five hundred billion" -> "500 billion", "one million two hundred thousand" ->
        // "1.2 million", which is how the figures are written.
        figure = scaled(n.value(), n.top_scale) + ' ' + std::string(scale_word(n.top_scale));
    } else {
        figure = std::to_string(n.value());
    }

    std::string out;
    if (m.negative) {
        out += '-';
    }
    if (m.dollars) {
        out += '$';
        if (!m.cents_digits.empty()) {
            figure += "." + m.cents_digits;
        }
    }
    out += figure;
    if (m.percent) {
        out += '%';
    }
    return out;
}

bool is_number_word(const Entry* e) { return e && is_number_word_kind(e->kind); }

// Whether word i starts a sentence: nothing but spaces, quotes and brackets since the start of the
// text or a ".", "!", "?" or ":".
bool starts_sentence(const Words& w, std::size_t i) {
    const std::string_view t = w.text();
    std::size_t p = w[i].begin;
    while (p > 0) {
        const char c = t[p - 1];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == '[' ||
            (static_cast<unsigned char>(c) & 0x80) != 0) {
            --p;  // a space, an opening quote or bracket, or a UTF-8 quote or space
            continue;
        }
        return c == '.' || c == '!' || c == '?' || c == ':' || c == '\n';
    }
    return true;
}

}  // namespace

std::string normalize_numbers(std::string_view utf8) {
    const Words w(utf8);
    std::string out;
    out.reserve(utf8.size());
    std::size_t copied = 0;
    std::size_t i = 0;
    while (i < w.size()) {
        Match m;
        if (!match_at(w, i, m)) {
            ++i;
            continue;
        }
        // "minus five", "negative two percent". Not "five minus three", where it is an operator.
        // "minus" makes a figure of any number; "negative" only of one that is a figure anyway,
        // since "negative one" can be prose.
        const bool signed_number =
            !m.number.ordinal && !m.year && m.time.empty() && i > 0 && w.joined(i) &&
            w[i - 1].entry && w[i - 1].entry->kind == Kind::Minus && w[i - 1].begin >= copied &&
            !(i > 1 && w.joined(i - 1) && is_number_word(w[i - 2].entry));
        if (signed_number && w.spells(i - 1, "minus")) {
            m.negative = true;
        }
        if (!should_convert(m)) {
            ++i;
            continue;
        }
        m.negative = signed_number;
        // A capitalized number word inside a sentence is a name: "the Big Ten", "Ocean's
        // Eleven", "Matchbox Twenty".
        if (w[i].capitalized && !starts_sentence(w, i)) {
            i = m.next;
            continue;
        }
        // An ordinal after a count or "a" is a fraction: "one twentieth", "a tenth". So is "one
        // millionth of a second", though "one hundredth" alone is the 100th.
        if (m.number.ordinal) {
            const Entry* prev = i > 0 && w.joined(i) ? w[i - 1].entry : nullptr;
            const bool after_count = is_number_word(prev) || (prev && prev->kind == Kind::A);
            // "one" and an ordinal parse as one number only for "hundredth" and the scales.
            const bool one_scale_of =
                m.number.words == 2 && w.spells(i, "one") && w.spells(m.next, "of");
            if (after_count || one_scale_of) {
                i = m.next;
                continue;
            }
        }
        std::size_t begin = w[m.begin].begin;
        if (m.negative) {
            begin = w[i - 1].begin;
        }
        // "a hundred dollar bill": the article belongs to the bill, so it stays.
        if (m.article && m.dollar_singular) {
            begin = w[m.begin + 1].begin;
        }
        out.append(utf8.substr(copied, begin - copied));
        out += render(m);
        copied = w[m.next - 1].end;
        i = m.next;
    }
    out.append(utf8.substr(copied));
    return out;
}

}  // namespace echoir::text
