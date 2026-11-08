#pragma once

#include <string>
#include <string_view>

namespace echoir::translate {

// The text of a finished generation as a clean UTF-8 string: bytes that are not valid UTF-8 are
// dropped, which includes a character the generation limit cut in half, and leading and trailing
// white space is trimmed. The model works in bytes, and a looping or capped generation can end
// inside a character or hold stray bytes, which JSON writers reject.
std::string clean_output(std::string_view raw);

}  // namespace echoir::translate
