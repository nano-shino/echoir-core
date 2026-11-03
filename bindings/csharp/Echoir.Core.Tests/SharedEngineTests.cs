using System.Text.RegularExpressions;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// Several transcribers on one EchoirEngine. These run only against the ECHOIR_FAKE_BACKEND build,
// whose scripted decoder answers "fake <n>" for n blocks of 100 ms, so the FINALs of a transcriber
// add up to the audio it was given.
[TestClass]
[TestCategory("fake")]
public sealed partial class SharedEngineTests
{
    private const int SampleRate = 16000;

    [TestMethod]
    public async Task TwoTranscribersOnOneEngineEachTranscribeTheirOwnAudio()
    {
        using EchoirEngine engine = await LoadEngineAsync();
        Assert.AreEqual("whisper-tiny-en", engine.ModelId);
        await using var first = new EchoirTranscriber(engine);
        await using var second = new EchoirTranscriber(engine);
        Assert.IsTrue(first.IsLoaded);
        Assert.IsTrue(second.IsLoaded);

        first.Start(AudioSource.Push);
        second.Start(AudioSource.Push);
        Task pushFirst = Task.Run(() => PushTenths(first, 30));
        Task pushSecond = Task.Run(() => PushTenths(second, 15));
        await Task.WhenAll(pushFirst, pushSecond);
        await first.StopAsync();
        await second.StopAsync();

        // Every block a transcriber was given is in exactly one of its own FINALs.
        AssertBlocks(30, first);
        AssertBlocks(15, second);
    }

    [TestMethod]
    public async Task TranscribersOnOneEngineStartTogetherOnTheTape()
    {
        using EchoirEngine engine = await LoadEngineAsync();
        await using var tape = EchoirTape.Create(null);
        tape.AddTrack("system", "System audio");
        await using var mic = new EchoirTranscriber(engine);
        await using var system = new EchoirTranscriber(engine);

        // An earlier run leaves a second on the tape.
        mic.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        PushTenths(mic, 10);
        await mic.StopAsync();

        mic.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape });
        system.Start(AudioSource.Push, options: new TranscriberOptions { Tape = tape, TapeTrack = "system" });
        Assert.AreEqual(1000L, mic.TapeOffsetMs);
        Assert.AreEqual(mic.TapeOffsetMs, system.TapeOffsetMs);
        PushTenths(mic, 10);
        PushTenths(system, 10);
        await mic.StopAsync();
        await system.StopAsync();
        Assert.AreEqual(2000L, tape.DurationMs);
    }

    [TestMethod]
    public async Task DisposingTheEngineFirstLeavesARunningTranscriberRunning()
    {
        EchoirEngine engine = await LoadEngineAsync();
        await using var transcriber = new EchoirTranscriber(engine);
        transcriber.Start(AudioSource.Push);
        PushTenths(transcriber, 10);

        engine.Dispose();
        engine.Dispose();
        PushTenths(transcriber, 20);
        Wait.Until(() => transcriber.Transcript.Finals.Count > 0, "a FINAL after the engine was disposed");
        await transcriber.StopAsync();
        AssertBlocks(30, transcriber);

        // No new session starts on a disposed engine.
        Assert.IsFalse(transcriber.IsLoaded);
        Assert.ThrowsException<ObjectDisposedException>(() => transcriber.Start(AudioSource.Push));
        Assert.ThrowsException<ObjectDisposedException>(() => new EchoirTranscriber(engine));
    }

    [TestMethod]
    public async Task DisposingATranscriberLeavesTheSharedEngineLoaded()
    {
        using EchoirEngine engine = await LoadEngineAsync();
        var first = new EchoirTranscriber(engine);
        first.Start(AudioSource.Push);
        PushTenths(first, 10);
        await first.DisposeAsync();

        await using var second = new EchoirTranscriber(engine);
        second.Start(AudioSource.Push);
        PushTenths(second, 20);
        await second.StopAsync();
        AssertBlocks(20, second);
    }

    [TestMethod]
    public async Task LoadAsyncIsRefusedOnASharedEngine()
    {
        using EchoirEngine engine = await LoadEngineAsync();
        await using var transcriber = new EchoirTranscriber(engine);
        await Assert.ThrowsExceptionAsync<InvalidOperationException>(
            () => transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir));
        Assert.IsTrue(transcriber.IsLoaded);
    }

    private static Task<EchoirEngine> LoadEngineAsync() =>
        EchoirEngine.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);

    // Pushes silence in 100 ms frames. The scripted backend does not read the samples.
    private static void PushTenths(EchoirTranscriber transcriber, int tenths)
    {
        var frame = new float[SampleRate / 10];
        for (int i = 0; i < tenths; i++)
        {
            transcriber.Push(frame, SampleRate, 1);
        }
    }

    // Every pushed block, give or take one, is in the FINALs of the transcriber it was pushed to.
    private static void AssertBlocks(int expected, EchoirTranscriber transcriber)
    {
        int blocks = Blocks(transcriber);
        Assert.IsTrue(Math.Abs(expected - blocks) <= 1, $"The FINALs cover {blocks} blocks, not {expected}.");
    }

    // The blocks of 100 ms that the FINALs of a transcriber cover, from their "fake <n>" texts.
    private static int Blocks(EchoirTranscriber transcriber) =>
        transcriber.Transcript.Finals.Sum(segment => int.Parse(FakeText().Match(segment.Text).Groups[1].Value));

    [GeneratedRegex(@"^fake (\d+)$")]
    private static partial Regex FakeText();
}
