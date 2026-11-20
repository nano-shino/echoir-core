// The JSON line of a stats event, as jsonl prints it and the stdio service sends it.
#include <string>

#include "doctest.h"
#include "output.h"
#include "stream/session.h"

TEST_CASE("event json: a stats event is a flat object with its type first") {
    echoir::SessionEvent ev;
    ev.type = ECHOIR_EVENT_STATS;
    ev.detail = R"({"rtf":0.12,"decode_ms":180,"window_ms":2600,"backend":"qwen3-asr"})";
    CHECK(echoir::cli::event_json(ev) ==
          R"({"type":"stats","rtf":0.12,"decode_ms":180,"window_ms":2600,"backend":"qwen3-asr"})");
}

TEST_CASE("event json: a final carries the detected language after its text") {
    echoir::SessionEvent ev;
    ev.type = ECHOIR_EVENT_FINAL;
    ev.segment_id = 3;
    ev.start_ms = 100;
    ev.end_ms = 900;
    ev.text = "Hello.";
    ev.language = "en";
    CHECK(echoir::cli::event_json(ev) ==
          R"({"type":"final","id":3,"start_ms":100,"end_ms":900,"text":"Hello.","language":"en"})");
    ev.type = ECHOIR_EVENT_PARTIAL;
    ev.language.clear();
    CHECK(echoir::cli::event_json(ev) ==
          R"({"type":"partial","id":3,"start_ms":100,"end_ms":900,"text":"Hello."})");
}

TEST_CASE("event json: a stats detail that is not an object writes no line") {
    echoir::SessionEvent ev;
    ev.type = ECHOIR_EVENT_STATS;
    ev.detail = "not json";
    CHECK(echoir::cli::event_json(ev).empty());
    ev.detail = "[1,2]";
    CHECK(echoir::cli::event_json(ev).empty());
}
