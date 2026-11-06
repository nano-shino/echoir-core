// Settings overrides for a VAD, given as "key=value" pairs. They are for development runs, where a
// tuning is tried without a rebuild. Each VAD reads the keys it has and rejects the others.
#pragma once

#include <map>
#include <string>

namespace echoir::vad {

struct VadOverrides {
    // Adds one "key=value". Throws std::invalid_argument if there is no '=' or the key is empty.
    // A key given twice keeps its last value.
    void add(const std::string& assignment);

    // Keys in sorted order, so an error names the same key every time.
    std::map<std::string, std::string> values;

    // Settings that every VAD has, given as numbers by the C API. Each is applied after the keys,
    // in the VAD's own units, and 0 leaves the value from the keys or the default.
    // threshold: the speech probability needed for speech, above 0 and at most 1.
    double threshold = 0.0;
    // min_silence_ms: the shortest pause that ends a speech range, in ms, 0 or more.
    int min_silence_ms = 0;
};

// The value of a setting as a finite real number in decimal, such as 0.25 or 1e-3. Throws
// std::invalid_argument, naming the key, unless the whole text is such a number.
double parse_real(const std::string& key, const std::string& value);

// The value of a setting as a real number from 0 to 1. Throws std::invalid_argument, naming the
// key, unless the whole text is such a number.
double parse_probability(const std::string& key, const std::string& value);

// The value of a setting as a whole number, zero or more. Throws std::invalid_argument, naming the
// key, unless the whole text is such a number.
int parse_count(const std::string& key, const std::string& value);

}  // namespace echoir::vad
