#include "text/env.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cwchar>
#endif

namespace echoir::text {

std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    // Names are ASCII, so widening one is a plain copy.
    const std::wstring wide_name(name, name + std::strlen(name));
    // The secure _wgetenv_s: with a null buffer it reports the size, terminator included, and 0
    // for a variable that is not set. Then it fills a buffer of that size.
    std::size_t size = 0;
    if (_wgetenv_s(&size, nullptr, 0, wide_name.c_str()) != 0 || size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    if (_wgetenv_s(&size, value.data(), value.size(), wide_name.c_str()) != 0) {
        return std::nullopt;
    }
    value.resize(std::wcslen(value.c_str()));
    if (value.empty()) {
        return std::string();
    }
    const int wide_len = static_cast<int>(value.size());
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), wide_len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), wide_len, out.data(), bytes, nullptr, nullptr);
    return out;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

}  // namespace echoir::text
