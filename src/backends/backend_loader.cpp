#include "backends/backend_loader.h"

#include <mutex>

#include "backends/backend_log.h"
#include "errors.h"
#include "ggml-backend.h"

#ifdef GGML_BACKEND_DL
#include <dlfcn.h>

#include <string>
#endif

namespace echoir::backends {

#ifdef GGML_BACKEND_DL
namespace {

// The folder of the library this code is linked into, or empty when dladdr cannot tell.
std::string library_folder() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&load_backends), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    const std::string path = info.dli_fname;
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace
#endif

void load_backends() {
    configure_backend_logs();
#ifdef GGML_BACKEND_DL
    // ggml looks next to the executable and in the current folder, which in an app are
    // app_process and /. The modules sit next to the library this code is linked into.
    static const std::string folder = library_folder();
    static std::once_flag once;
    std::call_once(
        once, [] { ggml_backend_load_all_from_path(folder.empty() ? nullptr : folder.c_str()); });
    // Without the CPU module, each model fails later with a message that does not say where
    // it looked, so the missing module is reported here.
    if (ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU) == nullptr) {
        throw BackendUnavailable(
            "the ggml CPU backend module (libggml-cpu-*.so) was not found in " +
            (folder.empty() ? std::string("the default module path") : folder));
    }
#endif
}

}  // namespace echoir::backends
