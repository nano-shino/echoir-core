using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;

[assembly: DoNotParallelize]

namespace Echoir.Core.Tests;

internal static class TestPaths
{
    private static readonly string Root = FindRepoRoot();

    // The models/ folder of the repository. Model files are fetched there by tools/fetch-models.
    public static string ModelsDir { get; } = Path.Combine(Root, "models");

    public static string JfkWav { get; } = Path.Combine(Root, "tests", "fixtures", "jfk.wav");

    // The files whisper-tiny-en needs, with its default VAD.
    public static bool WhisperModelsPresent =>
        File.Exists(Path.Combine(ModelsDir, "ggml-tiny.en.bin"))
        && File.Exists(Path.Combine(ModelsDir, "ggml-silero-v6.2.0.bin"));

    // Walks up from the test output folder to the directory with the top-level CMakeLists.txt.
    private static string FindRepoRoot()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
        {
            dir = dir.Parent;
        }
        return dir?.FullName ?? throw new InvalidOperationException(
            "The repository root was not found above " + AppContext.BaseDirectory);
    }
}

internal static class Wait
{
    // Polls until the condition holds, or fails the test after the timeout.
    public static void Until(Func<bool> condition, string what, int timeoutMs = 10_000)
    {
        Stopwatch clock = Stopwatch.StartNew();
        while (!condition())
        {
            if (clock.ElapsedMilliseconds > timeoutMs)
            {
                Assert.Fail($"Timed out waiting for {what}.");
            }
            Thread.Sleep(10);
        }
    }
}

// Records what the transcriber raised, from whichever thread raised it.
internal sealed class EventLog
{
    private readonly object _gate = new();
    private readonly List<string> _entries = new();

    public void Add(string entry)
    {
        lock (_gate)
        {
            _entries.Add(entry);
        }
    }

    public string[] Snapshot()
    {
        lock (_gate)
        {
            return _entries.ToArray();
        }
    }
}
