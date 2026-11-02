#pragma once

#include <string>
#include <string_view>

namespace echoir::text {

// Inverse text normalization for spoken numbers, as written in news and prose. Qwen3-ASR writes
// numbers as words, so this turns them into digits:
//   - numbers from ten up, and ordinals from tenth up: "thirty nine" -> "39", "two hundred and
//     forty thousand" -> "240000", "twelve hundred" -> "1200", "one hundred and first" -> "101st"
//   - large figures by their top scale: "one point two billion" and "one billion two hundred
//     million" -> "1.2 billion"
//   - decimals and halves: "four point five" -> "4.5", "one and a half million" -> "1.5 million"
//   - percent, dollars and cents: "thirty nine percent" -> "39%", "five dollars and fifty cents"
//     -> "$5.50", "minus two percent" -> "-2%"
//   - years and decades: "nineteen ninety nine" -> "1999", "the nineteen eighties" -> "the 1980s"
//   - clock times next to "a.m.", "p.m." or after "at": "nine forty five a.m." -> "9:45 a.m."
// Smaller numbers stay words ("two months", "the second drop", "a hundred people"), as in prose,
// and so do fractions ("one twentieth") and capitalized names inside a sentence ("the Big Ten").
// Speech is loose, so the words of a number may be separated by any spaces, one hyphen, or fillers
// ("two hundred uh forty"); a comma ends a number. Text that is not a number is copied unchanged,
// including the spacing and punctuation around each number.
std::string normalize_numbers(std::string_view utf8);

}  // namespace echoir::text
