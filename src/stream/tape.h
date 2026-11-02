// The session tape: the audio of a whole recording session, kept across the sessions (runs) that
// write it, for hosts that replay or save it.
//
// A tape holds named tracks (one per input, such as "mic" and "system"), each with a display label,
// on one timeline that starts at 0. A session attaches to one track and appends its 16 kHz mono
// audio there. A session that attaches later starts at the end of the tape (its longest track,
// rounded up to whole 10 ms), so the time between two runs is not stored: the runs meet at a seam.
// A track that is shorter than the end of the tape when a session attaches is padded with silence
// up to it, so every track keeps the same timeline. Several sessions may write several tracks at
// once; a track has one writer at a time.
//
// Storage, per track: the newest kMemoryMs as 16-bit samples in memory. With a folder, older audio
// is encoded with Opus (kBitrate, 20 ms frames, one continuous stream per track) and written to the
// folder as Ogg Opus page files of kPageMs each, as each one fills, so a crash loses at most the
// page being filled. Without a folder, older audio is dropped. close() joins each track's pages
// into one <track>.opus file and deletes the page files. checkpoint() writes that file while the
// tape stays open, so everything recorded so far is on disk in one playable file. Reads by time
// range come from memory or from the pages, decoding only the pages they need. The 10 ms level
// envelope covers the whole tape. File IO uses the C++ standard library only.
//
// Thread-safe: every call may run on any thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace echoir::stream {

class Tape : public std::enable_shared_from_this<Tape> {
public:
    // The newest audio of each track that stays in memory as 16-bit samples.
    static constexpr std::int64_t kMemoryMs = 120000;
    // The audio of one page file.
    static constexpr std::int64_t kPageMs = 60000;
    // The Opus bitrate of the pages, in bits per second.
    static constexpr std::int32_t kBitrate = 24000;
    // A track name is 1 to this many bytes of UTF-8.
    static constexpr std::size_t kMaxNameBytes = 64;

    // A tape that keeps its pages in folder, which is created when it is missing. An empty folder
    // keeps only the newest kMemoryMs of each track, in memory. Throws std::runtime_error when the
    // folder cannot be created.
    static std::shared_ptr<Tape> create(const std::filesystem::path& folder);
    ~Tape();
    Tape(const Tape&) = delete;
    Tape& operator=(const Tape&) = delete;

    // Adds a track. label is the name to show; empty means the name. Throws std::invalid_argument
    // for an empty or too long name, or a name that equals an existing one ignoring ASCII case
    // (their files would collide on a case-insensitive file system). Throws StateError after
    // close().
    void add_track(const std::string& name, const std::string& label);

    // The tracks as a JSON object:
    //   {"duration_ms":N,"closed":bool,"tracks":[{"name","label","file","duration_ms","writing",
    //    "runs":[{"start_ms","end_ms"}],"error"}]}
    // file is the name of the joined file in the folder (written by close()), runs are the
    // stretches that sessions wrote (the seams are between them), and error, present only after a
    // page could not be written, is the reason.
    std::string tracks_json() const;

    // The end of the tape: the length of its longest track, in whole ms.
    std::int64_t duration_ms() const;

    // Copies the audio of [start_ms, end_ms) of a track as 16-bit samples into out, which holds
    // cap samples, and returns how many there are. end_ms is clamped to the end of the track, and
    // to end_limit_samples when that is smaller. With out null, only counts them. Throws
    // std::invalid_argument for an unknown track, a negative start, an end before the start, or a
    // cap too small. Throws StateError when part of the range is no longer kept (a tape without a
    // folder keeps kMemoryMs; a closed one without a folder keeps none). Throws
    // std::runtime_error when a page file cannot be read.
    std::size_t read_audio(const std::string& track, std::int64_t start_ms, std::int64_t end_ms,
                           std::int16_t* out, std::size_t cap,
                           std::int64_t end_limit_samples = INT64_MAX) const;

    // The level envelope of [start_ms, end_ms) of a track, as Envelope::read, with the end also
    // clamped to limit_cells whole cells. Throws std::invalid_argument for an unknown track and
    // the argument errors of Envelope::read.
    std::size_t read_peaks(const std::string& track, std::int64_t start_ms, std::int64_t end_ms,
                           std::int64_t bucket_ms, float* out, std::size_t cap,
                           std::int64_t limit_cells = INT64_MAX) const;

    // With a folder, writes <track>.opus for every track that has audio: a complete Ogg Opus file
    // of the whole track so far, while the tape stays open. The page files are copied into it, and
    // the audio still in memory is encoded on a copy of the track's encoder, so the track's own
    // stream, its memory and its reads are unchanged, and later audio continues it with no gap. A
    // later checkpoint or close() replaces the file. Each call copies the page files and encodes
    // at most the newest kMemoryMs. Without a folder, or after close(), does nothing. Throws
    // std::runtime_error when a file cannot be written; the other tracks are still written.
    void checkpoint();

    // Ends the tape: no more writes, and with a folder, the rest of each track is encoded and its
    // pages are joined into <track>.opus. Later reads come from that file. Without a folder the
    // audio is dropped and only the envelope stays. A second call does nothing. Throws
    // std::runtime_error when a file cannot be written; the tape is closed even then.
    void close();
    bool closed() const;

private:
    struct Track;

public:
    // Writing. A Writer attaches a session to a track and appends to it until it is destroyed.
    class Writer {
    public:
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // Appends 16 kHz mono samples (full scale ±1). Dropped after the tape is closed.
        void append(std::span<const float> pcm);
        // Ends this run and starts a new one at the end of the tape, as a new attach does.
        void restart();
        // Ends this run and starts a new one where the track ends now, with no padding, so the
        // writer's audio keeps its place on the tape (start_samples() does not move). For a stretch
        // of time the writer leaves out, such as a long capture gap. Does nothing when the current
        // run is still empty.
        void seam();
        // Where this run starts on the tape, in samples (a whole number of 10 ms).
        std::int64_t start_samples() const noexcept;
        const std::string& track() const noexcept;
        const std::shared_ptr<Tape>& tape() const noexcept { return tape_; }

    private:
        friend class Tape;
        Writer(std::shared_ptr<Tape> tape, Track* track, std::int64_t start);
        std::shared_ptr<Tape> tape_;
        Track* track_;  // lives as long as the tape
        std::atomic<std::int64_t> start_;
    };

    // Attaches a writer to a track, adding the track (labelled with its name) when it is missing.
    // The run starts at the end of the tape, rounded up to whole 10 ms, and the track is padded
    // with silence up to there. Throws std::invalid_argument for a bad name, and StateError when
    // the track already has a writer or the tape is closed.
    std::unique_ptr<Writer> attach(const std::string& track);

private:
    explicit Tape(std::filesystem::path folder);
    Track* find_locked(const std::string& name) const;
    Track& add_locked(const std::string& name, const std::string& label);
    std::int64_t end_samples_locked() const;
    // Starts a run of a track at the end of the tape. Caller holds mu_, not the track's lock.
    std::int64_t begin_run(Track& track);

    const std::filesystem::path folder_;
    // Guards the track list and the closed flag. Taken before a track's own lock.
    mutable std::mutex mu_;
    // Held shared while page files are read, and exclusively by close(), which replaces them.
    // Taken before mu_ and before any track lock.
    mutable std::shared_mutex files_mu_;
    // One checkpoint at a time, as they write the same files. Taken before files_mu_.
    std::mutex checkpoint_mu_;
    std::vector<std::unique_ptr<Track>> tracks_;
    bool closed_ = false;
};

}  // namespace echoir::stream
