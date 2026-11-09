using System.Collections.Concurrent;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// The provisional translation of PARTIAL text in TranscriptTranslator, against a scripted
// translator, so no native library or model is needed. A PARTIAL repeated with the same times is
// no change to the transcript, so each partial here carries a new end time.
[TestClass]
public sealed class ProvisionalTranslationTests
{
    private sealed record Call(string Text, string? Context, string? Glossary);

    // Translates to "<target>:<text>". A call waits for Release when Hold is set.
    private sealed class Script : ITextTranslator
    {
        private readonly object _lock = new();
        private TaskCompletionSource _gate = Open();
        public ConcurrentQueue<Call> Calls { get; } = new();
        public bool IsProvisional { get; set; } = true;
        public Func<string, Exception?>? Fail { get; set; }

        bool ITextTranslator.Provisional => IsProvisional;

        private static TaskCompletionSource Open()
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

        public Task<Translation> TranslateAsync(
            string text, string targetLanguage, string? sourceLanguage = null, string? context = null,
            CancellationToken cancellationToken = default)
            => ((ITextTranslator)this).TranslateAsync(text, targetLanguage, sourceLanguage, context, null, cancellationToken);

        async Task<Translation> ITextTranslator.TranslateAsync(
            string text, string targetLanguage, string? sourceLanguage, string? context, string? glossary,
            CancellationToken cancellationToken)
        {
            Calls.Enqueue(new Call(text, context, glossary));
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

        public string[] Texts => Calls.Select(c => c.Text).ToArray();
    }

    private static long _tick;

    private static void Partial(Transcript transcript, long id, string text)
        => transcript.ApplyPartial(new Segment(id, id * 1000, id * 1000 + 100 + Interlocked.Increment(ref _tick), text));

    private static void Final(Transcript transcript, long id, string text)
        => transcript.ApplyFinal(new Segment(id, id * 1000, id * 1000 + 900, text));

    private static void Settle() => Thread.Sleep(150);

    private sealed class Probe
    {
        public ConcurrentQueue<ProvisionalTranslatedEventArgs> Provisional { get; } = new();
        public ConcurrentQueue<TranslatedEventArgs> Finals { get; } = new();
        public ConcurrentQueue<TranslationSkippedEventArgs> Skipped { get; } = new();

        public Probe(TranscriptTranslator translator)
        {
            translator.ProvisionalTranslated += (_, e) => Provisional.Enqueue(e);
            translator.Translated += (_, e) => Finals.Enqueue(e);
            translator.Skipped += (_, e) => Skipped.Enqueue(e);
        }

        public ProvisionalTranslatedEventArgs Last => Provisional.ToArray().Last();
    }

    // ----- splitting and comparing

    [TestMethod]
    public void SplitsAtSentenceEndsAndKeepsClosingQuotes()
    {
        CollectionAssert.AreEqual(
            new[] { "Hello there.", "How are you?", "Fine!", "And you" },
            SentenceText.Split("Hello there. How are you? Fine! And you"));
        CollectionAssert.AreEqual(
            new[] { "He said \"Stop.\"", "Then left." },
            SentenceText.Split("He said \"Stop.\" Then left."));
        CollectionAssert.AreEqual(new[] { "Wait…", "What was that" }, SentenceText.Split("Wait…What was that"));
        CollectionAssert.AreEqual(
            new[] { "今日は。", "明日も！", "さよなら" },
            SentenceText.Split("今日は。明日も！さよなら"));
        CollectionAssert.AreEqual(new[] { "「こんにちは。」", "あ" },
            SentenceText.Split("「こんにちは。」あ"));
    }

    [TestMethod]
    public void ADotInsideANumberOrWithoutASpaceDoesNotEndASentence()
    {
        CollectionAssert.AreEqual(
            new[] { "It cost 3.5 million." }, SentenceText.Split("It cost 3.5 million."));
        CollectionAssert.AreEqual(new[] { "See a.b" }, SentenceText.Split("See a.b"));
        CollectionAssert.AreEqual(new[] { "Really?!", "Yes" }, SentenceText.Split("Really?! Yes"));
        Assert.AreEqual(0, SentenceText.Split("   ").Count);
        // A piece with no letters does not stand alone.
        CollectionAssert.AreEqual(new[] { "... and then." }, SentenceText.Split("... and then."));
    }

    [TestMethod]
    public void TheKeyIgnoresCasePunctuationAndSpaces()
    {
        Assert.AreEqual(SentenceText.Key("Kai, come here!"), SentenceText.Key("kai come   here."));
        Assert.AreNotEqual(SentenceText.Key("Kai come here."), SentenceText.Key("Kai came here."));
        Assert.AreEqual("", SentenceText.Key(" ... "));
    }

    // ----- stable sentences and tails

    [TestMethod]
    public void ASentenceIsTranslatedOnceItIsCompleteAndStableInTwoPartials()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi", "en");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript, "mic");

        Partial(transcript, 1, "Hello there. How are");
        Settle();
        Assert.AreEqual(0, script.Calls.Count, "one partial is not agreement");

        Partial(transcript, 1, "Hello there. How are");
        Wait.Until(() => probe.Provisional.Count == 1, "the first provisional text");
        CollectionAssert.AreEqual(new[] { "Hello there." }, script.Texts);
        Assert.AreEqual("vi:Hello there.", probe.Last.StableText);
        Assert.AreEqual("", probe.Last.TailText);
        Assert.AreEqual(1, probe.Last.SegmentId);
        Assert.AreEqual("mic", probe.Last.Track);

        // More partials with the same sentence do not translate it again.
        Partial(transcript, 1, "Hello there. How are you");
        Wait.Until(() => script.Calls.Count == 2, "the tail");
        Partial(transcript, 1, "hello there! How are you doing");
        Settle();
        CollectionAssert.AreEqual(new[] { "Hello there.", "How are you" }, script.Texts);
        Assert.AreEqual("vi:Hello there.", probe.Last.StableText);
        Assert.AreEqual("vi:How are you", probe.Last.TailText);
    }

    [TestMethod]
    public void ASentenceThatChangesBetweenPartialsIsNotStable()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "I think so. Maybe");
        Partial(transcript, 1, "I thank so. Maybe");
        Partial(transcript, 1, "I think so. Maybe");
        Settle();
        Assert.AreEqual(0, script.Calls.Count);
    }

    [TestMethod]
    public void ATranslatedSentenceIsKeptAcrossAForcedCutThatChangesTheSegmentId()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript, "system");

        Partial(transcript, 1, "First sentence here. Second one");
        Partial(transcript, 1, "First sentence here. Second one coming");
        Wait.Until(() => probe.Provisional.Count >= 1, "the first sentence");
        Final(transcript, 1, "First sentence here.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
        int calls = script.Calls.Count;

        // The speech after the cut comes back in the next segment, with the same sentence.
        Partial(transcript, 2, "first sentence here! Second one coming now");
        Wait.Until(() => probe.Provisional.ToArray().Any(p => p.SegmentId == 2), "text for segment 2");
        ProvisionalTranslatedEventArgs shown = probe.Provisional.ToArray().First(p => p.SegmentId == 2);
        Assert.AreEqual("vi:First sentence here.", shown.StableText);
        Partial(transcript, 2, "first sentence here! Second one coming now please");
        Settle();
        Assert.IsFalse(
            script.Texts.Skip(calls).Any(t => SentenceText.Key(t) == SentenceText.Key("First sentence here.")),
            "the cached sentence is not translated again");
    }

    [TestMethod]
    public void TheTailNeedsThreeWordsThenTwoMoreForEachNewTranslation()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "one two");
        Settle();
        Assert.AreEqual(0, script.Calls.Count);

        Partial(transcript, 1, "one two three");
        Wait.Until(() => script.Calls.Count == 1, "the tail at 3 words");

        Partial(transcript, 1, "one two three four");
        Settle();
        Assert.AreEqual(1, script.Calls.Count, "1 word more is not enough");

        Partial(transcript, 1, "one two three four five");
        Wait.Until(() => script.Calls.Count == 2, "the tail at 5 words");
        CollectionAssert.AreEqual(new[] { "one two three", "one two three four five" }, script.Texts);

        // A new sentence starts the count again.
        Partial(transcript, 1, "one two three four five. six seven");
        Settle();
        Assert.AreEqual(2, script.Calls.Count);
        Partial(transcript, 1, "one two three four five. six seven eight");
        Wait.Until(() => script.Texts.Contains("six seven eight"), "the new tail");
    }

    [TestMethod]
    public void TheNewestTailWinsWhileACallIsInFlight()
    {
        var script = new Script();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "one two three");
        Wait.Until(() => script.Calls.Count == 1, "the first tail to start");
        Partial(transcript, 1, "one two three four five");
        Partial(transcript, 1, "one two three four five six seven");
        script.Release();
        Wait.Until(() => script.Calls.Count == 2, "the next tail");
        Settle();
        CollectionAssert.AreEqual(new[] { "one two three", "one two three four five six seven" }, script.Texts);
        Assert.AreEqual("vi:one two three four five six seven", probe.Last.TailText);
    }

    // ----- order and the drop rule

    [TestMethod]
    public void FinalsGoBeforeSentencesAndSentencesBeforeTails()
    {
        var script = new Script();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "Earlier line.");
        Wait.Until(() => script.Calls.Count == 1, "the first FINAL to start");
        Partial(transcript, 2, "Aa bb cc. Dd ee ff gg");
        Partial(transcript, 2, "Aa bb cc. Dd ee ff gg");
        Final(transcript, 3, "Later FINAL line.");
        script.Release();
        Wait.Until(() => script.Calls.Count == 4, "four calls");
        CollectionAssert.AreEqual(
            new[] { "Earlier line.", "Later FINAL line.", "Aa bb cc.", "Dd ee ff gg" }, script.Texts);
    }

    [TestMethod]
    public void OnlyFinalsCountForTheDropRule()
    {
        var script = new Script();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "line 1");
        Wait.Until(() => script.Calls.Count == 1, "the first FINAL to start");
        // Many stable sentences wait; none is a dropped FINAL.
        Partial(transcript, 2, "One a. Two b. Three c. Four d. Five e. Six f. Seven");
        Partial(transcript, 2, "One a. Two b. Three c. Four d. Five e. Six f. Seven");
        Final(transcript, 2, "line 2");
        Final(transcript, 3, "line 3");
        Final(transcript, 4, "line 4");
        Settle();
        Assert.AreEqual(0, probe.Skipped.Count);
        Final(transcript, 5, "line 5");
        Wait.Until(() => probe.Skipped.Count == 1, "one dropped FINAL");
        Assert.AreEqual(2, probe.Skipped.Single().SegmentId);
        script.Release();
        Wait.Until(() => probe.Finals.Count == 4, "four translated FINALs");
    }

    [TestMethod]
    public void StaleSentenceWorkIsDiscarded()
    {
        var script = new Script();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "line 1");
        Wait.Until(() => script.Calls.Count == 1, "the FINAL to start");
        Partial(transcript, 2, "Old words here. More");
        Partial(transcript, 2, "Old words here. More");
        // The speech model rewrote its partial: the queued sentence is gone from it.
        Partial(transcript, 2, "Completely different now. More");
        script.Release();
        Settle();
        CollectionAssert.DoesNotContain(script.Texts, "Old words here.");
    }

    [TestMethod]
    public void AFinalEndsTheProvisionalWorkOfItsSegment()
    {
        var script = new Script();
        script.Hold();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "line 1");
        Wait.Until(() => script.Calls.Count == 1, "the FINAL to start");
        Partial(transcript, 2, "Pending one. Pending two. Tail words here");
        Partial(transcript, 2, "Pending one. Pending two. Tail words here");
        Final(transcript, 2, "Pending one. Pending two. Tail words here.");
        script.Release();
        Wait.Until(() => probe.Finals.Count == 2, "both FINALs");
        Settle();
        // The FINAL is translated whole, and the sentences that waited are not.
        CollectionAssert.AreEqual(
            new[] { "line 1", "Pending one. Pending two. Tail words here." }, script.Texts);
        Assert.AreEqual("vi:Pending one. Pending two. Tail words here.", probe.Finals.ToArray().Last().Text);
        Assert.IsFalse(probe.Provisional.ToArray().Any(p => p.SegmentId == 2));
    }

    [TestMethod]
    public void AFinalIsTranslatedWholeWithItsTwoEarlierFinalsAsContext()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "First line.");
        Final(transcript, 2, "Second line.");
        Partial(transcript, 3, "Third. Fourth part here");
        Partial(transcript, 3, "Third. Fourth part here");
        Final(transcript, 3, "Third. Fourth part here.");
        Wait.Until(() => probe.Finals.Count == 3, "three FINALs");
        Call last = script.Calls.ToArray().Last(c => c.Text == "Third. Fourth part here.");
        Assert.AreEqual("First line.\nSecond line.", last.Context);
    }

    // ----- hold rule

    [TestMethod]
    public void ATranslationThatCoversLessSpeechIsHeldBack()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "Aa bb cc. Dd ee ff gg hh");
        Partial(transcript, 1, "Aa bb cc. Dd ee ff gg hh");
        Wait.Until(() => script.Calls.Count == 2, "the sentence and the tail");
        Wait.Until(() => probe.Last.TailText == "vi:Dd ee ff gg hh", "the tail text");
        int shown = probe.Provisional.Count;

        // The speech model splits its partial again: the new tail is short, and the sentence
        // before it is not translated yet, so what would show is less than what shows.
        Partial(transcript, 1, "Aa bb cc. Dd ee. ff gg hh");
        Wait.Until(() => script.Texts.Contains("ff gg hh"), "the short tail");
        Settle();
        Assert.AreEqual(shown, probe.Provisional.Count, "no event for a shorter cover");

        // Once the middle sentence is translated, the cover is whole again.
        Partial(transcript, 1, "Aa bb cc. Dd ee. ff gg hh");
        Wait.Until(() => probe.Provisional.Count > shown, "the whole text again");
        Assert.AreEqual("vi:Aa bb cc. vi:Dd ee.", probe.Last.StableText);
        Assert.AreEqual("vi:ff gg hh", probe.Last.TailText);
    }

    // ----- when it is on

    [TestMethod]
    public void ATranslatorThatIsNotProvisionalOnlyTranslatesFinals()
    {
        var script = new Script { IsProvisional = false };
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);
        Assert.IsFalse(translator.ProvisionalActive);

        Partial(transcript, 1, "Aa bb cc. Dd ee ff gg");
        Partial(transcript, 1, "Aa bb cc. Dd ee ff gg");
        Settle();
        Assert.AreEqual(0, script.Calls.Count);
        Final(transcript, 1, "Aa bb cc. Dd ee ff gg.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
        Assert.AreEqual(0, probe.Provisional.Count);
    }

    [TestMethod]
    public void MoreThanTwoFollowedTranscriptsMeanFinalsOnly()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var a = new Transcript();
        var b = new Transcript();
        var c = new Transcript();
        translator.Follow(a, "a");
        translator.Follow(b, "b");
        Assert.IsTrue(translator.ProvisionalActive);
        translator.Follow(c, "c");
        Assert.IsFalse(translator.ProvisionalActive);

        Partial(a, 1, "Aa bb cc. Dd ee ff gg");
        Partial(a, 1, "Aa bb cc. Dd ee ff gg");
        Settle();
        Assert.AreEqual(0, script.Calls.Count);

        translator.Unfollow(c);
        Assert.IsTrue(translator.ProvisionalActive);
        Partial(a, 1, "Aa bb cc. Dd ee ff gg");
        Partial(a, 1, "Aa bb cc. Dd ee ff gg");
        Wait.Until(() => script.Calls.Count >= 2, "provisional work after one is unfollowed");
        Final(a, 1, "Aa bb cc. Dd ee ff gg.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
    }

    [TestMethod]
    public void TwoTranscriptsKeepTheirOwnSentencesAndContext()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var mic = new Transcript();
        var system = new Transcript();
        translator.Follow(mic, "mic");
        translator.Follow(system, "system");

        Partial(mic, 1, "Mic words one. Mic tail");
        Partial(system, 1, "System words one. System tail");
        Partial(mic, 1, "Mic words one. Mic tail");
        Partial(system, 1, "System words one. System tail");
        Wait.Until(
            () => probe.Provisional.ToArray().Any(p => p.Track == "mic" && p.StableText.Length > 0)
                && probe.Provisional.ToArray().Any(p => p.Track == "system" && p.StableText.Length > 0),
            "text for both tracks");
        Assert.AreEqual("vi:Mic words one.", probe.Provisional.ToArray().First(p => p.Track == "mic").StableText);
        Assert.AreEqual("vi:System words one.", probe.Provisional.ToArray().First(p => p.Track == "system").StableText);
    }

    [TestMethod]
    public void ASentenceGetsTheTwoSentencesBeforeItAsContext()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Final(transcript, 1, "Older one. Older two.");
        Wait.Until(() => script.Calls.Count == 1, "the FINAL");
        Partial(transcript, 2, "Now one. Now two. Now three. Now four");
        Partial(transcript, 2, "Now one. Now two. Now three. Now four");
        Wait.Until(() => script.Calls.Count == 4, "three sentences");
        Call[] calls = script.Calls.ToArray();
        Assert.AreEqual("Now one.", calls[1].Text);
        Assert.AreEqual("Older one.\nOlder two.", calls[1].Context);
        Assert.AreEqual("Older two.\nNow one.", calls[2].Context);
        Assert.AreEqual("Now one.\nNow two.", calls[3].Context);
    }

    [TestMethod]
    public void AClearForgetsTheKeptSentences()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "Same sentence here. Tail");
        Partial(transcript, 1, "Same sentence here. Tail");
        Wait.Until(() => script.Calls.Count == 1, "the sentence");
        transcript.ApplyCleared();

        Partial(transcript, 1, "Same sentence here. Tail");
        Partial(transcript, 1, "Same sentence here. Tail");
        Wait.Until(() => script.Calls.Count == 2, "the sentence again after a clear");
    }

    // ----- glossary

    [TestMethod]
    public void TheGlossaryGoesWithEveryCall()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi")
        {
            Glossary = new[]
            {
                new GlossaryEntry("Kai", "Kaie"),
                new GlossaryEntry("a\tb", "x\ny"),
                new GlossaryEntry(" ", "blank"),
                new GlossaryEntry("Shin", ""),
            },
        };
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "Kai is here. Tail words now");
        Partial(transcript, 1, "Kai is here. Tail words now");
        Wait.Until(() => script.Calls.Count == 2, "sentence and tail");
        Final(transcript, 1, "Kai is here. Tail words now.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
        Assert.IsTrue(script.Calls.All(c => c.Glossary == "Kai\tKaie\na b\tx y\n"));
        Assert.AreEqual(3, script.Calls.Count);
    }

    [TestMethod]
    public void ChangingTheGlossaryForgetsTheKeptTranslations()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "vi");
        var transcript = new Transcript();
        translator.Follow(transcript);

        Partial(transcript, 1, "Kai is here. Tail");
        Partial(transcript, 1, "Kai is here. Tail");
        Wait.Until(() => script.Calls.Count == 1, "the sentence");
        Assert.IsNull(script.Calls.Single().Glossary);

        translator.Glossary = new[] { new GlossaryEntry("Kai", "Kaie") };
        Partial(transcript, 1, "Kai is here. Tail");
        Partial(transcript, 1, "Kai is here. Tail");
        Wait.Until(() => script.Calls.Count == 2, "the sentence with the new glossary");
        Assert.AreEqual("Kai\tKaie\n", script.Calls.ToArray()[1].Glossary);
    }

    [TestMethod]
    public void AFailedSentenceIsNotTriedAgainAndFinalsStillWork()
    {
        var script = new Script { Fail = t => t == "Bad one." ? new InvalidOperationException("no") : null };
        using var translator = new TranscriptTranslator(script, "vi");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        for (int i = 0; i < 4; i++)
        {
            Partial(transcript, 1, "Bad one. Tail");
            Thread.Sleep(30);
        }
        Settle();
        Assert.AreEqual(1, script.Texts.Count(t => t == "Bad one."));
        Final(transcript, 1, "Bad one. Tail.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
    }

    [TestMethod]
    public void JapaneseSentencesAreJoinedWithoutASpace()
    {
        var script = new Script();
        using var translator = new TranscriptTranslator(script, "ja");
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);

        string text = "one two three. four five six. seven";
        Partial(transcript, 1, text);
        Partial(transcript, 1, text);
        Wait.Until(
            () => probe.Provisional.ToArray().Any(p => p.StableText == "ja:one two three.ja:four five six."),
            "two sentences joined");
    }

    [TestMethod]
    public void ATranslatorWrittenBeforeGlossariesStillWorks()
    {
        // The scripted translator of the older tests implements only the five-argument method.
        var old = new OldTranslator();
        using var translator = new TranscriptTranslator(old, "vi")
        {
            Glossary = new[] { new GlossaryEntry("Kai", "Kaie") },
        };
        var probe = new Probe(translator);
        var transcript = new Transcript();
        translator.Follow(transcript);
        Assert.IsFalse(translator.ProvisionalActive);
        Final(transcript, 1, "Kai is here.");
        Wait.Until(() => probe.Finals.Count == 1, "the FINAL");
        Assert.AreEqual("vi:Kai is here.", probe.Finals.Single().Text);
    }

    private sealed class OldTranslator : ITextTranslator
    {
        public Task<Translation> TranslateAsync(
            string text, string targetLanguage, string? sourceLanguage = null, string? context = null,
            CancellationToken cancellationToken = default)
            => Task.FromResult(new Translation(targetLanguage + ":" + text, false, 1, 1, 1, TimeSpan.Zero, TimeSpan.Zero));
    }

    [TestMethod]
    public void TheGlossaryTextIsOneTabbedPairPerLine()
    {
        Assert.IsNull(TranslationGlossary.Format(null));
        Assert.IsNull(TranslationGlossary.Format(Array.Empty<GlossaryEntry>()));
        Assert.AreEqual(
            "A\tB\nC D\tE F\n",
            TranslationGlossary.Format(new[] { new GlossaryEntry(" A ", "B"), new GlossaryEntry("C\tD", "E\r\nF") }));
    }
}
