// Process-wide setup of llama.cpp, shared by every backend that loads a llama.cpp model.
#pragma once

namespace echoir::backends {

// Runs llama_backend_init once for the process, however many threads and backends ask at the same
// time. Every caller returns after it has finished. Call it before the first model load.
void init_llama_backend();

}  // namespace echoir::backends
