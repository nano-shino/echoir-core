// The command line's translation of final segments: the worker that runs beside the transcription,
// and how a translated segment is written in each output format.
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "doctest.h"
#include "output.h"
#include "translate.h"

namespace {

// "<target>:<text>", and the context it was given, kept for the test. Throws for "bad".
class RecordingTranslator final : public echoir::translate::ITranslator {
public:
    explicit RecordingTranslator(std::vector<std::string>* contexts) : contexts_(contexts) {}

    echoir::translate::Result translate(const echoir::translate::Request& request) override {
        contexts_->push_back(request.context);
        if (request.text == "bad") {
            throw std::runtime_error("model failed");
        }
        echoir::translate::Result result;
        result.text = request.target + ":" + request.text;
        result.capped = request.text == "long";
        return result;
    }

    std::string name() const override { return "recording"; }

private:
    std::vector<std::string>* contexts_;
};

}  // namespace

TEST_CASE("translation worker translates in order with the two earlier lines as context") {
    std::vector<std::string> contexts;
    std::vector<echoir::cli::TranslatedLine> lines;
    std::vector<std::string> errors;
    {
        echoir::cli::TranslationWorker worker(
            std::make_unique<RecordingTranslator>(&contexts), "vi",
            [&](const echoir::cli::TranslatedLine& line) { lines.push_back(line); },
            [&](std::int64_t id, const std::string& message) {
                errors.push_back(std::to_string(id) + ": " + message);
            });
        worker.submit(1, 0, 900, "one");
        worker.submit(2, 1000, 1900, "   ");  // blank: ignored
        worker.submit(3, 2000, 2900, "two");
        worker.submit(4, 3000, 3900, "bad");
        worker.submit(5, 4000, 4900, "long");
        worker.submit(6, 5000, 5900, "four");
        worker.finish();
    }
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].segment_id == 1);
    CHECK(lines[0].text == "vi:one");
    CHECK(lines[0].target == "vi");
    CHECK(lines[0].start_ms == 0);
    CHECK(lines[0].end_ms == 900);
    CHECK_FALSE(lines[0].capped);
    CHECK(lines[1].segment_id == 3);
    CHECK(lines[2].segment_id == 5);
    CHECK(lines[2].capped);
    CHECK(lines[3].segment_id == 6);

    // The context is the source of the two lines before, failed or not.
    REQUIRE(contexts.size() == 5);
    CHECK(contexts[0].empty());
    CHECK(contexts[1] == "one");
    CHECK(contexts[2] == "one\ntwo");
    CHECK(contexts[3] == "two\nbad");
    CHECK(contexts[4] == "bad\nlong");

    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "4: model failed");
}

TEST_CASE("translation json is one line with the segment's times") {
    echoir::cli::TranslatedLine line;
    line.segment_id = 7;
    line.start_ms = 1200;
    line.end_ms = 3400;
    line.target = "ja";
    line.text = "Hello \"you\"";
    line.capped = true;
    CHECK(echoir::cli::translation_json(line) ==
          R"({"type":"translation","id":7,"start_ms":1200,"end_ms":3400,"lang":"ja",)"
          R"("text":"Hello \"you\"","capped":true})");
}

TEST_CASE("translations are written after their segment in each format") {
    echoir::cli::TranslatedLine line;
    line.segment_id = 1;
    line.start_ms = 1000;
    line.end_ms = 2500;
    line.target = "vi";
    line.text = "Xin chao";

    const auto written = [&](echoir::cli::OutputFormat format) {
#ifdef _WIN32
        std::FILE* file = nullptr;
        tmpfile_s(&file);
#else
        std::FILE* file = std::tmpfile();
#endif
        REQUIRE(file != nullptr);
        {
            echoir::cli::EventWriter writer(format, file, false);
            echoir::SessionEvent event;
            event.type = ECHOIR_EVENT_FINAL;
            event.segment_id = 1;
            event.start_ms = 1000;
            event.end_ms = 2500;
            event.text = "Hello";
            writer.write(event);
            writer.write_translation(line);
        }
        std::rewind(file);
        std::string text;
        char buffer[256];
        std::size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
            text.append(buffer, n);
        }
        std::fclose(file);
        return text;
    };

    CHECK(written(echoir::cli::OutputFormat::Text) ==
          "[00:01.000 \xE2\x86\x92 00:02.500] Hello\n"
          "[00:01.000 \xE2\x86\x92 00:02.500] [vi] Xin chao\n");
    CHECK(written(echoir::cli::OutputFormat::Srt) ==
          "1\n00:00:01,000 --> 00:00:02,500\nHello\n\n"
          "2\n00:00:01,000 --> 00:00:02,500\nXin chao\n\n");
    CHECK(written(echoir::cli::OutputFormat::Jsonl) ==
          "{\"type\":\"final\",\"id\":1,\"start_ms\":1000,\"end_ms\":2500,\"text\":\"Hello\"}\n"
          "{\"type\":\"translation\",\"id\":1,\"start_ms\":1000,\"end_ms\":2500,\"lang\":\"vi\","
          "\"text\":\"Xin chao\",\"capped\":false}\n");
}
