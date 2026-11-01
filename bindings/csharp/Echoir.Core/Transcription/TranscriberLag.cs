using System.Globalization;
using System.Text.Json;

namespace Echoir.Core;

// Whether the decoding keeps up with the audio, from EchoirTranscriber.Lag.
public enum LagState
{
    // No session is running.
    Idle,

    // The captions follow the audio within about a tick.
    Live,

    // The decoder is close to its limit: the captions trail by a few seconds, or the backlog creeps up.
    Strained,

    // The decoder is slower than real time: the backlog is large or keeps growing.
    Behind,
}

// One reading of how far the decoding is behind the audio taken in.
//   State: the verdict, held for a moment after it eases so a display does not flicker.
//   Backlog: the largest backlog of the last two seconds, so how far behind the captions may be.
//   Current: the backlog now. It rises during each decode and drops when the decode ends.
//   Trend: backlog gained per second of real time over the last ten seconds. Near 0 while the
//     decoder keeps up; positive while it decodes slower than the audio arrives.
//   TickRunning: how long the decode in progress has run; zero between decodes.
public readonly record struct TranscriberLag(
    LagState State,
    TimeSpan Backlog,
    TimeSpan Current,
    double Trend,
    TimeSpan TickRunning)
{
    // The reading while no session runs.
    public static TranscriberLag Idle { get; } = new(LagState.Idle, TimeSpan.Zero, TimeSpan.Zero, 0, TimeSpan.Zero);
}

// The speed of the decoding over a run of ticks, from one STATS event of the core.
public sealed record TranscriberStats
{
    // Decode time over the audio of the windows, carried audio included: how fast the model
    // decodes. 1 / Rtf is how many times faster than real time.
    public double Rtf { get; init; }

    // Decode time over the new audio: the share of real time the decoder is busy. Once a decode
    // takes longer than MinChunkMs the ticks run back to back and this stays near 1 whether or not
    // the session keeps up; EchoirTranscriber.Lag says that.
    public double Load { get; init; }

    // The Rtf over the ticks that decoded speech only; 0 when none did. Silence is cheap to decode,
    // so this is the speed of a model on speech.
    public double SpeechRtf { get; init; }

    public int Ticks { get; init; }

    // Means per tick, in ms.
    public int DecodeMs { get; init; }

    public int WindowMs { get; init; }

    public int NewMs { get; init; }

    public int SpeechMs { get; init; }

    // The slowest tick, in ms.
    public int DecodeMaxMs { get; init; }

    public string Backend { get; init; } = string.Empty;

    // Reads the detail of a STATS event. Fields the core did not send stay zero. Null for text that
    // is not a JSON object.
    public static TranscriberStats? Parse(string json)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return null;
            }
            return new TranscriberStats
            {
                Rtf = Number(root, "rtf"),
                Load = Number(root, "load"),
                SpeechRtf = Number(root, "speech_rtf"),
                Ticks = (int)Number(root, "ticks"),
                DecodeMs = (int)Number(root, "decode_ms"),
                WindowMs = (int)Number(root, "window_ms"),
                NewMs = (int)Number(root, "new_ms"),
                SpeechMs = (int)Number(root, "speech_ms"),
                DecodeMaxMs = (int)Number(root, "decode_max_ms"),
                Backend = root.TryGetProperty("backend", out JsonElement backend) && backend.ValueKind == JsonValueKind.String
                    ? backend.GetString() ?? string.Empty
                    : string.Empty,
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public override string ToString() => string.Create(
        CultureInfo.InvariantCulture,
        $"rtf {Rtf:0.000} (speech {SpeechRtf:0.000}), load {Load:0.00}, decode {DecodeMs} ms (max {DecodeMaxMs}), window {WindowMs} ms");

    private static double Number(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number
            ? value.GetDouble()
            : 0;
    }
}

// How a model did over one run, from EchoirTranscriber.RunSpeed: what a host keeps per model to
// show how fast it is on this machine.
public sealed record RunSpeed
{
    public string Backend { get; init; } = string.Empty;

    // How long the session ran.
    public TimeSpan Duration { get; init; }

    // The speech the ticks decoded. Audio carried into later ticks counts again with each decode.
    public TimeSpan Speech { get; init; }

    // The median of the speech rtf over the STATS events that saw speech: how fast the model
    // decodes speech. Zero when none did.
    public double SpeechRtfMedian { get; init; }

    // The number of STATS events the median comes from.
    public int Samples { get; init; }

    // How far the captions trailed the audio for most of the run: the 90th percentile of the
    // backlog peak of each second (LagMonitor.TypicalBacklog). A busy second or two on the
    // machine does not move it.
    public TimeSpan TypicalBacklog { get; init; }

    public TimeSpan MaxBacklog { get; init; }

    // The share of the run spent in LagState.Behind, from 0 to 1.
    public double BehindShare { get; init; }
}
