// Environment variables, read as UTF-8.
#pragma once

#include <optional>
#include <string>

namespace echoir::text {

// The value of an environment variable, or nullopt when it is not set. Windows has no UTF-8
// getenv, so it reads the wide value and converts it.
std::optional<std::string> get_env(const char* name);

}  // namespace echoir::text
