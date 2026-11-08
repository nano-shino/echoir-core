#include "translate/language.h"

#include <cctype>
#include <string>
#include <string_view>

namespace echoir::translate {
namespace {

struct Language {
    const char* code;  // lower case, "-" between parts
    const char* name;
};

// The languages Hy-MT2 translates among, with the names its prompts use.
constexpr Language kLanguages[] = {
    {"zh", "Chinese"},    {"en", "English"},
    {"fr", "French"},     {"pt", "Portuguese"},
    {"es", "Spanish"},    {"ja", "Japanese"},
    {"tr", "Turkish"},    {"ru", "Russian"},
    {"ar", "Arabic"},     {"ko", "Korean"},
    {"th", "Thai"},       {"it", "Italian"},
    {"de", "German"},     {"vi", "Vietnamese"},
    {"ms", "Malay"},      {"id", "Indonesian"},
    {"tl", "Filipino"},   {"hi", "Hindi"},
    {"pl", "Polish"},     {"cs", "Czech"},
    {"nl", "Dutch"},      {"km", "Khmer"},
    {"my", "Burmese"},    {"fa", "Persian"},
    {"gu", "Gujarati"},   {"ur", "Urdu"},
    {"te", "Telugu"},     {"mr", "Marathi"},
    {"he", "Hebrew"},     {"bn", "Bengali"},
    {"ta", "Tamil"},      {"uk", "Ukrainian"},
    {"bo", "Tibetan"},    {"kk", "Kazakh"},
    {"mn", "Mongolian"},  {"ug", "Uyghur"},
    {"yue", "Cantonese"}, {"zh-hant", "Traditional Chinese"},
};

}  // namespace

std::string language_name(std::string_view code) {
    std::string key;
    for (const char c : code) {
        key += c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const Language& language : kLanguages) {
        if (key == language.code) {
            return language.name;
        }
    }
    return {};
}

}  // namespace echoir::translate
