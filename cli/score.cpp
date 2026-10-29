#include "score.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#include "text/score.h"
#include "text/utf.h"

namespace echoir::cli {

std::string read_text_input(const std::string& path) {
    if (path == "-") {
        return std::string(std::istreambuf_iterator<char>(std::cin), {});
    }
    const std::filesystem::path file = text::path_from_utf8(path);
    // A directory opens on POSIX systems and reads as empty, so it is refused before the open.
    std::error_code ec;
    if (std::filesystem::is_directory(file, ec)) {
        throw std::runtime_error("cannot read " + path + ": it is a directory");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void run_score(const ScoreOptions& o) {
    if (o.normalize) {
        std::printf("%s\n", text::normalize_for_score(read_text_input(o.hyp)).c_str());
        return;
    }
    const std::string hyp = read_text_input(o.hyp);
    const std::string ref = read_text_input(o.ref);
    const text::ErrorRate result = text::error_rate(hyp, ref);
    // %g with 9 digits prints 0.25 as 0.25 and 1/3 as 0.333333333, and it is valid JSON.
    std::printf("{\"rate\":%.9g,\"subs\":%zu,\"dels\":%zu,\"ins\":%zu,\"ref_len\":%zu}\n",
                result.rate, result.subs, result.dels, result.ins, result.ref_len);
}

}  // namespace echoir::cli
