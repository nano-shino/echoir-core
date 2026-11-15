using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// These run only against the real build, with whisper-tiny-en from models/. They are reported as
// inconclusive when the model files are not there.
[TestClass]
[TestCategory("real")]
public sealed class RealTranscriberTests
{
    private const int SampleRate = 16000;

    [TestMethod]
    public async Task JfkWavTranscribesWithTheRealModel()
    {
        if (!TestPaths.WhisperModelsPresent)
        {
            Assert.Inconclusive("whisper-tiny-en and its VAD are not in models/.");
        }

        (float[] samples, int rate) = WavFile.ReadMono(TestPaths.JfkWav);
        Assert.AreEqual(SampleRate, rate, "jfk.wav is expected at 16 kHz.");

        await using var transcriber = new EchoirTranscriber();
        await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);
        transcriber.Start(AudioSource.Push);
        for (int offset = 0; offset < samples.Length; offset += SampleRate / 10)
        {
            int count = Math.Min(SampleRate / 10, samples.Length - offset);
            transcriber.Push(samples.AsSpan(offset, count), rate, 1);
        }
        await transcriber.StopAsync();

        string text = string.Join(" ", transcriber.Transcript.Finals.Select(segment => segment.Text))
            .ToLowerInvariant();
        StringAssert.Contains(text, "americans");
        StringAssert.Contains(text, "country");
    }

    [TestMethod]
    public async Task AContextOverTheLimitIsRefusedAndAFittingOneIsKept()
    {
        if (!TestPaths.WhisperModelsPresent)
        {
            Assert.Inconclusive("whisper-tiny-en and its VAD are not in models/.");
        }

        // Each word is at least one token, so 300 of them are over Whisper's 223-token prompt.
        string tooLong = string.Join(" ", Enumerable.Repeat("Kennedy", 300));
        await using var transcriber = new EchoirTranscriber();
        await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);

        var refused = Assert.ThrowsException<EchoirException>(
            () => transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Context = tooLong }));
        Assert.AreEqual(EchoirStatus.InvalidArgument, refused.Status);
        Assert.IsFalse(transcriber.IsRunning);

        transcriber.Start(AudioSource.Push, options: new TranscriberOptions { Context = "Vocabulary: Kennedy." });
        var whileRunning = Assert.ThrowsException<EchoirException>(() => transcriber.SetContext(tooLong));
        Assert.AreEqual(EchoirStatus.InvalidArgument, whileRunning.Status);
        StringAssert.Contains(whileRunning.Message, "223");

        // The refused context leaves the run going with the old one.
        (float[] samples, int rate) = WavFile.ReadMono(TestPaths.JfkWav);
        transcriber.SetContext("Vocabulary: Kennedy, Americans.");
        for (int offset = 0; offset < samples.Length; offset += SampleRate / 10)
        {
            int count = Math.Min(SampleRate / 10, samples.Length - offset);
            transcriber.Push(samples.AsSpan(offset, count), rate, 1);
        }
        await transcriber.StopAsync();
        string text = string.Join(" ", transcriber.Transcript.Finals.Select(segment => segment.Text))
            .ToLowerInvariant();
        StringAssert.Contains(text, "country");
    }
}
