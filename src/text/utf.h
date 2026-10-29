#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace echoir::text {

// Text is UTF-8 at the edges and UTF-32 inside this module, so prefix and repeat logic never
// splits a code point. That matters for CJK text, where one character is several UTF-8 bytes.

// Invalid bytes decode to U+FFFD, one per maximal invalid subsequence (Unicode's replacement
// practice). Valid code points are never dropped, and the decoder never throws.
std::u32string utf8_to_utf32(std::string_view utf8);

// Surrogates and values above U+10FFFF encode as U+FFFD.
std::string utf32_to_utf8(std::u32string_view utf32);

// A file system path from UTF-8 text, such as a path argument or a path from the C ABI. A narrow
// std::string would be read in the ANSI code page on Windows, so the bytes are kept as UTF-8.
std::filesystem::path path_from_utf8(std::string_view utf8);

// The path as UTF-8 text, for messages and for the C ABI.
std::string path_utf8(const std::filesystem::path& path);

inline bool is_ascii_digit(char32_t c) { return c >= U'0' && c <= U'9'; }

}  // namespace echoir::text
