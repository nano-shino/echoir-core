#pragma once

#include <string>
#include <string_view>

namespace echoir::text {

// Inverse text normalization for spoken numbers, as written in news and prose. Qwen3-ASR writes
// numbers as words, so this turns them into digits:
//   - numbers from ten up, and ordinals from tenth up: "thirty nine" -> "39", "thirtieth" -> "30th"
//   - decimals: "four point five" -> "4.5"
//   - percent and dollars: "thirty nine percent" -> "39%", "six point three billion dollars" ->
//     "$6.3 billion"
// Smaller numbers stay words ("two months", "the second drop"), as in prose. Text that is not a
// number is copied unchanged, including the spacing and punctuation around each number.
std::string normalize_numbers(std::string_view utf8);

}  // namespace echoir::text
