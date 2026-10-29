#include "backends/backend_log.h"

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

#include "text/env.h"
#include "whisper.h"

#ifdef ECHOIR_HAS_QWEN
#include "llama.h"
#include "mtmd-helper.h"
#endif

namespace echoir::backends {
namespace {

// Written once, inside configure_backend_logs(), before the callback is installed.
bool g_debug_logs = false;

// The problem flag of the innermost LoadWatch on this thread.
thread_local bool* g_problem = nullptr;

// One callback for whisper, ggml and llama alike. ggml keeps a single callback, so they all share
// it. Lines are dropped unless ECHOIR_LOG=debug.
void route_log(ggml_log_level level, const char* text, void* /*user_data*/) {
    if (g_problem != nullptr && (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR)) {
        *g_problem = true;
    }
    if (g_debug_logs) {
        std::fputs(text, stderr);
        std::fflush(stderr);
    }
}

}  // namespace

void configure_backend_logs() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::optional<std::string> value = text::get_env("ECHOIR_LOG");
        g_debug_logs = value && *value == "debug";
        // whisper_log_set also sets ggml's callback.
        whisper_log_set(route_log, nullptr);
#ifdef ECHOIR_HAS_QWEN
        llama_log_set(route_log, nullptr);
        mtmd_helper_log_set(route_log, nullptr);
#endif
    });
}

LoadWatch::LoadWatch() : previous_(g_problem) { g_problem = &problem_; }

LoadWatch::~LoadWatch() { g_problem = previous_; }

}  // namespace echoir::backends
