#include "model_dirs.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "models/registry.h"
#include "text/env.h"
#include "text/utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace echoir::cli {
namespace {

namespace fs = std::filesystem;

// Resolves "..": a build-tree path such as bin/../../../models reads as the plain repository path.
// Falls back to the path as given when the file system cannot resolve it.
fs::path tidy(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return ec ? path : canonical;
}

// The directory of the running executable. Empty when neither the platform nor argv0 gives one.
fs::path executable_dir(const char* argv0) {
    fs::path exe;
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        exe = buf;
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // fails, but reports the size it needs
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        exe = buf.c_str();
    }
#elif defined(__linux__)
    std::error_code link_ec;
    exe = fs::read_symlink("/proc/self/exe", link_ec);
    if (link_ec) {
        exe.clear();
    }
#endif
    if (exe.empty() && argv0 != nullptr) {
        exe = argv0;
    }
    // A PATH lookup leaves argv0 bare, so there is no directory to use.
    std::error_code ec;
    const fs::path canonical = fs::canonical(exe, ec);
    return ec ? exe.parent_path() : canonical.parent_path();
}

}  // namespace

std::vector<fs::path> model_search_dirs(const std::string& models_dir, const char* argv0) {
    std::vector<fs::path> dirs;
    if (!models_dir.empty()) {
        dirs.push_back(text::path_from_utf8(models_dir));
    }
    if (const auto env = text::get_env("ECHOIR_MODELS"); env && !env->empty()) {
        dirs.push_back(text::path_from_utf8(*env));
    }
    dirs.emplace_back("models");
    const fs::path exe_dir = executable_dir(argv0);
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir / "models");
        // build/<preset>/bin is three levels below the repository root.
        dirs.push_back(tidy(exe_dir / ".." / ".." / ".." / "models"));
    }
    return dirs;
}

Catalogue load_catalogue(const std::vector<fs::path>& search_dirs) {
    for (const fs::path& dir : search_dirs) {
        const fs::path registry_file = dir / "registry.json";
        std::error_code ec;
        if (!fs::is_regular_file(registry_file, ec)) {
            continue;
        }
        std::ifstream in(registry_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        try {
            return Catalogue{models::Registry::parse(text), fs::absolute(dir)};
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error("cannot read " + text::path_utf8(registry_file) + ": " +
                                     e.what());
        }
    }

    // No registry.json: the built-in catalogue, with the files of the first directory that exists.
    fs::path models_dir = search_dirs.front();
    for (const fs::path& dir : search_dirs) {
        std::error_code ec;
        if (fs::is_directory(dir, ec)) {
            models_dir = dir;
            break;
        }
    }
    return Catalogue{models::Registry::builtin(), fs::absolute(models_dir)};
}

}  // namespace echoir::cli
