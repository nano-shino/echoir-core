// Base64 decoding for the stdio service's audio. Standard alphabet with padding, as RFC 4648.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace echoir::cli {

// Decodes standard base64. The length must be a multiple of four, padding is allowed only at the
// end, and the unused bits of the last group must be zero, as an RFC 4648 encoder writes them.
// Whitespace and line breaks are not allowed. Throws std::invalid_argument on anything else.
std::vector<std::uint8_t> base64_decode(std::string_view text);

}  // namespace echoir::cli
