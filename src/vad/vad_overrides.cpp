#include "vad/vad_overrides.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <system_error>

namespace echoir::vad {

void VadOverrides::add(const std::string& assignment) {
    const std::size_t eq = assignment.find('=');
    if (eq == std::string::npos || eq == 0) {
        throw std::invalid_argument("VAD setting must be key=value, got '" + assignment + "'");
    }
    values[assignment.substr(0, eq)] = assignment.substr(eq + 1);
}

double parse_real(const std::string& key, const std::string& value) {
    // strtod also takes leading spaces and hexadecimal, which a setting should not.
    const bool plain =
        !value.empty() && value.find_first_not_of("0123456789+-.eE") == std::string::npos;
    char* end = nullptr;
    const double number = plain ? std::strtod(value.c_str(), &end) : 0.0;
    if (!plain || end != value.c_str() + value.size() || !std::isfinite(number)) {
        throw std::invalid_argument("VAD setting " + key + " needs a number, got '" + value + "'");
    }
    return number;
}

double parse_probability(const std::string& key, const std::string& value) {
    const double number = parse_real(key, value);
    if (number < 0.0 || number > 1.0) {
        throw std::invalid_argument("VAD setting " + key + " must be between 0 and 1, got '" +
                                    value + "'");
    }
    return number;
}

int parse_count(const std::string& key, const std::string& value) {
    int count = 0;
    const char* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), last, count);
    if (value.empty() || ec != std::errc{} || ptr != last || count < 0) {
        throw std::invalid_argument("VAD setting " + key +
                                    " needs a whole number, zero or more, got '" + value + "'");
    }
    return count;
}

}  // namespace echoir::vad
