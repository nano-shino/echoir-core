// Link check for the backend libraries. Calling their entry points makes this binary need
// libwhisper, and libllama and libmtmd when Qwen is built, all on one shared ggml. A bad link or a
// second copy of ggml shows up here, before any decode runs.
#include <cstring>

#include "doctest.h"
#include "whisper.h"

#ifdef ECHOIR_HAS_QWEN
#include "llama.h"
#include "mtmd.h"
#endif

TEST_CASE("backend libraries link and report their build info") {
    CHECK(std::strlen(whisper_print_system_info()) > 0);
#ifdef ECHOIR_HAS_QWEN
    CHECK(std::strlen(llama_print_system_info()) > 0);
    CHECK(std::strlen(mtmd_default_marker()) > 0);
#endif
}
