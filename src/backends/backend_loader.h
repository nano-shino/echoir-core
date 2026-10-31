// ggml's backends, when they are separate modules. A GGML_BACKEND_DL build (Android) ships the
// CPU backend in several variants, one per set of ARM features, and loads the best one the CPU
// runs.
#pragma once

namespace echoir::backends {

// Installs the backend log callbacks, then, in a GGML_BACKEND_DL build, loads the backend modules
// from the folder of the library that holds this code (an app's native library folder, or the
// CLI's bin/). The modules load once; later calls do nothing else. Call it before any ggml device
// or backend use. In a GGML_BACKEND_DL build it throws BackendUnavailable when no CPU module
// was loaded, since no model can run without one.
void load_backends();

}  // namespace echoir::backends
