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
    // either way.
    public TimeSpan RetainAudio { get; init; } = TimeSpan.Zero;
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
