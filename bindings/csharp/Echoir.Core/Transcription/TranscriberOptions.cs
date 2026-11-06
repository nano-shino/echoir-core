namespace Echoir.Core;

// Where the transcriber's audio comes from.
public enum AudioSource
{
    // The default microphone, or the given device.
    Mic,

    // What the machine plays, where the platform has loopback capture (Windows).
    Loopback,

    // Audio the host passes to Push.
    Push,
}

// How the text of the segment in progress is stabilized (echoir.h, echoir_stream_policy).
public enum StreamPolicy
{
    // Rollback where the model supports it, otherwise agreement.
    Default = 0,

    // Each PARTIAL keeps the words that earlier decodes of the segment agree on.
    Agreement = 1,

    // The decoder continues from its previous output. Qwen3-ASR only.
    Rollback = 2,
}

// Settings of one Start. The defaults match echoir_session_params_init.
public sealed record TranscriberOptions
{
    // "en", "zh", ... or "auto" to let the model detect the language.
    public string Language { get; init; } = "auto";

    // A decode runs once more than this much audio is pending.
    public int MinChunkMs { get; init; } = 500;

    // An ongoing segment is cut at a quiet point when it reaches this length.
    public int MaxSegmentMs { get; init; } = 10000;

    public bool EmitPartials { get; init; } = true;

    public StreamPolicy Policy { get; init; } = StreamPolicy.Default;

    // How much of the newest audio the session keeps for TryReadAudio, as 16 kHz mono (about
    // 1.9 MB a minute). Zero keeps none. The level envelope behind ReadPeaks and Level is kept
    // either way. Ignored when Tape is set.
    public TimeSpan RetainAudio { get; init; } = TimeSpan.Zero;

    // The tape that keeps the run's audio, across runs and model switches. The run writes the
    // track TapeTrack, starting at the end of the tape, and TryReadAudio, ReadPeaks and Level read
    // through to it on the run's own timeline (TapeOffsetMs maps it to the tape). Null keeps the
    // audio in the run (RetainAudio). A read-only tape (EchoirTape.Open) is refused by Start.
    public EchoirTape? Tape { get; init; }

    // The track of Tape that the run writes. Null: "system" for the loopback source, "mic" for the
    // others. A track the tape does not have is added, labelled with its name.
    public string? TapeTrack { get; init; }
}

// How a model and its VAD are loaded (echoir.h, echoir_model_params). The defaults match
// echoir_model_params_init. The VAD belongs to the engine, so every transcriber on it shares these.
public sealed record EngineLoadOptions
{
    // Registry id of the VAD. Null or empty: the default VAD of the model's backend.
    public string? VadId { get; init; }

    // The GPU backend built into the library, or the CPU when false. No effect where the library
    // has no GPU backend.
    public bool UseGpu { get; init; } = true;

    // CPU threads of the speech model. Zero: the library's default (4).
    public int Threads { get; init; }

    // Speech probability the VAD needs for speech, above 0 and at most 1; higher hears less as
    // speech. Zero: the VAD's own value (Silero 0.2, FireRedVAD 0.4).
    public float VadThreshold { get; init; }

    // Shortest pause that ends a speech range, in ms, up to 60000; lower ends lines sooner.
    // Zero: the VAD's own value (Silero 1500, FireRedVAD 500).
    public int VadMinSilenceMs { get; init; }

    // Throws ArgumentOutOfRangeException for a value the library would refuse.
    internal void Validate()
    {
        ArgumentOutOfRangeException.ThrowIfNegative(Threads, nameof(Threads));
        if (!(VadThreshold >= 0f && VadThreshold <= 1f))
        {
            throw new ArgumentOutOfRangeException(nameof(VadThreshold), VadThreshold, "Must be from 0 to 1.");
        }
        ArgumentOutOfRangeException.ThrowIfNegative(VadMinSilenceMs, nameof(VadMinSilenceMs));
        ArgumentOutOfRangeException.ThrowIfGreaterThan(VadMinSilenceMs, 60000, nameof(VadMinSilenceMs));
    }
}

// The state of a capture (echoir.h, echoir_capture_state).
public enum CaptureState
{
    // No device is captured: not running, or the Push source.
    Off = 0,

    // The device is open and its audio is transcribed.
    Live = 1,

    // The default device was lost and none has come back for a second or more, such as when the
    // only headset is unplugged. The capture resumes by itself when a device appears.
    Waiting = 2,

    // The capture ended by itself: the chosen device was lost (Error is raised with DeviceLost)
    // or the capture failed. It stays so until StopAsync.
    Ended = 3,
}

// An error from the session, such as a lost device. DeviceLost is true for "device_lost".
public sealed class TranscriberErrorEventArgs : EventArgs
{
    public TranscriberErrorEventArgs(string message)
    {
        Message = message;
    }

    public string Message { get; }

    public bool DeviceLost => Message == "device_lost";
}
