// `echoir score`: scores a transcript against a reference text, or prints the normalized form of
// a text. The end-to-end runner scores only through this command, so both sides of every
// comparison use the same normalization and the same error rate.
#pragma once

#include <string>

namespace echoir::cli {

struct ScoreOptions {
    std::string hyp;         // transcript file, or "-" for standard input
    std::string ref;         // reference file, or "-"; not used with normalize
    bool normalize = false;  // print the normalized text of hyp instead of scoring
};

// Reads a whole text file as UTF-8, or standard input for "-". Throws std::runtime_error when the
// file cannot be read.
std::string read_text_input(const std::string& path);

// Scores the transcript and prints one JSON line:
// {"rate":…,"subs":…,"dels":…,"ins":…,"ref_len":…}.
// With normalize, prints the normalized text of hyp and a newline. Throws std::runtime_error when
// a file cannot be read.
void run_score(const ScoreOptions& options);

}  // namespace echoir::cli
