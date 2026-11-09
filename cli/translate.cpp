#include "translate.h"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <utility>

#include "backends/translator_backend.h"
#include "model_dirs.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "text/utf.h"
#include "translate/language.h"
#include "translate/prompt.h"

namespace echoir::cli {
namespace {

// Earlier final segments sent with a segment as context.
constexpr std::size_t kContextLines = 2;

}  // namespace

std::unique_ptr<translate::ITranslator> load_translator(
    const TranscribeOptions& options, const std::vector<std::filesystem::path>& model_dirs) {
    if (translate::language_name(options.translate_to).empty()) {
        throw UsageError("--translate: unsupported language '" + options.translate_to + "'");
    }
    const Catalogue catalogue = load_catalogue(model_dirs);
    const std::string id =
        options.translator.empty() ? catalogue.registry.default_translator() : options.translator;
    if (id.empty()) {
        throw UsageError("the catalogue names no default translator; pass --translator");
    }
    const models::Model* model = catalogue.registry.find(id);
    if (model == nullptr || model->backend != "translator") {
        throw UsageError(id + " is not a translator in the registry");
    }
    const auto style =
        translate::parse_prompt_style(model->entry.value("prompt_style", std::string()));
    if (!style) {
        throw std::runtime_error("registry entry " + id + " has an unknown prompt_style");
    }

    TranslatorConfig config;
    config.model_path = text::path_utf8(models::resolve(catalogue.dir, *model, "model"));
    config.style = *style;
    config.use_gpu = options.run.use_gpu;
    config.n_threads = options.run.threads;
    return make_translator(config);
}

TranslationWorker::TranslationWorker(std::unique_ptr<translate::ITranslator> translator,
                                     std::string target, Sink sink, ErrorSink on_error,
                                     std::string glossary)
    : translator_(std::move(translator)),
      target_(std::move(target)),
      glossary_(std::move(glossary)),
      sink_(std::move(sink)),
      on_error_(std::move(on_error)) {
    thread_ = std::thread([this] { run(); });
}

TranslationWorker::~TranslationWorker() { finish(); }

void TranslationWorker::submit(std::int64_t segment_id, std::int64_t start_ms, std::int64_t end_ms,
                               const std::string& text) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string context;
        for (const std::string& line : history_) {
            context += context.empty() ? line : "\n" + line;
        }
        queue_.push_back(Job{segment_id, start_ms, end_ms, text, std::move(context)});
        history_.push_back(text);
        if (history_.size() > kContextLines) {
            history_.pop_front();
        }
    }
    wake_.notify_one();
}

void TranslationWorker::finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void TranslationWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return done_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            translate::Request request;
            request.text = job.text;
            request.target = target_;
            request.context = job.context;
            request.glossary = glossary_;
            const translate::Result result = translator_->translate(request);
            sink_(TranslatedLine{job.id, job.start_ms, job.end_ms, target_, result.text,
                                 result.capped});
        } catch (const std::exception& e) {
            on_error_(job.id, e.what());
        }
    }
}

}  // namespace echoir::cli
