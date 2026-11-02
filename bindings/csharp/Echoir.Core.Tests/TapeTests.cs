using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// EchoirTape, the transcriber writing it across runs, and TranscribeSamplesAsync. These run only
// against the ECHOIR_FAKE_BACKEND build, whose scripted VAD reports all audio as speech in
// 1000 ms segments, so audio in memory compares sample for sample.
[TestClass]
[TestCategory("fake")]
public sealed class TapeTests
{
    private const int SampleRate = 16000;

    [TestMethod]
    public async Task TheTapeSpansTwoRunsWithASeamBetween()
    {
        using var folder = new TempFolder();
        await using var tape = EchoirTape.Create(folder.Path);
        await using var transcriber = await LoadAsync();
        short[] first = Voice(3 * SampleRate, seed: 1);
        short[] second = Voice(2 * SampleRate, seed: 2);
        var options = new TranscriberOptions { Tape = tape };

        transcriber.Start(AudioSource.Push, options: options);
        Assert.AreEqual(0L, transcriber.TapeOffsetMs);
        transcriber.Push(first, SampleRate, 1);
        await transcriber.StopAsync();

        transcriber.Start(AudioSource.Push, options: options);
        Assert.AreEqual(3000L, transcriber.TapeOffsetMs, "The second run does not start at the end of the tape.");
        transcriber.Push(second, SampleRate, 1);
        await transcriber.StopAsync();

        Assert.AreEqual(5000L, tape.DurationMs);
        TapeTrack mic = tape.Tracks.Single();
        Assert.AreEqual("mic", mic.Name);
        CollectionAssert.AreEqual(new[] { new TapeRun(0, 3000), new TapeRun(3000, 5000) }, mic.Runs.ToArray());

        // One read across the seam: the end of the first run, then the start of the second.
        Assert.IsTrue(tape.TryReadAudio("mic", 2500, 3500, out short[] across));
        CollectionAssert.AreEqual(Slice(first, 2500, 3000).Concat(Slice(second, 0, 500)).ToArray(), across);

        // The transcriber reads its last run on the run's own timeline.
        Segment segment = transcriber.Transcript.Finals.Last();
        Assert.IsTrue(segment.StartMs < 2000, "The last segment is not from the second run.");
        Assert.IsTrue(transcriber.TryReadAudio(segment, out short[] own, padMs: 0));
        CollectionAssert.AreEqual(Slice(second, segment.StartMs, segment.EndMs), own);
        Assert.IsTrue(tape.TryReadAudio("mic", 3000 + segment.StartMs, 3000 + segment.EndMs, out short[] onTape));
        CollectionAssert.AreEqual(own, onTape);
        Assert.AreEqual(100, transcriber.ReadPeaks(0, 1000, 10).Length);
        Assert.AreEqual(500, tape.ReadPeaks("mic", 0, long.MaxValue, 10).Length);
    }

    [TestMethod]
    public async Task ARangeOlderThanTwoMinutesIsReadFromThePages()
    {
        using var folder = new TempFolder();
        await using var tape = EchoirTape.Create(folder.Path);
        await using var transcriber = await LoadAsync();
        short[] audio = Voice(190 * SampleRate, seed: 3);

        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape, TapeTrack = "voice" });
        transcriber.Push(audio, SampleRate, 1);
        await transcriber.StopAsync();
        Assert.IsTrue(File.Exists(System.IO.Path.Combine(folder.Path, "voice.00001.opus")), "No page file was written.");

        // Decoded Opus: the same length, close to the original, but not equal to it.
        Assert.IsTrue(tape.TryReadAudio("voice", 10_000, 20_000, out short[] paged));
        short[] original = Slice(audio, 10_000, 20_000);
        Assert.AreEqual(original.Length, paged.Length);
        double correlation = Correlation(original, paged);
        Assert.IsTrue(correlation > 0.95, $"The paged audio correlates {correlation:F3} with the original.");
        CollectionAssert.AreNotEqual(original, paged);

        // Through the transcriber as well, for a segment of the first seconds.
        Segment early = transcriber.Transcript.Finals.First();
        Assert.IsTrue(transcriber.TryReadAudio(early, out short[] fromRun, padMs: 0));
        Assert.AreEqual((int)((early.EndMs - early.StartMs) * 16), fromRun.Length);

        // The newest 2 minutes are exact.
        Assert.IsTrue(tape.TryReadAudio("voice", 100_000, 101_000, out short[] recent));
        CollectionAssert.AreEqual(Slice(audio, 100_000, 101_000), recent);
    }

    [TestMethod]
    public async Task WithoutAFolderOnlyTheNewestTwoMinutesAreKept()
    {
        await using var tape = EchoirTape.Create(null);
        await using var transcriber = await LoadAsync();
        short[] audio = Voice(130 * SampleRate, seed: 4);
        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        transcriber.Push(audio, SampleRate, 1);
        await transcriber.StopAsync();

        Assert.IsNull(tape.Folder);
        Assert.IsFalse(tape.TryReadAudio("mic", 0, 1000, out short[] gone));
        Assert.AreEqual(0, gone.Length);
        Assert.IsFalse(transcriber.TryReadAudio(transcriber.Transcript.Finals.First(), out _));
        Assert.IsTrue(tape.TryReadAudio("mic", 20_000, 21_000, out short[] kept));
        CollectionAssert.AreEqual(Slice(audio, 20_000, 21_000), kept);
    }

    [TestMethod]
    public async Task DisposingTheTranscriberFirstKeepsTheTape()
    {
        using var folder = new TempFolder();
        var tape = EchoirTape.Create(folder.Path);
        var transcriber = await LoadAsync();
        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        transcriber.Push(Voice(2 * SampleRate, seed: 5), SampleRate, 1);

        await transcriber.DisposeAsync();
        Assert.IsTrue(tape.TryReadAudio("mic", 0, 2000, out short[] samples));
        Assert.AreEqual(2 * SampleRate, samples.Length);
        Assert.IsFalse(tape.Tracks.Single().Writing);

        await tape.CloseAsync();
        Assert.IsTrue(tape.IsClosed);
        Assert.IsTrue(File.Exists(System.IO.Path.Combine(folder.Path, "mic.opus")));
        // A closed tape still reads, from the joined file.
        Assert.IsTrue(tape.TryReadAudio("mic", 0, 2000, out short[] joined));
        Assert.AreEqual(2 * SampleRate, joined.Length);
        await tape.DisposeAsync();
        Assert.ThrowsException<ObjectDisposedException>(() => tape.TryReadAudio("mic", 0, 10, out _));
    }

    [TestMethod]
    public async Task CheckpointWritesThePlayableFileWhileTheTapeStaysOpen()
    {
        using var folder = new TempFolder();
        await using var tape = EchoirTape.Create(folder.Path);
        await using var transcriber = await LoadAsync();
        string file = System.IO.Path.Combine(folder.Path, "mic.opus");
        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        transcriber.Push(Voice(2 * SampleRate, seed: 8), SampleRate, 1);
        await transcriber.StopAsync();
        Assert.IsFalse(File.Exists(file));

        await tape.CheckpointAsync();
        Assert.IsTrue(File.Exists(file));
        long first = new FileInfo(file).Length;
        Assert.IsFalse(tape.IsClosed);

        // A later run goes on writing, and the next checkpoint replaces the file with the longer track.
        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        transcriber.Push(Voice(2 * SampleRate, seed: 9), SampleRate, 1);
        await transcriber.StopAsync();
        await tape.CheckpointAsync();
        Assert.IsTrue(new FileInfo(file).Length > first);
        Assert.IsTrue(tape.TryReadAudio("mic", 0, 4000, out short[] samples));
        Assert.IsTrue(samples.Length >= 4 * SampleRate - 16);

        await tape.CloseAsync();
        await tape.CheckpointAsync();
        Assert.IsTrue(File.Exists(file));

        await using var memory = EchoirTape.Create(null);
        await memory.CheckpointAsync();
    }

    [TestMethod]
    public async Task DisposingTheTapeFirstWhileARunWritesItIsSafe()
    {
        using var folder = new TempFolder();
        var tape = EchoirTape.Create(folder.Path);
        await using var transcriber = await LoadAsync();
        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        transcriber.Push(Voice(2 * SampleRate, seed: 6), SampleRate, 1);

        // Dispose closes the tape and joins its file. The run goes on; its new audio is not kept.
        tape.Dispose();
        Assert.IsTrue(File.Exists(System.IO.Path.Combine(folder.Path, "mic.opus")));
        transcriber.Push(Voice(2 * SampleRate, seed: 7), SampleRate, 1);
        await transcriber.StopAsync();
        Assert.IsTrue(transcriber.Transcript.Finals.Count > 0);
        Assert.IsTrue(transcriber.TryReadAudio(new Segment(0, 0, 1000, string.Empty), out short[] early, padMs: 0));
        Assert.AreEqual(SampleRate, early.Length);
        transcriber.TryReadAudio(new Segment(0, 3000, 4000, string.Empty), out short[] late, padMs: 0);
        Assert.AreEqual(0, late.Length, "Audio written after the tape closed was kept.");

        // A new run on a disposed tape is refused before it starts.
        Assert.ThrowsException<ObjectDisposedException>(
            () => transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape }));
    }

    [TestMethod]
    public async Task TwoTracksShareTheTimeline()
    {
        await using var tape = EchoirTape.Create(null);
        tape.AddTrack("system", "System audio");
        await using var mic = await LoadAsync();
        await using var system = await LoadAsync();
        mic.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        mic.Push(Voice(SampleRate, seed: 1), SampleRate, 1);
        system.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape, TapeTrack = "system" });
        Assert.AreEqual(1000L, system.TapeOffsetMs, "A track added mid-run does not start at the end of the tape.");
        system.Push(Voice(SampleRate, seed: 2), SampleRate, 1);
        await mic.StopAsync();
        await system.StopAsync();

        TapeTrack[] tracks = tape.Tracks.ToArray();
        CollectionAssert.AreEqual(new[] { "system", "mic" }, tracks.Select(t => t.Name).ToArray());
        Assert.AreEqual("System audio", tracks[0].Label);
        Assert.AreEqual(2000L, tracks[0].DurationMs);
        Assert.IsTrue(tape.TryReadAudio("system", 0, 1000, out short[] before));
        Assert.IsTrue(before.All(s => s == 0), "The stretch before the system run is not silence.");
    }

    [TestMethod]
    public async Task TapeMembersFollowOneErrorConvention()
    {
        await using var tape = EchoirTape.Create(null);
        tape.AddTrack("mic");
        Assert.ThrowsException<ArgumentException>(() => tape.AddTrack("MIC"));
        Assert.ThrowsException<ArgumentException>(() => tape.AddTrack(""));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => tape.TryReadAudio("mic", -1, 10, out _));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => tape.TryReadAudio("mic", 20, 10, out _));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => tape.ReadPeaks("mic", 0, 100, 15));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => tape.ReadPeaks("mic", -1, 100, 10));
        Assert.ThrowsException<ArgumentOutOfRangeException>(() => tape.ReadPeaks("mic", 20, 10, 10));
        Assert.ThrowsException<ArgumentException>(() => tape.TryReadAudio("", 0, 10, out _));
        Assert.ThrowsException<ArgumentNullException>(() => tape.TryReadAudio(null!, 0, 10, out _));
        Assert.ThrowsException<ArgumentException>(() => tape.ReadPeaks("", 0, 100, 10));
        // Missing audio is false or empty.
        Assert.IsFalse(tape.TryReadAudio("none", 0, 10, out short[] none));
        Assert.AreEqual(0, none.Length);
        Assert.IsFalse(tape.TryReadAudio("mic", 0, 10, out _));
        Assert.AreEqual(0, tape.ReadPeaks("none", 0, 100, 10).Length);
        Assert.AreEqual(0L, tape.DurationMs);
        await tape.CloseAsync();
        Assert.ThrowsException<InvalidOperationException>(() => tape.AddTrack("late"));
    }

    [TestMethod]
    public async Task TranscribeSamplesReturnsTheFinalsAndLeavesTheTranscript()
    {
        await using var transcriber = await LoadAsync();
        transcriber.Start(AudioSource.Push);
        transcriber.Push(Voice(2 * SampleRate, seed: 1), SampleRate, 1);
        await Assert.ThrowsExceptionAsync<InvalidOperationException>(
            () => transcriber.TranscribeSamplesAsync(Voice(SampleRate, seed: 2)));
        await transcriber.StopAsync();
        int before = transcriber.Transcript.Finals.Count;

        string text = await transcriber.TranscribeSamplesAsync(Voice(3 * SampleRate + SampleRate / 2, seed: 2));
        // The scripted decoder answers "fake <n>" for a segment of n 100 ms blocks.
        StringAssert.StartsWith(text, "fake 10 fake 10 fake 10 fake ");
        Assert.AreEqual(4, text.Split("fake").Length - 1, $"Unexpected text: {text}");
        Assert.AreEqual(before, transcriber.Transcript.Finals.Count);
        Assert.AreEqual(string.Empty, await transcriber.TranscribeSamplesAsync(Array.Empty<short>()));

        // A cancelled request stops between seconds, and the transcriber can start again.
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await Assert.ThrowsExceptionAsync<TaskCanceledException>(
            () => transcriber.TranscribeSamplesAsync(Voice(SampleRate, seed: 3), cancellationToken: cancelled.Token));
        transcriber.Start(AudioSource.Push);
        await transcriber.StopAsync();
    }

    private static async Task<EchoirTranscriber> LoadAsync()
    {
        var transcriber = new EchoirTranscriber();
        await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);
        return transcriber;
    }

    // A voiced, speech-like signal (harmonics of a gliding pitch under a syllable-rate envelope),
    // which Opus codes the way it codes voice. seed shifts the pitch and the envelope.
    private static short[] Voice(int count, int seed)
    {
        var samples = new short[count];
        for (int i = 0; i < count; i++)
        {
            double t = (double)i / SampleRate;
            double phase = 2 * Math.PI * (130.0 + 20.0 * seed) * t - 50.0 / 0.4 * Math.Cos(2 * Math.PI * 0.4 * t + seed);
            double v = 0;
            for (int k = 1; k <= 10; k++)
            {
                v += Math.Sin(k * phase) / k;
            }
            double envelope = 0.55 + 0.45 * Math.Sin(2 * Math.PI * 3.1 * t + 0.7 * seed);
            samples[i] = (short)Math.Round(0.2 * envelope * v * 32767);
        }
        return samples;
    }

    private static double Correlation(short[] a, short[] b)
    {
        double ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < a.Length; i++)
        {
            ab += (double)a[i] * b[i];
            aa += (double)a[i] * a[i];
            bb += (double)b[i] * b[i];
        }
        return ab / Math.Sqrt(aa * bb);
    }

    // The samples of [fromMs, toMs) of a 16 kHz clip.
    private static short[] Slice(short[] samples, long fromMs, long toMs)
    {
        return samples[(int)(fromMs * SampleRate / 1000)..(int)(toMs * SampleRate / 1000)];
    }

    private sealed class TempFolder : IDisposable
    {
        public TempFolder()
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "echoir-tape-" + Guid.NewGuid().ToString("N"));
        }

        public string Path { get; }

        public void Dispose()
        {
            try
            {
                Directory.Delete(Path, recursive: true);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }
}
