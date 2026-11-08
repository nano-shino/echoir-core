using System.Collections.Concurrent;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// TranscriptTranslator against a scripted translator, so they need no native library or model.
[TestClass]
public sealed class TranscriptTranslatorTests
{
    // Translates to "<target>:<text>". A call waits for Release when Hold is set, so a test decides
    // when it finishes.
    private sealed class ScriptedTranslator : ITextTranslator
    {
        private readonly object _lock = new();
        private TaskCompletionSource _gate = Done();
        public ConcurrentQueue<(string Text, string Target, string? Source, string? Context)> Calls { get; } = new();
        public Func<string, Exception?>? Fail { get; set; }

        private static TaskCompletionSource Done()
        {
            var source = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            source.SetResult();
            return source;
        }

        public void Hold()
        {
            lock (_lock)
            {
                _gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            }
        }

        public void Release()
        {
            lock (_lock)
            {
                _gate.TrySetResult();
            }
        }

        public async Task<Translation> TranslateAsync(
            string text, string targetLanguage, string? sourceLanguage = null, string? context = null,
            CancellationToken cancellationToken = default)
        {
            Calls.Enqueue((text, targetLanguage, sourceLanguage, context));
            Task gate;
            lock (_lock)
            {
                gate = _gate.Task;
            }
            await gate.WaitAsync(cancellationToken);
            if (Fail?.Invoke(text) is { } error)
            {
                throw error;
            }
            return new Translation(targetLanguage + ":" + text, false, 1, 1, 1, TimeSpan.Zero, TimeSpan.Zero);
        }
    }

    private static Segment Seg(long id, string text) => new(id, id * 1000, id * 1000 + 900, text);

    // The followed transcripts need Finals to change, which only EchoirTranscriber does through its
    // notifications. The tests drive Transcript's internal Apply methods.
    private static void AddFinal(Transcript transcript, Segment segment) => transcript.ApplyFinal(segment);

    private static ConcurrentQueue<TranslatedEventArgs> Collect(TranscriptTranslator translator)
    {
        var seen = new ConcurrentQueue<TranslatedEventArgs>();
        translator.Translated += (_, e) => seen.Enqueue(e);
        return seen;
    }

    [TestMethod]
    public void TranslatesEachFinalWithTheTwoEarlierOnesAsContext()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "vi", "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        translator.Follow(transcript, "mic");

        AddFinal(transcript, Seg(1, "one"));
        AddFinal(transcript, Seg(2, "two"));
        AddFinal(transcript, Seg(3, "three"));
        Wait.Until(() => seen.Count == 3, "three translations");

        TranslatedEventArgs[] results = seen.ToArray();
        CollectionAssert.AreEqual(new[] { 1L, 2L, 3L }, results.Select(r => r.SegmentId).ToArray());
        Assert.AreEqual("vi:three", results[2].Text);
        Assert.AreEqual("mic", results[2].Track);
        Assert.AreSame(transcript, results[2].Transcript);

        var calls = script.Calls.ToArray();
        Assert.IsNull(calls[0].Context);
        Assert.AreEqual("one", calls[1].Context);
        Assert.AreEqual("one\ntwo", calls[2].Context);
        Assert.AreEqual("en", calls[2].Source);
        Assert.AreEqual("vi", calls[2].Target);

        AddFinal(transcript, Seg(4, "four"));
        Wait.Until(() => seen.Count == 4, "a fourth translation");
        Assert.AreEqual("two\nthree", script.Calls.ToArray()[3].Context);
    }

    [TestMethod]
    public void PartialsAndBlankFinalsAreNotTranslated()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        transcript.ApplyPartial(Seg(1, "partial text"));
        AddFinal(transcript, Seg(1, "  "));
        AddFinal(transcript, Seg(2, "real"));
        Wait.Until(() => seen.Count == 1, "one translation");
        Thread.Sleep(100);
        Assert.AreEqual(1, script.Calls.Count);
        Assert.AreEqual("real", script.Calls.Single().Text);
    }

    [TestMethod]
    public void SegmentsAlreadyInTheTranscriptAreNotTranslated()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "en");
        var transcript = new Transcript();
        AddFinal(transcript, Seg(1, "old"));
        translator.Follow(transcript);
        Thread.Sleep(100);
        Assert.AreEqual(0, script.Calls.Count);
    }

    [TestMethod]
    public void PastThreeWaitingTheOldestIsDroppedAndReported()
    {
        var script = new ScriptedTranslator();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var skipped = new ConcurrentQueue<TranslationSkippedEventArgs>();
        translator.Skipped += (_, e) => skipped.Enqueue(e);
        var transcript = new Transcript();
        translator.Follow(transcript);

        AddFinal(transcript, Seg(1, "a"));
        Wait.Until(() => script.Calls.Count == 1, "the first call to start");
        for (long id = 2; id <= 6; id++)
        {
            AddFinal(transcript, Seg(id, "line " + id));
        }
        Wait.Until(() => skipped.Count == 2, "two dropped lines");
        script.Release();
        Wait.Until(() => seen.Count == 4, "four translations");

        CollectionAssert.AreEqual(new[] { 2L, 3L }, skipped.Select(s => s.SegmentId).ToArray());
        Assert.IsTrue(skipped.All(s => s.Error is null));
        CollectionAssert.AreEqual(new[] { 1L, 4L, 5L, 6L }, seen.Select(r => r.SegmentId).ToArray());
    }

    [TestMethod]
    public void AClearDropsWhatWaitsAndTheResultInFlight()
    {
        var script = new ScriptedTranslator();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        AddFinal(transcript, Seg(1, "a"));
        Wait.Until(() => script.Calls.Count == 1, "the first call to start");
        AddFinal(transcript, Seg(2, "b"));
        transcript.ApplyCleared();
        script.Release();

        // Only a line added after the clear is translated.
        AddFinal(transcript, Seg(3, "c"));
        Wait.Until(() => seen.Count == 1, "the line after the clear");
        Thread.Sleep(100);
        Assert.AreEqual(1, seen.Count);
        Assert.AreEqual(3, seen.Single().SegmentId);
        Assert.IsNull(script.Calls.ToArray().Last().Context);
    }

    [TestMethod]
    public void ResetDropsWhatWaitsAndTheResultInFlight()
    {
        var script = new ScriptedTranslator();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        AddFinal(transcript, Seg(1, "a"));
        Wait.Until(() => script.Calls.Count == 1, "the first call to start");
        AddFinal(transcript, Seg(2, "b"));
        translator.Reset(transcript);
        script.Release();
        AddFinal(transcript, Seg(3, "c"));
        Wait.Until(() => seen.Count == 1, "the line after the reset");
        Thread.Sleep(100);
        Assert.AreEqual(3, seen.Single().SegmentId);
    }

    [TestMethod]
    public void AnIdThatGoesBackIsANewSessionAndDropsTheOldWork()
    {
        var script = new ScriptedTranslator();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        AddFinal(transcript, Seg(5, "old five"));
        Wait.Until(() => script.Calls.Count == 1, "the first call to start");
        AddFinal(transcript, Seg(6, "old six"));
        // A new session on the same transcript starts its ids again.
        AddFinal(transcript, Seg(1, "new one"));
        script.Release();
        Wait.Until(() => seen.Count == 1, "the new session's line");
        Thread.Sleep(100);
        Assert.AreEqual("en:new one", seen.Single().Text);
    }

    [TestMethod]
    public void TwoTranscriptsKeepTheirOwnContextAndLimits()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "ja");
        var seen = Collect(translator);
        var mic = new Transcript();
        var system = new Transcript();
        translator.Follow(mic, "mic");
        translator.Follow(system, "system");

        AddFinal(mic, Seg(1, "mic one"));
        AddFinal(system, Seg(1, "system one"));
        AddFinal(mic, Seg(2, "mic two"));
        Wait.Until(() => seen.Count == 3, "three translations");

        var byKey = seen.ToDictionary(r => (r.Track, r.SegmentId));
        Assert.AreEqual("ja:system one", byKey[("system", 1)].Text);
        var calls = script.Calls.ToArray();
        Assert.IsNull(calls.Single(c => c.Text == "system one").Context);
        Assert.AreEqual("mic one", calls.Single(c => c.Text == "mic two").Context);
    }

    [TestMethod]
    public void UnfollowStopsTranslatingATranscript()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "en");
        var transcript = new Transcript();
        translator.Follow(transcript);
        translator.Unfollow(transcript);
        AddFinal(transcript, Seg(1, "a"));
        Thread.Sleep(100);
        Assert.AreEqual(0, script.Calls.Count);
        Assert.ThrowsException<InvalidOperationException>(() => translator.Submit(transcript, Seg(1, "a")));
    }

    [TestMethod]
    public void FollowingTwiceIsRefused()
    {
        using var translator = new TranscriptTranslator(new ScriptedTranslator(), "en");
        var transcript = new Transcript();
        translator.Follow(transcript);
        Assert.ThrowsException<InvalidOperationException>(() => translator.Follow(transcript));
    }

    [TestMethod]
    public void AFailedTranslationIsReportedAndTheNextLineStillTranslates()
    {
        var script = new ScriptedTranslator
        {
            Fail = text => text == "bad" ? new InvalidOperationException("model failed") : null,
        };
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var skipped = new ConcurrentQueue<TranslationSkippedEventArgs>();
        translator.Skipped += (_, e) => skipped.Enqueue(e);
        var transcript = new Transcript();
        translator.Follow(transcript);

        AddFinal(transcript, Seg(1, "bad"));
        AddFinal(transcript, Seg(2, "good"));
        Wait.Until(() => seen.Count == 1 && skipped.Count == 1, "an error and a translation");
        Assert.AreEqual("model failed", skipped.Single().Error);
        Assert.AreEqual(1L, skipped.Single().SegmentId);
        Assert.AreEqual(2L, seen.Single().SegmentId);
    }

    [TestMethod]
    public void SubmitTranslatesASegmentAgainWithItsContext()
    {
        var script = new ScriptedTranslator();
        using var translator = new TranscriptTranslator(script, "en");
        var seen = Collect(translator);
        var transcript = new Transcript();
        AddFinal(transcript, Seg(1, "one"));
        AddFinal(transcript, Seg(2, "two"));
        translator.Follow(transcript);

        translator.Submit(transcript, transcript.Finals[1]);
        Wait.Until(() => seen.Count == 1, "the submitted line");
        Assert.AreEqual("one", script.Calls.Single().Context);
        Assert.AreEqual(2L, seen.Single().SegmentId);
    }

    [TestMethod]
    public void EventsAreRaisedOnTheContextOfTheFollow()
    {
        var script = new ScriptedTranslator();
        var context = new PumpContext();
        TranscriptTranslator translator;
        var transcript = new Transcript();
        var threadIds = new ConcurrentQueue<int>();
        int pumpThread = Environment.CurrentManagedThreadId;

        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(context);
        try
        {
            translator = new TranscriptTranslator(script, "en");
            translator.Translated += (_, _) => threadIds.Enqueue(Environment.CurrentManagedThreadId);
            translator.Follow(transcript);
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }

        using (translator)
        {
            AddFinal(transcript, Seg(1, "a"));
            Wait.Until(() => script.Calls.Count == 1, "the call");
            Thread.Sleep(100);
            Assert.AreEqual(0, threadIds.Count, "Nothing may be raised before the context pumps.");
            context.PumpUntil(() => threadIds.Count == 1, TimeSpan.FromSeconds(5));
            Assert.AreEqual(pumpThread, threadIds.Single());
        }
    }

    [TestMethod]
    public void AResultThatWaitsOnTheContextIsDroppedByAResetBeforeItRuns()
    {
        var script = new ScriptedTranslator();
        var context = new PumpContext();
        var transcript = new Transcript();
        var seen = new ConcurrentQueue<long>();
        TranscriptTranslator translator;

        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(context);
        try
        {
            translator = new TranscriptTranslator(script, "en");
            translator.Translated += (_, e) => seen.Enqueue(e.SegmentId);
            translator.Follow(transcript);
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }

        using (translator)
        {
            AddFinal(transcript, Seg(1, "a"));
            Wait.Until(() => script.Calls.Count == 1, "the call");
            Thread.Sleep(100);
            // The translation is done and its raise is posted. The reset comes first on the context.
            translator.Reset(transcript);
            AddFinal(transcript, Seg(2, "b"));
            // The raise of segment 1 was posted first and is stale; the one of segment 2 follows it.
            context.PumpUntil(() => seen.Count == 1, TimeSpan.FromSeconds(5));
            Thread.Sleep(100);
            CollectionAssert.AreEqual(new[] { 2L }, seen.ToArray());
        }
    }
}
