// Exception types that the C ABI maps to echoir_status codes (see src/capi/capi.cpp). All of them
// derive from std::runtime_error, so code that catches std::runtime_error still catches them.
#pragma once

#include <stdexcept>

namespace echoir {

// A model file is missing, unreadable, truncated, not the expected format, or does not load.
class ModelLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The backend that was asked for is not compiled into this build.
class BackendUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A backend or the VAD failed while a stream was running (a decode or a detection error).
class BackendFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A call does not fit the session's state: process() while the worker runs, or start() after
// process() has run. The C ABI maps it to ECHOIR_ERR_STATE.
class StateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The request needs a feature that the current audio backend does not have, such as loopback
// capture where there is no loopback. The C ABI maps it to ECHOIR_ERR_NOT_IMPLEMENTED.
class NotSupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace echoir
