using System.Runtime.InteropServices;
using System.Text.Json;
using Echoir.Core.Native;

namespace Echoir.Core;

// One stretch of a track that a transcriber run wrote. A seam is where one run ends and the next
// starts; the time between runs is not on the tape.
public readonly record struct TapeRun(long StartMs, long EndMs);

// A track of a tape, as it was when Tracks was read.
public sealed record TapeTrack(
    string Name,
    string Label,
    // The joined file of the track in the folder, written by CheckpointAsync and CloseAsync; empty
    // without a folder.
    string File,
    long DurationMs,
    // True while a transcriber writes the track.
    bool Writing,
    IReadOnlyList<TapeRun> Runs,
    // Why a page file could not be written, or null. The audio then stays in memory.
    string? Error);

// The audio of a whole recording session, kept across transcriber runs and model switches
// (echoir.h, echoir_tape_*). Give it to TranscriberOptions.Tape, and every run writes its audio to
// a track of the tape, on one timeline: a later run starts at the end of the tape. The newest
// 2 minutes of each track stay in memory; with a folder, older audio goes there as Opus pages, and
// CloseAsync joins them into <track>.opus. CheckpointAsync writes that file while the tape stays
// open. Without a folder only the newest 2 minutes are kept.
//
// Errors: audio that is missing (an unknown track, a range past the end, or audio no longer kept)
// is false or empty, never an exception. Bad arguments throw ArgumentOutOfRangeException, or
// ArgumentException for a bad track name. Every member is thread-safe.
public sealed class EchoirTape : IDisposable, IAsyncDisposable
{
    // The envelope keeps one min/max pair per this many ms.
    private const int PeakCellMs = 10;

    private readonly TapeHandle _handle;

    private EchoirTape(TapeHandle handle, string? folder)
    {
        _handle = handle;
        Folder = folder;
    }

    // The folder of the page files and the joined files; null for a tape kept in memory only.
    public string? Folder { get; }

    // The newest audio of each track that stays in memory; older audio is in the folder, or gone.
    public static TimeSpan MemoryWindow { get; } = TimeSpan.FromMinutes(2);

    // Creates a tape whose pages go to folder, which is created when it is missing. A null or empty
    // folder keeps the newest 2 minutes of each track in memory only.
    public static EchoirTape Create(string? folder)
    {
        string? normalized = string.IsNullOrEmpty(folder) ? null : folder;
        using var path = Utf8String.Create(normalized);
        nint tape = NativeMethods.TapeCreate(path.Pointer);
        if (tape == 0)
        {
            throw EchoirException.FromLastError(EchoirStatus.Internal, "echoir_tape_create");
        }
        return new EchoirTape(new TapeHandle(tape), normalized);
    }

    // The length of the tape, that of its longest track, in ms.
    public unsafe long DurationMs
    {
        get
        {
            long ms = 0;
            using Lease lease = Lease.Take(_handle);
            int status = NativeMethods.TapeDurationMs(lease.Handle, &ms);
            EchoirException.ThrowIfFailed(status, "echoir_tape_duration_ms");
            return ms;
        }
    }

    // The tracks, in the order they were added.
    public IReadOnlyList<TapeTrack> Tracks => ParseTracks(TracksJson(out _));

    // True once CloseAsync has run.
    public bool IsClosed
    {
        get
        {
            TracksJson(out bool closed);
            return closed;
        }
    }

    // Adds a track. A transcriber also adds the track it writes when it is missing, labelled with
    // its name. Throws ArgumentException for a name that is empty, longer than 64 bytes of UTF-8,
    // or equal to another track's name ignoring case; InvalidOperationException after CloseAsync.
    public void AddTrack(string name, string? label = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        using var nativeName = Utf8String.Create(name);
        using var nativeLabel = Utf8String.Create(label);
        using Lease lease = Lease.Take(_handle);
        int status = NativeMethods.TapeAddTrack(lease.Handle, nativeName.Pointer, nativeLabel.Pointer);
        if (status == NativeValues.StatusInvalidArgument)
        {
            throw new ArgumentException(LastError("echoir_tape_add_track"), nameof(name));
        }
        if (status == NativeValues.StatusState)
        {
            throw new InvalidOperationException(LastError("echoir_tape_add_track"));
        }
        EchoirException.ThrowIfFailed(status, "echoir_tape_add_track");
    }

    // Copies the audio of [startMs, endMs) of a track as 16 kHz mono samples. The end is clamped to
    // the end of the track. Audio older than 2 minutes comes from the Opus pages, so it is close to,
    // not equal to, what was recorded. Returns false, with an empty array, when the track is unknown,
    // the range starts at or after its end, or part of it is no longer kept. Throws
    // ArgumentOutOfRangeException for a negative start or an end before the start.
    public unsafe bool TryReadAudio(string track, long startMs, long endMs, out short[] samples)
    {
        ArgumentException.ThrowIfNullOrEmpty(track);
        ArgumentOutOfRangeException.ThrowIfNegative(startMs);
        ArgumentOutOfRangeException.ThrowIfLessThan(endMs, startMs);
        samples = Array.Empty<short>();
        using var name = Utf8String.Create(track);
        using Lease lease = Lease.Take(_handle);

        nuint frames = 0;
        int status = NativeMethods.TapeReadAudio(lease.Handle, name.Pointer, startMs, endMs, null, 0, &frames);
        if (status is NativeValues.StatusState or NativeValues.StatusInvalidArgument)
        {
            return false;
        }
        EchoirException.ThrowIfFailed(status, "echoir_tape_read_audio");
        if (frames == 0)
        {
            return false;
        }

        // The track may grow between the two calls, so the copy asks for whole ms that the count
        // already covered: it can then never need more room than the count gave.
        long end = Math.Min(endMs, startMs + (long)frames / 16);
        if (end <= startMs)
        {
            end = startMs + 1;
        }
        var copy = new short[(int)frames];
        nuint copied = 0;
        fixed (short* pcm = copy)
        {
            status = NativeMethods.TapeReadAudio(lease.Handle, name.Pointer, startMs, end, pcm, frames, &copied);
        }
        if (status is NativeValues.StatusState or NativeValues.StatusInvalidArgument)
        {
            return false;
        }
        EchoirException.ThrowIfFailed(status, "echoir_tape_read_audio");
        samples = copied == frames ? copy : copy[..(int)copied];
        return samples.Length > 0;
    }

    // The level envelope of [startMs, endMs) of a track: the min and max of each bucket of bucketMs,
    // a positive multiple of 10. The end is clamped to the newest whole 10 ms of the track, and the
    // last bucket may be shorter. Empty for an unknown track. Throws ArgumentOutOfRangeException for
    // a bad bucket, a negative start or an end before the start.
    public unsafe Peak[] ReadPeaks(string track, long startMs, long endMs, int bucketMs)
    {
        ArgumentException.ThrowIfNullOrEmpty(track);
        if (bucketMs <= 0 || bucketMs % PeakCellMs != 0)
        {
            throw new ArgumentOutOfRangeException(nameof(bucketMs), "The bucket must be a positive multiple of 10 ms.");
        }
        ArgumentOutOfRangeException.ThrowIfNegative(startMs);
        ArgumentOutOfRangeException.ThrowIfLessThan(endMs, startMs);
        using var name = Utf8String.Create(track);
        using Lease lease = Lease.Take(_handle);

        // The envelope grows while a transcriber writes, so the end is fixed to what the count saw.
        nuint cells = 0;
        int status = NativeMethods.TapeReadPeaks(lease.Handle, name.Pointer, 0, long.MaxValue, PeakCellMs, null, 0, &cells);
        if (status == NativeValues.StatusInvalidArgument)
        {
            return Array.Empty<Peak>();
        }
        EchoirException.ThrowIfFailed(status, "echoir_tape_read_peaks");
        long newest = (long)cells * PeakCellMs;
        long end = Math.Max(startMs, Math.Min(endMs, newest));

        nuint buckets = 0;
        status = NativeMethods.TapeReadPeaks(lease.Handle, name.Pointer, startMs, end, bucketMs, null, 0, &buckets);
        EchoirException.ThrowIfFailed(status, "echoir_tape_read_peaks");
        if (buckets == 0)
        {
            return Array.Empty<Peak>();
        }
        var raw = new float[2 * (int)buckets];
        nuint written = 0;
        fixed (float* pairs = raw)
        {
            status = NativeMethods.TapeReadPeaks(lease.Handle, name.Pointer, startMs, end, bucketMs, pairs, buckets, &written);
        }
        EchoirException.ThrowIfFailed(status, "echoir_tape_read_peaks");
        var peaks = new Peak[(int)written];
        for (int i = 0; i < peaks.Length; i++)
        {
            peaks[i] = new Peak(raw[2 * i], raw[2 * i + 1]);
        }
        return peaks;
    }

    // With a folder, writes <track>.opus for every track that has audio, on a thread pool thread: a
    // complete, playable file of everything recorded so far, while the tape stays open and runs keep
    // writing it. The tape's own pages, memory and reads do not change, and later audio continues
    // each track with no gap; a later checkpoint or CloseAsync replaces the file. Each call copies
    // the page files and encodes at most the newest 2 minutes. Does nothing without a folder or
    // after CloseAsync. Fails with an EchoirException when a file cannot be written (such as one
    // another program holds open); the other tracks are still written.
    public Task CheckpointAsync()
    {
        return Task.Run(() =>
        {
            using Lease lease = Lease.Take(_handle);
            int status = NativeMethods.TapeCheckpoint(lease.Handle);
            EchoirException.ThrowIfFailed(status, "echoir_tape_checkpoint");
        });
    }

    // Ends the tape, on a thread pool thread: transcribers still running go on but their audio is no
    // longer kept, and with a folder each track's pages are joined into <track>.opus. Reads still
    // work afterwards (from those files; without a folder only ReadPeaks has data). A second call
    // does nothing. Fails with an EchoirException when a file cannot be written.
    public Task CloseAsync()
    {
        return Task.Run(() =>
        {
            using Lease lease = Lease.Take(_handle);
            int status = NativeMethods.TapeClose(lease.Handle);
            EchoirException.ThrowIfFailed(status, "echoir_tape_close");
        });
    }

    // Releases the tape, closing it first if CloseAsync was not called. A transcriber that writes
    // it keeps its own reference, so either may be disposed first.
    public void Dispose()
    {
        _handle.Dispose();
    }

    public async ValueTask DisposeAsync()
    {
        if (!_handle.IsClosed)
        {
            try
            {
                await CloseAsync();
            }
            catch (ObjectDisposedException)
            {
            }
        }
        Dispose();
    }

    internal TapeHandle Handle => _handle;

    private string TracksJson(out bool closed)
    {
        string? json;
        using (Lease lease = Lease.Take(_handle))
        {
            nint text = NativeMethods.TapeTracksJson(lease.Handle);
            if (text == 0)
            {
                throw EchoirException.FromLastError(EchoirStatus.Internal, "echoir_tape_tracks_json");
            }
            json = Marshal.PtrToStringUTF8(text);
        }
        using JsonDocument doc = JsonDocument.Parse(json ?? "{}");
        closed = doc.RootElement.TryGetProperty("closed", out JsonElement c) && c.GetBoolean();
        return json ?? "{}";
    }

    private static IReadOnlyList<TapeTrack> ParseTracks(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        var tracks = new List<TapeTrack>();
        if (!doc.RootElement.TryGetProperty("tracks", out JsonElement list))
        {
            return tracks;
        }
        foreach (JsonElement t in list.EnumerateArray())
        {
            var runs = new List<TapeRun>();
            foreach (JsonElement r in t.GetProperty("runs").EnumerateArray())
            {
                runs.Add(new TapeRun(r.GetProperty("start_ms").GetInt64(), r.GetProperty("end_ms").GetInt64()));
            }
            tracks.Add(new TapeTrack(
                t.GetProperty("name").GetString() ?? string.Empty,
                t.GetProperty("label").GetString() ?? string.Empty,
                t.GetProperty("file").GetString() ?? string.Empty,
                t.GetProperty("duration_ms").GetInt64(),
                t.GetProperty("writing").GetBoolean(),
                runs,
                t.TryGetProperty("error", out JsonElement e) ? e.GetString() : null));
        }
        return tracks;
    }

    private static string LastError(string call)
    {
        string? text = Marshal.PtrToStringUTF8(NativeMethods.LastError());
        return string.IsNullOrEmpty(text) ? call + " failed" : text;
    }

    // Keeps the handle alive for the native calls of one member, so a Dispose on another thread
    // frees the tape only after them. Throws ObjectDisposedException after Dispose.
    internal readonly ref struct Lease
    {
        private readonly TapeHandle _owner;

        private Lease(TapeHandle owner)
        {
            _owner = owner;
            Handle = owner.DangerousGetHandle();
        }

        public nint Handle { get; }

        public static Lease Take(TapeHandle owner)
        {
            bool added = false;
            owner.DangerousAddRef(ref added);
            return new Lease(owner);
        }

        public void Dispose()
        {
            _owner.DangerousRelease();
        }
    }
}
