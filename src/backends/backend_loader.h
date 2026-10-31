// ggml's backends, when they are separate modules. A GGML_BACKEND_DL build (Android) ships the
// CPU backend in several variants, one per set of ARM features, and loads the best one the CPU
// runs.
#pragma once

namespace echoir::backends {

// Installs the backend log callbacks, then, in a GGML_BACKEND_DL build, loads the backend modules
// from the folder of the library that holds this code (an app's native library folder, or the
// CLI's bin/). Runs once; later calls do nothing. Call it before any ggml device or backend use.
void load_backends();

}  // namespace echoir::backends
