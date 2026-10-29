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
}
