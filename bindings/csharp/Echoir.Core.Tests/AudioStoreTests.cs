using System.Buffers.Binary;
using System.Text;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// The audio store, the level envelope and input mute. These run only against the
// ECHOIR_FAKE_BACKEND build. The scripted VAD reports all audio as speech in 1000 ms segments, so
// the segments here are whole seconds and the audio is compared sample for sample.
[TestClass]
[TestCategory("fake")]
public sealed class AudioStoreTests
{
    private const int SampleRate = 16000;
    private const int ToneHz = 440;

    private static readonly TranscriberOptions Retain10s = new() { RetainAudio = TimeSpan.FromSeconds(10) };

    [TestMethod]
    public async Task FinalSegmentReadsBackTheSamplesPushed()
    {
        await using var transcriber = await LoadAsync();
        short[] pushed = Noise(3 * SampleRate, seed: 1);

        transcriber.Start(AudioSource.Push, options: Retain10s);
        transcriber.Push(pushed, SampleRate, 1);
        await transcriber.StopAsync();

        Segment[] finals = transcriber.Transcript.Finals.ToArray();
        Assert.IsTrue(finals.Length > 0, "No FINAL was raised.");
        foreach (Segment segment in finals)
        {
            Assert.IsTrue(transcriber.TryReadAudio(segment, out short[] exact, padMs: 0),
                $"Segment {segment.Id} was not readable.");
            CollectionAssert.AreEqual(Slice(pushed, segment.StartMs, segment.EndMs), exact,
                $"Segment {segment.Id} differs from the audio pushed.");

            // The default pad adds up to 150 ms on each side, where the audio exists.
            long total = pushed.Length * 1000L / SampleRate;
            Assert.IsTrue(transcriber.TryReadAudio(segment, out short[] padded),
                $"Segment {segment.Id} was not readable with the pad.");
            CollectionAssert.AreEqual(
                Slice(pushed, Math.Max(0, segment.StartMs - 150), Math.Min(total, segment.EndMs + 150)),
                padded,
                $"Segment {segment.Id} with the pad differs from the audio pushed.");
        }
    }

    [TestMethod]
    public async Task StereoFloatAt48kIsStoredAsSixteenKhzMono()
    {
        await using var transcriber = await LoadAsync();
        const int rate = 48000;
        const int seconds = 2;
        const double amplitude = 0.5;
        var interleaved = new float[seconds * rate * 2];
        for (int i = 0; i < seconds * rate; i++)
        {
            float value = (float)(amplitude * Math.Sin(2 * Math.PI * ToneHz * i / rate));
            interleaved[2 * i] = value;
            interleaved[2 * i + 1] = value;
        }

        transcriber.Start(AudioSource.Push, options: Retain10s);
        transcriber.Push(interleaved, rate, 2);
        await transcriber.StopAsync();

        var whole = new Segment(0, 0, seconds * 1000L, string.Empty);
        Assert.IsTrue(transcriber.TryReadAudio(whole, out short[] samples, padMs: 0), "Audio was not readable.");
        double expectedLength = seconds * SampleRate;
        Assert.IsTrue(Math.Abs(samples.Length - expectedLength) <= expectedLength * 0.01,
            $"Read {samples.Length} samples, expected about {expectedLength}.");

        double sumOfSquares = 0;
        foreach (short sample in samples)
        {
            double value = sample / 32768.0;
            sumOfSquares += value * value;
        }
        double rms = Math.Sqrt(sumOfSquares / samples.Length);
        double expectedRms = amplitude / Math.Sqrt(2);
        Assert.IsTrue(Math.Abs(rms - expectedRms) <= expectedRms * 0.05,
            $"RMS is {rms:F4}, expected about {expectedRms:F4}.");
    }

    [TestMethod]
    public async Task AudioOlderThanRetainAudioIsNotReadable()
    {
        await using var transcriber = await LoadAsync();
        var options = new TranscriberOptions { RetainAudio = TimeSpan.FromSeconds(1) };

        transcriber.Start(AudioSource.Push, options: options);
        transcriber.Push(Noise(3 * SampleRate, seed: 2), SampleRate, 1);
        await transcriber.StopAsync();

        Segment first = transcriber.Transcript.Finals.OrderBy(segment => segment.StartMs).First();
        Assert.IsFalse(transcriber.TryReadAudio(first, out short[] samples), "The first segment is still readable.");
        Assert.AreEqual(0, samples.Length);
    }

    [TestMethod]
    public async Task WithoutRetainAudioOnlyThePeaksAreKept()
    {
        await using var transcriber = await LoadAsync();

        transcriber.Start(AudioSource.Push);
        transcriber.Push(Tone(SampleRate, 0.5), SampleRate, 1);

        Assert.IsFalse(transcriber.TryReadAudio(new Segment(1, 0, 1000, string.Empty), out short[] samples, padMs: 0));
        Assert.AreEqual(0, samples.Length);
        Assert.AreEqual(100, transcriber.ReadPeaks(0, 1000, 10).Length);
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task PeaksOfATonAreItsAmplitudeInEveryBucket()
    {
        await using var transcriber = await LoadAsync();
        const double amplitude = 0.5;

        transcriber.Start(AudioSource.Push);
        transcriber.Push(Tone(SampleRate, amplitude), SampleRate, 1);

        // A 10 ms bucket holds 4.4 cycles of the tone, so it reaches both peaks.
        Peak[] tenMs = transcriber.ReadPeaks(0, 1000, 10);
        Assert.AreEqual(100, tenMs.Length);
        foreach (Peak peak in tenMs)
        {
            AssertNear(peak.Min, -amplitude);
            AssertNear(peak.Max, amplitude);
        }

        Peak[] fiftyMs = transcriber.ReadPeaks(0, 1000, 50);
        Assert.AreEqual(20, fiftyMs.Length);
        foreach (Peak peak in fiftyMs)
        {
            AssertNear(peak.Min, -amplitude);
            AssertNear(peak.Max, amplitude);
        }

        // The envelope covers the whole session: its count in 10 ms steps is the length.
        Assert.AreEqual(100, transcriber.ReadPeaks(0, long.MaxValue, 10).Length);
        AssertNear(transcriber.Level, amplitude);
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task LevelIsZeroAfterSilenceAndWithNoAudio()
    {
        await using var transcriber = await LoadAsync();
        Assert.AreEqual(0f, transcriber.Level, "Level is not zero with no session.");

        transcriber.Start(AudioSource.Push);
        Assert.AreEqual(0f, transcriber.Level, "Level is not zero with no audio.");

        transcriber.Push(Tone(SampleRate, 0.5), SampleRate, 1);
        AssertNear(transcriber.Level, 0.5);

        transcriber.Push(new short[SampleRate / 10], SampleRate, 1);
        Assert.AreEqual(0f, transcriber.Level, "Level is not zero after 100 ms of silence.");
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task MutedInputIsStoredAsSilenceAndTheTimelineKeepsRunning()
    {
        await using var transcriber = await LoadAsync();
        short[] half = Tone(SampleRate / 2, 0.5);

        transcriber.Start(AudioSource.Push, options: Retain10s);
        transcriber.InputMuted = true;
        Assert.IsTrue(transcriber.InputMuted);
        transcriber.Push(half, SampleRate, 1);
        transcriber.InputMuted = false;
        Assert.IsFalse(transcriber.InputMuted);
        transcriber.Push(half, SampleRate, 1);
        await transcriber.StopAsync();

        Assert.AreEqual(1000L, transcriber.ReadPeaks(0, long.MaxValue, 10).Length * 10L,
            "The envelope does not cover all the audio pushed.");
        Assert.IsTrue(transcriber.TryReadAudio(new Segment(1, 0, 500, string.Empty), out short[] muted, padMs: 0));
        Assert.IsTrue(muted.All(sample => sample == 0), "The muted audio is not silence.");
        Assert.IsTrue(transcriber.TryReadAudio(new Segment(2, 500, 1000, string.Empty), out short[] live, padMs: 0));
        Assert.IsTrue(live.Any(sample => sample != 0), "The unmuted audio is silence.");
    }

    [TestMethod]
    public async Task InputMutedIsIgnoredWhileStoppedAndClearedByStop()
    {
        await using var transcriber = await LoadAsync();

        transcriber.InputMuted = true;
        Assert.IsFalse(transcriber.InputMuted, "Muting took effect with no session running.");

        transcriber.Start(AudioSource.Push);
        transcriber.InputMuted = true;
        await transcriber.StopAsync();
        Assert.IsFalse(transcriber.InputMuted, "Muting survived the stop.");

        transcriber.Start(AudioSource.Push);
        Assert.IsFalse(transcriber.InputMuted, "A new session started muted.");
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task ResetEmptiesThePeaksAndTheStore()
    {
        await using var transcriber = await LoadAsync();

        transcriber.Start(AudioSource.Push, options: Retain10s);
        transcriber.Push(Tone(SampleRate, 0.5), SampleRate, 1);
        Assert.AreEqual(100, transcriber.ReadPeaks(0, long.MaxValue, 10).Length);

        transcriber.Reset();

        Assert.AreEqual(0, transcriber.ReadPeaks(0, long.MaxValue, 10).Length);
        Assert.IsFalse(transcriber.TryReadAudio(new Segment(1, 0, 1000, string.Empty), out _, padMs: 0));
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task AudioIsReadableAfterStopUntilTheNextStart()
    {
        await using var transcriber = await LoadAsync();

        transcriber.Start(AudioSource.Push, options: Retain10s);
        transcriber.Push(Tone(2 * SampleRate, 0.5), SampleRate, 1);
        await transcriber.StopAsync();

        Segment last = transcriber.Transcript.Finals.OrderBy(segment => segment.StartMs).Last();
        Assert.IsTrue(transcriber.TryReadAudio(last, out short[] samples, padMs: 0), "Audio was not readable after stop.");
        Assert.AreEqual(SampleRate, samples.Length);
        Assert.AreEqual(200, transcriber.ReadPeaks(0, long.MaxValue, 10).Length);

        transcriber.Start(AudioSource.Push, options: Retain10s);
        Assert.AreEqual(0, transcriber.ReadPeaks(0, long.MaxValue, 10).Length, "Old audio survived the next Start.");
        Assert.IsFalse(transcriber.TryReadAudio(last, out _), "Old audio survived the next Start.");
        await transcriber.StopAsync();
    }

    [TestMethod]
    public void ToWavWritesACanonicalHeaderAndLittleEndianSamples()
    {
        short[] samples = { 1, -2, 0x1234 };

        byte[] wav = AudioClip.ToWav(samples);

        Assert.AreEqual(44 + 6, wav.Length);
        Assert.AreEqual("RIFF", Encoding.ASCII.GetString(wav, 0, 4));
        Assert.AreEqual(36 + 6u, BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(4)));
        Assert.AreEqual("WAVE", Encoding.ASCII.GetString(wav, 8, 4));
        Assert.AreEqual("fmt ", Encoding.ASCII.GetString(wav, 12, 4));
        Assert.AreEqual(16u, BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(16)));
        Assert.AreEqual((ushort)1, BinaryPrimitives.ReadUInt16LittleEndian(wav.AsSpan(20)), "Format is not PCM.");
        Assert.AreEqual((ushort)1, BinaryPrimitives.ReadUInt16LittleEndian(wav.AsSpan(22)), "Channels is not 1.");
        Assert.AreEqual(16000u, BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(24)));
        Assert.AreEqual(32000u, BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(28)));
        Assert.AreEqual((ushort)2, BinaryPrimitives.ReadUInt16LittleEndian(wav.AsSpan(32)));
        Assert.AreEqual((ushort)16, BinaryPrimitives.ReadUInt16LittleEndian(wav.AsSpan(34)));
        Assert.AreEqual("data", Encoding.ASCII.GetString(wav, 36, 4));
        Assert.AreEqual(6u, BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(40)));
        CollectionAssert.AreEqual(new byte[] { 0x01, 0x00, 0xFE, 0xFF, 0x34, 0x12 }, wav[44..]);

        Assert.AreEqual(96000u, BinaryPrimitives.ReadUInt32LittleEndian(AudioClip.ToWav(samples, 48000).AsSpan(28)));
    }

    private static async Task<EchoirTranscriber> LoadAsync()
    {
        var transcriber = new EchoirTranscriber();
        await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);
        return transcriber;
    }

    private static void AssertNear(float actual, double expected)
    {
        Assert.IsTrue(Math.Abs(actual - expected) <= 0.01,
            $"Expected {expected:F3} within 0.01, got {actual:F4}.");
    }

    // A sine of ToneHz at the given fraction of full scale.
    private static short[] Tone(int count, double amplitude)
    {
        var samples = new short[count];
        for (int i = 0; i < count; i++)
        {
            samples[i] = (short)Math.Round(amplitude * 32767 * Math.Sin(2 * Math.PI * ToneHz * i / SampleRate));
        }
        return samples;
    }

    private static short[] Noise(int count, int seed)
    {
        var random = new Random(seed);
        var samples = new short[count];
        for (int i = 0; i < count; i++)
        {
            samples[i] = (short)random.Next(short.MinValue, short.MaxValue);
        }
        return samples;
    }

    // The samples of [fromMs, toMs) of a 16 kHz clip.
    private static short[] Slice(short[] samples, long fromMs, long toMs)
    {
        return samples[(int)(fromMs * SampleRate / 1000)..(int)(toMs * SampleRate / 1000)];
    }
}
