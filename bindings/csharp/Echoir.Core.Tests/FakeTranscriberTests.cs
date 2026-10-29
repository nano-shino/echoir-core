using System.ComponentModel;
using System.Collections.Specialized;
using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// These run only against the ECHOIR_FAKE_BACKEND build, where the decoder and the VAD are
// scripted. Their output is not speech, so they check the event flow, not the words.
[TestClass]
[TestCategory("fake")]
public sealed class FakeTranscriberTests
{
    private const int SampleRate = 16000;

    [TestMethod]
    public async Task PartialIsReplacedByFinalWithTheSameId()
    {
        await using var transcriber = await LoadAsync();
        var log = new EventLog();
        WireLog(transcriber, log);

        transcriber.Start(AudioSource.Push);
        PushSeconds(transcriber, 3);
        Wait.Until(() => log.Snapshot().Any(e => e.StartsWith('P')), "a PARTIAL");
        await transcriber.StopAsync();

        string[] entries = log.Snapshot();
        string[] finals = entries.Where(e => e.StartsWith('F')).ToArray();
        Assert.IsTrue(finals.Length > 0, "No FINAL was raised.");
        // A segment that finishes in the tick it first appears in is sent as a FINAL with no
        // PARTIAL before it (session.cpp), so only the segments that had a PARTIAL are ordered.
        int replaced = 0;
        foreach (string final in finals)
        {
            string id = final[1..];
            int finalIndex = Array.IndexOf(entries, final);
            int partialIndex = Array.IndexOf(entries, "P" + id);
            if (partialIndex < 0)
            {
                continue;
            }
            Assert.IsTrue(partialIndex < finalIndex, $"FINAL {id} came before its PARTIAL.");
            replaced++;
        }
        Assert.IsTrue(replaced > 0, "No FINAL replaced a PARTIAL with the same id.");
        Assert.AreEqual(finals.Length, transcriber.Transcript.Finals.Count);
        Assert.IsNull(transcriber.Transcript.Partial);
    }

    [TestMethod]
    public async Task ClearEmptiesTheTranscript()
    {
        await using var transcriber = await LoadAsync();
        transcriber.Start(AudioSource.Push);
        PushSeconds(transcriber, 3);
        Wait.Until(() => transcriber.Transcript.Finals.Count > 0, "a FINAL");

        transcriber.Clear();

        Assert.AreEqual(0, transcriber.Transcript.Finals.Count);
        Assert.IsNull(transcriber.Transcript.Partial);
        await transcriber.StopAsync();
    }

    [TestMethod]
    public async Task HundredStartStopCyclesLeaveNoRunningSession()
    {
        await using var transcriber = await LoadAsync();
        var chunk = new float[SampleRate / 10];

        for (int cycle = 0; cycle < 100; cycle++)
        {
            transcriber.Start(AudioSource.Push);
            Assert.IsTrue(transcriber.IsRunning, $"Cycle {cycle} did not start.");
            transcriber.Push(chunk, SampleRate, 1);
            await transcriber.StopAsync();
            Assert.IsFalse(transcriber.IsRunning, $"Cycle {cycle} did not stop.");
        }
    }

    [TestMethod]
    public async Task EventsAreRaisedOnTheCapturedContext()
    {
        await using var transcriber = await LoadAsync();
        var pump = new PumpContext();
        var raisedOn = new List<int>();
        int pumpThread = Environment.CurrentManagedThreadId;
        transcriber.Transcript.PropertyChanged += (_, _) => raisedOn.Add(Environment.CurrentManagedThreadId);
        ((INotifyCollectionChanged)transcriber.Transcript.Finals).CollectionChanged +=
            (_, _) => raisedOn.Add(Environment.CurrentManagedThreadId);

        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(pump);
        try
        {
            transcriber.Start(AudioSource.Push);
            PushSeconds(transcriber, 3);
            pump.PumpUntil(() => transcriber.Transcript.Finals.Count > 0, TimeSpan.FromSeconds(10));

            Task stop = transcriber.StopAsync();
            pump.PumpUntil(() => stop.IsCompleted, TimeSpan.FromSeconds(10));
            stop.GetAwaiter().GetResult();
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }

        Assert.IsTrue(raisedOn.Count > 0, "Nothing was raised on the context.");
        Assert.IsTrue(raisedOn.All(thread => thread == pumpThread),
            "Events were raised on a thread other than the one that pumps the context.");
    }

    // Sustained load: 10,000 events within 60 s, with no FINAL lost. The fake decoder is fast, so
    // the audio is pushed faster than real time until the events arrive. FINALs are never dropped
    // and ids are not reused, so the FINAL ids must run 1, 2, 3, ... with no gap.
    [TestMethod]
    public async Task TenThousandEventsArriveInSixtySecondsWithoutLosingFinals()
    {
        await using var transcriber = await LoadAsync();
        int raised = 0;
        var finalIds = new List<long>();
        transcriber.Transcript.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(Transcript.Partial))
            {
                Interlocked.Increment(ref raised);
            }
        };
        ((INotifyCollectionChanged)transcriber.Transcript.Finals).CollectionChanged += (_, e) =>
        {
            if (e.NewItems is null)
            {
                return;
            }
            foreach (Segment segment in e.NewItems.Cast<Segment>())
            {
                finalIds.Add(segment.Id);
                Interlocked.Increment(ref raised);
            }
        };

        var frame = new float[SampleRate / 10];
        var clock = Stopwatch.StartNew();
        long pushedMs = 0;
        transcriber.Start(AudioSource.Push);
        while (Volatile.Read(ref raised) < 10_000 && clock.Elapsed < TimeSpan.FromSeconds(60))
        {
            // Paced to about 170 events a second, so the run lasts about a minute, as a live
            // session at a high event rate would. The pace is by events, not audio.
            if (Volatile.Read(ref raised) > clock.Elapsed.TotalSeconds * 170)
            {
                Thread.Sleep(2);
                continue;
            }
            transcriber.Push(frame, SampleRate, 1);
            pushedMs += 100;
        }
        double seconds = clock.Elapsed.TotalSeconds;
        await transcriber.StopAsync();

        int events = Volatile.Read(ref raised);
        Assert.IsTrue(events >= 10_000,
            $"Only {events} events in {seconds:F1} s ({pushedMs} ms of audio pushed).");
        Assert.IsTrue(seconds <= 60, $"The run took {seconds:F1} s.");
        Assert.AreEqual(finalIds.Count, transcriber.Transcript.Finals.Count);
        for (int i = 0; i < finalIds.Count; i++)
        {
            Assert.AreEqual(i + 1L, finalIds[i], $"FINAL ids have a gap at position {i}.");
        }
        Console.WriteLine($"{events} events, {finalIds.Count} FINALs in {seconds:F1} s ({pushedMs} ms of audio).");
    }

    [TestMethod]
    public void StartWithoutAModelIsRejected()
    {
        using var transcriber = new EchoirTranscriber();
        Assert.ThrowsException<InvalidOperationException>(() => transcriber.Start(AudioSource.Push));
    }

    private static async Task<EchoirTranscriber> LoadAsync()
    {
        var transcriber = new EchoirTranscriber();
        await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);
        return transcriber;
    }

    // Pushes silence in 100 ms frames. The scripted backend does not read the samples.
    private static void PushSeconds(EchoirTranscriber transcriber, int seconds)
    {
        var frame = new float[SampleRate / 10];
        for (int i = 0; i < seconds * 10; i++)
        {
            transcriber.Push(frame, SampleRate, 1);
        }
    }

    private static void WireLog(EchoirTranscriber transcriber, EventLog log)
    {
        transcriber.Transcript.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(Transcript.Partial) && transcriber.Transcript.Partial is { } partial)
            {
                log.Add("P" + partial.Id);
            }
        };
        ((INotifyCollectionChanged)transcriber.Transcript.Finals).CollectionChanged += (_, e) =>
        {
            if (e.NewItems is null)
            {
                return;
            }
            foreach (Segment segment in e.NewItems.Cast<Segment>())
            {
                log.Add("F" + segment.Id);
            }
        };
    }
}
