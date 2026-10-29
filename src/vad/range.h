// A speech range found by a VAD.
#pragma once

#include <cstdint>

namespace echoir::vad {

// A speech range in ms from the start of the audio given to the VAD. Both ends lie in
// [0, audio duration] and end_ms > start_ms.
struct Range {
    std::int64_t start_ms;
    std::int64_t end_ms;
};

}  // namespace echoir::vad
