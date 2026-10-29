// Log routing for the backend libraries. whisper.cpp, ggml and llama.cpp print
// nothing unless ECHOIR_LOG=debug. stdout belongs to the CLI's JSONL output.
#pragma once

namespace echoir::backends {

// Installs the shared log callbacks once. Later calls do nothing.
void configure_backend_logs();

// While alive on this thread, notes any WARN or ERROR that the backend libraries log. A model
// loader reports a bad file this way instead of through its return value.
// Watches nest, and each one restores the outer watch when it ends. Use one on one thread only.
class LoadWatch {
public:
    LoadWatch();
    ~LoadWatch();
    LoadWatch(const LoadWatch&) = delete;
    LoadWatch& operator=(const LoadWatch&) = delete;

    bool problem() const { return problem_; }

private:
    bool problem_ = false;
    bool* previous_ = nullptr;
};

}  // namespace echoir::backends
