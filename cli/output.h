// Output of the transcription commands. The same events are printed in three formats.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "stream/session.h"

namespace echoir::cli {

// text: one line per final segment. On a terminal, the partial text is redrawn in place.
// jsonl: one JSON object per event, as in the protocol. srt: one subtitle per final segment.
enum class OutputFormat { Text, Jsonl, Srt };

// Throws std::invalid_argument for a name other than text, jsonl or srt.
OutputFormat parse_format(const std::string& name);

// The JSON object of an event, on one line with no line break, as jsonl prints it and as the
// stdio service sends it. Empty for the stats kind, which this version never sends.
std::string event_json(const echoir::SessionEvent& event);

// Whether the stream is an interactive terminal, so the partial text can be redrawn in place.
bool is_terminal(std::FILE* stream);

// The line that `echoir transcribe` prints for a final segment: "[mm:ss.mmm → mm:ss.mmm] text".
std::string format_segment(std::int64_t start_ms, std::int64_t end_ms, const std::string& text);

// Writes session events to a stream, one format. Each event is flushed, so a pipe reader sees it
// at once. In text and srt output an error is not written: the caller reports it on standard
// error. In jsonl output it is an event like any other.
class EventWriter {
public:
    EventWriter(OutputFormat format, std::FILE* out, bool terminal);

    void write(const echoir::SessionEvent& event);

private:
    void write_text(const echoir::SessionEvent& event);
    void write_jsonl(const echoir::SessionEvent& event);
    void write_srt(const echoir::SessionEvent& event);
    // Erases the partial line that a terminal shows, if one is shown.
    void clear_partial();

    OutputFormat format_;
    std::FILE* out_;
    bool terminal_;
    bool partial_shown_ = false;
    std::int64_t subtitle_index_ = 0;
};

}  // namespace echoir::cli
