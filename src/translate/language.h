// Language names for the translation prompt.
#pragma once

#include <string>
#include <string_view>

namespace echoir::translate {

// The English name of a language code the translators write ("vi" gives "Vietnamese"), or an empty
// string for a code they do not support. Codes are ISO 639 and match ignoring case, with "-" or "_"
// between parts: "zh-Hant" is Traditional Chinese, "yue" Cantonese.
std::string language_name(std::string_view code);

}  // namespace echoir::translate
