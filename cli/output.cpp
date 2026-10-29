#include "output.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "json.hpp"
#include "text/utf.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace echoir::cli {
namespace {

// Moves the cursor to the start of the line and erases it.
constexpr const char* kEraseLine = "\r\x1b[K";

// The partial line shows its newest text in at most this many terminal columns, "..." included, so
// it fits an 80-column terminal on one line. Terminals narrower than this still wrap it.
constexpr std::size_t kPartialColumns = 60;

// Terminal columns of a code point: East Asian wide and fullwidth characters take two, and most
// other characters take one. Combining marks count as one, which only makes the tail shorter.
std::size_t column_width(char32_t cp) {
    struct Range {
        char32_t first;
        char32_t last;
    };
    static constexpr Range kWide[] = {
        {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF}, {0x3400, 0x4DBF},
        {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
        {0xFE30, 0xFE4F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F},
        {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
    };
    for (const Range& range : kWide) {
        if (cp >= range.first && cp <= range.last) {
            return 2;
        }
    }
    return 1;
}

// mm:ss.mmm. Minutes keep counting past 59, so a long file still reads well.
std::string format_clock(std::int64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld.%03lld", static_cast<long long>(ms / 60000),
                  static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return buf;
}

// HH:MM:SS,mmm, the SubRip timestamp.
std::string format_srt_time(std::int64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(ms / 3600000), static_cast<long long>(ms / 60000 % 60),
                  static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return buf;
}

// The end of a partial text that fits the terminal line, with "..." in front when it is cut.
std::string terminal_tail(const std::string& text) {
    const std::u32string code_points = text::utf8_to_utf32(text);
    std::size_t width = 0;
    for (const char32_t cp : code_points) {
        width += column_width(cp);
    }
    if (width <= kPartialColumns) {
        return text;
    }
    // Keep the newest code points that still fit after the "...".
    const std::size_t budget = kPartialColumns - 3;
    std::size_t used = 0;
    std::size_t start = code_points.size();
    while (start > 0 && used + column_width(code_points[start - 1]) <= budget) {
        --start;
        used += column_width(code_points[start]);
    }
    return "..." + text::utf32_to_utf8(code_points.substr(start));
}

}  // namespace

std::string event_json(const echoir::SessionEvent& event) {
    nlohmann::ordered_json line;
    switch (event.type) {
        case ECHOIR_EVENT_PARTIAL:
            line = {{"type", "partial"},
                    {"id", event.segment_id},
                    {"start_ms", event.start_ms},
                    {"end_ms", event.end_ms},
                    {"text", event.text}};
            break;
        case ECHOIR_EVENT_FINAL:
            line = {{"type", "final"},
                    {"id", event.segment_id},
                    {"start_ms", event.start_ms},
                    {"end_ms", event.end_ms},
                    {"text", event.text}};
            break;
        case ECHOIR_EVENT_CLEARED:
            line = {{"type", "cleared"}};
            break;
        case ECHOIR_EVENT_ERROR:
            line = {{"type", "error"}, {"message", event.detail}};
            break;
        case ECHOIR_EVENT_STATS:
            // The session sends no statistics, so there is no line to write.
            return {};
    }
    // Bad UTF-8 in a message would throw, so it is replaced rather than dropping the event.
    return line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

OutputFormat parse_format(const std::string& name) {
    if (name == "text") return OutputFormat::Text;
    if (name == "jsonl") return OutputFormat::Jsonl;
    if (name == "srt") return OutputFormat::Srt;
    throw std::invalid_argument("unknown output format '" + name + "'");
}

bool is_terminal(std::FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

std::string format_segment(std::int64_t start_ms, std::int64_t end_ms, const std::string& text) {
    return "[" + format_clock(start_ms) + " → " + format_clock(end_ms) + "] " + text;
}

EventWriter::EventWriter(OutputFormat format, std::FILE* out, bool terminal)
    : format_(format), out_(out), terminal_(terminal) {}

void EventWriter::write(const echoir::SessionEvent& event) {
    switch (format_) {
        case OutputFormat::Text:
            write_text(event);
            break;
        case OutputFormat::Jsonl:
            write_jsonl(event);
            break;
        case OutputFormat::Srt:
            write_srt(event);
            break;
    }
    std::fflush(out_);
}

void EventWriter::write_text(const echoir::SessionEvent& event) {
    switch (event.type) {
        case ECHOIR_EVENT_PARTIAL:
            // Pipes get only the finals, so a partial is shown on a terminal alone.
            if (terminal_) {
                std::fprintf(out_, "%s%s", kEraseLine, terminal_tail(event.text).c_str());
                partial_shown_ = true;
            }
            break;
        case ECHOIR_EVENT_FINAL:
            clear_partial();
            // An empty final only retracts a partial, so it has no line to print.
            if (!event.text.empty()) {
                std::fprintf(out_, "%s\n",
                             format_segment(event.start_ms, event.end_ms, event.text).c_str());
            }
            break;
        case ECHOIR_EVENT_CLEARED:
        case ECHOIR_EVENT_ERROR:
            clear_partial();
            break;
        case ECHOIR_EVENT_STATS:
            break;
    }
}

void EventWriter::write_jsonl(const echoir::SessionEvent& event) {
    const std::string json = event_json(event);
    if (!json.empty()) {
        std::fprintf(out_, "%s\n", json.c_str());
    }
}

void EventWriter::write_srt(const echoir::SessionEvent& event) {
    // An empty final only retracts a partial, which srt never shows, so it is not a cue.
    if (event.type != ECHOIR_EVENT_FINAL || event.text.empty()) {
        return;
    }
    ++subtitle_index_;
    std::fprintf(out_, "%lld\n%s --> %s\n%s\n\n", static_cast<long long>(subtitle_index_),
                 format_srt_time(event.start_ms).c_str(), format_srt_time(event.end_ms).c_str(),
                 event.text.c_str());
}

void EventWriter::clear_partial() {
    if (partial_shown_) {
        std::fputs(kEraseLine, out_);
        partial_shown_ = false;
    }
}

}  // namespace echoir::cli
