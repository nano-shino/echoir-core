#include "backends/backend_loader.h"

#include <mutex>

#include "backends/backend_log.h"
#include "ggml-backend.h"

#ifdef GGML_BACKEND_DL
#include <dlfcn.h>

#include <string>
#endif

namespace echoir::backends {

void load_backends() {
    configure_backend_logs();
#ifdef GGML_BACKEND_DL
    static std::once_flag once;
    std::call_once(once, [] {
        // ggml looks next to the executable and in the current folder, which in an app are
        // app_process and /. The modules sit next to the library this code is linked into.
        Dl_info info{};
        std::string dir;
        if (dladdr(reinterpret_cast<const void*>(&load_backends), &info) != 0 &&
            info.dli_fname != nullptr) {
            dir = info.dli_fname;
            const std::string::size_type slash = dir.find_last_of('/');
            dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
        }
        ggml_backend_load_all_from_path(dir.empty() ? nullptr : dir.c_str());
    });
#endif
}

}  // namespace echoir::backends
