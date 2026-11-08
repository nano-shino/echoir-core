// Translation of the final segments of a transcription run (`transcribe --translate`).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "options.h"
#include "translate/translator.h"

namespace echoir::cli {

// One translated segment. Times are the source segment's.
struct TranslatedLine {
    std::int64_t segment_id = 0;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string target;  // ISO code
    std::string text;
    bool capped = false;
};

// Loads the translator of the options (--translator, else the catalogue's default) and checks the
// target language, before any audio is read. Throws UsageError for a language the translator does
// not support or an id that is not a translator, and std::runtime_error when the model is missing.
// Loading includes a short warm-up.
std::unique_ptr<translate::ITranslator> load_translator(
    const TranscribeOptions& options, const std::vector<std::filesystem::path>& model_dirs);

// Translates final segments on a thread of its own, one at a time in the order they were given,
// each with the two segments before it as context. The segment that is submitted never waits for
// a translation, so a decoder is not slowed by it. Unlike the library's TranscriptTranslator, it
// drops nothing: a command line run wants every line.
class TranslationWorker {
public:
    using Sink = std::function<void(const TranslatedLine&)>;
    // Called, on the worker's thread, for a segment whose translation failed.
    using ErrorSink = std::function<void(std::int64_t segment_id, const std::string& message)>;

    TranslationWorker(std::unique_ptr<translate::ITranslator> translator, std::string target,
                      Sink sink, ErrorSink on_error);
    ~TranslationWorker();
    TranslationWorker(const TranslationWorker&) = delete;
    TranslationWorker& operator=(const TranslationWorker&) = delete;

    // Queues a final segment. Blank text is ignored.
    void submit(std::int64_t segment_id, std::int64_t start_ms, std::int64_t end_ms,
                const std::string& text);

    // Waits for every queued segment to be translated, then stops the thread.
    void finish();

private:
    struct Job {
        std::int64_t id = 0;
        std::int64_t start_ms = 0;
        std::int64_t end_ms = 0;
        std::string text;
        std::string context;
    };
    void run();

    std::unique_ptr<translate::ITranslator> translator_;
    std::string target_;
    Sink sink_;
    ErrorSink on_error_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::deque<std::string> history_;  // the last final texts, oldest first; under mutex_
    bool done_ = false;
    std::thread thread_;
};

}  // namespace echoir::cli
