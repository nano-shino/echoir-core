#include "backends/llama_init.h"

#include <mutex>

#include "llama.h"

namespace echoir::backends {

void init_llama_backend() {
    static std::once_flag once;
    std::call_once(once, [] { llama_backend_init(); });
}

}  // namespace echoir::backends
