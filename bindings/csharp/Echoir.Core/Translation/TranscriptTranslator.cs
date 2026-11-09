using System.Collections.Specialized;
using System.ComponentModel;

namespace Echoir.Core;

// Raised when the translation of a FINAL segment is ready. Track and Segment.Id key the line.
public sealed class TranslatedEventArgs : EventArgs
{
    public TranslatedEventArgs(Transcript transcript, string track, Segment segment, Translation translation)
    {
        Transcript = transcript;
        Track = track;
        Segment = segment;
        Translation = translation;
    }

    // The transcript the segment belongs to, and the name it was followed under.
    public Transcript Transcript { get; }
    public string Track { get; }

    // The source segment, as it was in Transcript.Finals.
    public Segment Segment { get; }
    public long SegmentId => Segment.Id;

    public Translation Translation { get; }
    public string Text => Translation.Text;

    // True when the model stopped at its generation limit, so Text may end mid-sentence.
    public bool Capped => Translation.Capped;
}

// Raised when a segment will not be translated: it waited behind too many others, or its
// translation failed.
public sealed class TranslationSkippedEventArgs : EventArgs
{
    public TranslationSkippedEventArgs(Transcript transcript, string track, Segment segment, string? error)
    {
        Transcript = transcript;
        Track = track;
        Segment = segment;
        Error = error;
    }

    public Transcript Transcript { get; }
    public string Track { get; }
    public Segment Segment { get; }
    public long SegmentId => Segment.Id;

    // The failure, or null when the segment was dropped because translating fell behind.
    public string? Error { get; }
}

// Raised when a provisional translation of the segment in progress is ready: the translation of
// what has been said so far, before the FINAL is translated. Track and SegmentId key the line, as
// for Translated; each event replaces the one before it for the same track and segment.
//
// StableText is the translation of the finished sentences of the PARTIAL, which seldom change.
// TailText is the translation of the unfinished last part, which changes often, so a host can draw
// it apart (dimmer, for example). Either may be empty, never both. Draw StableText followed by
// TailText, joined by a space (none for Japanese and Chinese). The FINAL's translation (Translated)
// replaces all of it; a host drops provisional text when the FINAL arrives, when its transcript is
// cleared and after a reset.
public sealed class ProvisionalTranslatedEventArgs : EventArgs
{
    public ProvisionalTranslatedEventArgs(
        Transcript transcript, string track, long segmentId, string stableText, string tailText)
    {
        Transcript = transcript;
        Track = track;
        SegmentId = segmentId;
        StableText = stableText;
        TailText = tailText;
    }

    public Transcript Transcript { get; }
    public string Track { get; }

    // The id of the PARTIAL segment, which its FINAL shares.
    public long SegmentId { get; }

    public string StableText { get; }
    public string TailText { get; }
}

// Follows the FINAL segments of one or more transcripts and translates them through one translator.
//
// One translation is in flight at a time, for all followed transcripts. While it runs, the segments
// that arrive wait in order; once more than MaxWaiting of them wait, across all followed
// transcripts, the oldest waiting segment overall is dropped and stays untranslated (Skipped), so
// the translations show the present rather than a backlog. A segment is translated with the two
// FINAL segments before it in its own transcript as context, source text only, so a late or
// dropped translation changes nothing for the others.
//
// Provisional translation. When the translator is Provisional (the small default model) and at
// most MaxProvisionalTracks transcripts are followed, the segment in progress is translated too,
// so a translation appears about a second after the words rather than after the FINAL. The newest
// PARTIAL is split into sentences. A sentence that has text after it and is the same in two
// PARTIALs in a row (ignoring case, punctuation and spaces) is translated once, and the translation
// is kept per transcript under that text, so it survives a forced cut that moves speech into the
// next segment. The unfinished last part is translated speculatively once it has 3 words, and
// again after it has grown by 2 words; the newest wins. The results are raised as
// ProvisionalTranslated, one event per change, except that one covering less of the speech than
// the one before is held back. The FINAL is still translated whole, with the context above, and
// replaces the provisional text. The translator runs one call at a time, in this order: FINAL
// segments, stable sentences, tails, taking turns between transcripts. The drop rule above counts
// FINAL segments only; sentence and tail work that has gone stale is discarded. Context for a
// sentence or a tail: the two sentences before it in its transcript. With a translator that is
// not Provisional, or more transcripts followed, PARTIALs are never translated.
//
// A clear of a followed transcript, Reset, and a restart of its segment ids (a new session on the
// same transcript) drop what waits for it, and the result of a translation that was already running
// is dropped when it returns. Translated and Skipped are raised on the SynchronizationContext that
// was current when the transcript was followed, in the order the segments were translated; with no
// context, on the worker's thread, where an exception from a handler is reported to
// System.Diagnostics.Trace and the worker goes on. With a context, an exception reaches the
// context, as with any posted call.
//
// Follow, Unfollow, Reset and Submit are called on the transcript's context, like any read of its
// Finals. The translator is not owned: dispose it separately, after this. Dispose does not wait for
// a translation that is running: a call into a translator supplied by the host may still be in
// progress after Dispose returns, and its result is dropped.
public sealed class TranscriptTranslator : IDisposable
{
    // Segments, across all followed transcripts, that may wait behind the one being translated.
    public const int MaxWaiting = 3;

    // Earlier FINAL segments of the same transcript passed to the translator as context.
    public const int ContextLines = 2;

    // Provisional translation needs no more followed transcripts than this.
    public const int MaxProvisionalTracks = 2;

    // Words an unfinished sentence needs before it is translated, and how many more before it is
    // translated again.
    public const int TailMinWords = 3;
    public const int TailGrowWords = 2;

    // Earlier sentences passed as context for a sentence or a tail.
    public const int SentenceContext = 2;

    private const int CacheLimit = 64;
    private const int HistoryLimit = 6;

    private readonly ITextTranslator _translator;
    private readonly string _target;
    private readonly string? _source;

    // Guards the queue, the follows and the flags. Never held while user code or a translation runs.
    private readonly object _gate = new();
    private readonly List<Tracked> _follows = new();
    private readonly LinkedList<Work> _queue = new();
    private readonly LinkedList<Work> _sentences = new();
    private string? _glossaryText;
    private IReadOnlyList<GlossaryEntry> _glossary = Array.Empty<GlossaryEntry>();
    private int _turn;
    private readonly SemaphoreSlim _wake = new(0);
    private readonly CancellationTokenSource _stop = new();
    private Task? _worker;
    private bool _disposed;

    public TranscriptTranslator(ITextTranslator translator, string targetLanguage, string? sourceLanguage = null)
    {
        ArgumentNullException.ThrowIfNull(translator);
        ArgumentException.ThrowIfNullOrEmpty(targetLanguage);
        _translator = translator;
        _target = targetLanguage;
        _source = sourceLanguage;
    }

    // The language the segments are translated into.
    public string TargetLanguage => _target;

    // Raised when a translation is ready.
    public event EventHandler<TranslatedEventArgs>? Translated;

    // Raised for a segment that was dropped or whose translation failed.
    public event EventHandler<TranslationSkippedEventArgs>? Skipped;

    // Raised when the translation of the segment in progress changes (see the class remarks).
    // Handlers run where Translated does, except that with no synchronization context they run on
    // the thread that changed the transcript or finished the translation.
    public event EventHandler<ProvisionalTranslatedEventArgs>? ProvisionalTranslated;

    // The user's terms for TargetLanguage, passed to the translator with every call. The
    // translator uses the pairs whose term occurs in the text or its context. Setting it forgets
    // the provisional translations kept so far, which were made without the new terms.
    public IReadOnlyList<GlossaryEntry> Glossary
    {
        get
        {
            lock (_gate)
            {
                return _glossary;
            }
        }
        set
        {
            lock (_gate)
            {
                _glossary = value ?? Array.Empty<GlossaryEntry>();
                _glossaryText = TranslationGlossary.Format(_glossary);
                foreach (Tracked follow in _follows)
                {
                    follow.ForgetTranslations();
                }
                _sentences.Clear();
                foreach (Tracked follow in _follows)
                {
                    follow.Queued.Clear();
                }
            }
        }
    }

    // True when PARTIALs are being translated now: the translator is Provisional and at most
    // MaxProvisionalTracks transcripts are followed.
    public bool ProvisionalActive
    {
        get
        {
            lock (_gate)
            {
                return ProvisionalActiveLocked();
            }
        }
    }

    private bool ProvisionalActiveLocked()
        => !_disposed && _translator.Provisional && _follows.Count <= MaxProvisionalTracks;

    // Starts translating the FINAL segments added to transcript.Finals from now on; the segments
    // already there are not translated. track names the transcript in the events. Following a
    // transcript twice throws InvalidOperationException.
    public void Follow(Transcript transcript, string track = "")
    {
        ArgumentNullException.ThrowIfNull(transcript);
        ArgumentNullException.ThrowIfNull(track);
        Tracked follow;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_follows.Any(f => f.Transcript == transcript))
            {
                throw new InvalidOperationException("The transcript is already followed.");
            }
            follow = new Tracked(transcript, track, SynchronizationContext.Current);
            _follows.Add(follow);
        }
        follow.Handler = (_, e) => OnFinalsChanged(follow, e);
        follow.PartialHandler = (_, e) =>
        {
            if (e.PropertyName == nameof(Transcript.Partial))
            {
                OnPartialChanged(follow);
            }
        };
        ((INotifyCollectionChanged)transcript.Finals).CollectionChanged += follow.Handler;
        transcript.PropertyChanged += follow.PartialHandler;
    }

    // Stops following a transcript and drops what waits for it.
    public void Unfollow(Transcript transcript)
    {
        ArgumentNullException.ThrowIfNull(transcript);
        Tracked? follow;
        lock (_gate)
        {
            follow = _follows.FirstOrDefault(f => f.Transcript == transcript);
            if (follow is null)
            {
                return;
            }
            _follows.Remove(follow);
            DropAllLocked(follow);
        }
        ((INotifyCollectionChanged)transcript.Finals).CollectionChanged -= follow.Handler;
        transcript.PropertyChanged -= follow.PartialHandler;
    }

    // Drops the segments waiting for a followed transcript (all followed transcripts with null) and
    // the result of the translation running for it. Call it where the timeline restarts, such as
    // after EchoirTranscriber.Reset(); a clear of the transcript needs no call.
    public void Reset(Transcript? transcript = null)
    {
        lock (_gate)
        {
            foreach (Tracked follow in _follows)
            {
                if (transcript is null || follow.Transcript == transcript)
                {
                    DropAllLocked(follow);
                }
            }
        }
    }

    // Queues a segment of a followed transcript for translation, such as one decoded again. The
    // two segments before it in transcript.Finals are its context; a segment that is not in Finals
    // has none. Throws InvalidOperationException when the transcript is not followed.
    public void Submit(Transcript transcript, Segment segment)
    {
        ArgumentNullException.ThrowIfNull(transcript);
        ArgumentNullException.ThrowIfNull(segment);
        Tracked follow;
        lock (_gate)
        {
            follow = _follows.FirstOrDefault(f => f.Transcript == transcript)
                ?? throw new InvalidOperationException("The transcript is not followed.");
        }
        int index = -1;
        for (int i = 0; i < transcript.Finals.Count; i++)
        {
            if (ReferenceEquals(transcript.Finals[i], segment))
            {
                index = i;
                break;
            }
        }
        Enqueue(follow, segment, index);
    }

    // Stops the worker. A translation that is running is not waited for, and its result is dropped.
    public void Dispose()
    {
        List<Tracked> follows;
        bool noWorker;
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            follows = new List<Tracked>(_follows);
            _follows.Clear();
            _queue.Clear();
            _sentences.Clear();
            noWorker = _worker is null;
        }
        foreach (Tracked follow in follows)
        {
            ((INotifyCollectionChanged)follow.Transcript.Finals).CollectionChanged -= follow.Handler;
            follow.Transcript.PropertyChanged -= follow.PartialHandler;
        }
        if (noWorker)
        {
            _stop.Dispose();
            _wake.Dispose();
            return;
        }
        // The worker disposes both when it ends.
        try
        {
            _stop.Cancel();
            _wake.Release();
        }
        catch (ObjectDisposedException)
        {
            // The worker had already ended.
        }
    }

    private void OnFinalsChanged(Tracked follow, NotifyCollectionChangedEventArgs e)
    {
        switch (e.Action)
        {
            case NotifyCollectionChangedAction.Reset:
                lock (_gate)
                {
                    DropAllLocked(follow);
                    follow.LastId = long.MinValue;
                }
                break;
            case NotifyCollectionChangedAction.Add:
            case NotifyCollectionChangedAction.Replace:
                if (e.NewItems is not null)
                {
                    int index = e.NewStartingIndex;
                    foreach (Segment segment in e.NewItems)
                    {
                        // Ids grow within a session. One that does not is a new session on the same
                        // transcript, so what waits from the old one is stale.
                        lock (_gate)
                        {
                            if (segment.Id <= follow.LastId)
                            {
                                DropAllLocked(follow);
                            }
                            follow.LastId = segment.Id;
                            follow.NoteFinal(segment.Text, HistoryLimit);
                        }
                        Enqueue(follow, segment, index);
                        index = index < 0 ? index : index + 1;
                    }
                }
                break;
        }
    }

    // Queues a segment whose place in Finals is index (or -1), with its context.
    private void Enqueue(Tracked follow, Segment segment, int index)
    {
        if (string.IsNullOrWhiteSpace(segment.Text))
        {
            return;
        }
        string? context = null;
        if (index > 0)
        {
            var lines = new List<string>(ContextLines);
            for (int i = Math.Max(0, index - ContextLines); i < index; i++)
            {
                lines.Add(follow.Transcript.Finals[i].Text);
            }
            context = string.Join('\n', lines);
        }

        Work? dropped = null;
        lock (_gate)
        {
            if (_disposed || !_follows.Contains(follow))
            {
                return;
            }
            _queue.AddLast(Work.ForFinal(follow, segment, context, follow.Generation));
            if (_queue.Count > MaxWaiting && _queue.First is { } oldest)
            {
                dropped = oldest.Value;
                _queue.RemoveFirst();
            }
            _worker ??= Task.Run(RunAsync);
        }
        try
        {
            _wake.Release();
        }
        catch (ObjectDisposedException)
        {
            // Disposed meanwhile: nothing is translated any more.
        }
        if (dropped is not null)
        {
            Tracked owner = dropped.Follow;
            Segment skipped = dropped.Segment!;
            Raise(owner, dropped.Generation, () =>
                Skipped?.Invoke(this, new TranslationSkippedEventArgs(owner.Transcript, owner.Track, skipped, null)));
        }
    }

    // Removes what waits for the follow and makes a result still to come from it stale.
    private void DropAllLocked(Tracked follow)
    {
        follow.Generation++;
        RemoveWork(_queue, follow);
        RemoveWork(_sentences, follow);
        follow.ResetProvisional();
    }

    private static void RemoveWork(LinkedList<Work> list, Tracked follow)
    {
        for (LinkedListNode<Work>? node = list.First; node is not null;)
        {
            LinkedListNode<Work>? next = node.Next;
            if (node.Value.Follow == follow)
            {
                list.Remove(node);
            }
            node = next;
        }
    }

    private async Task RunAsync()
    {
        try
        {
            await RunLoopAsync().ConfigureAwait(false);
        }
        finally
        {
            _stop.Dispose();
            _wake.Dispose();
        }
    }

    private async Task RunLoopAsync()
    {
        CancellationToken token = _stop.Token;
        while (!token.IsCancellationRequested)
        {
            try
            {
                await _wake.WaitAsync(token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            // Work is picked again after every call, so a FINAL segment that arrived meanwhile
            // goes before the sentences and tails that were waiting.
            while (!token.IsCancellationRequested)
            {
                Work? work;
                string? glossary;
                lock (_gate)
                {
                    work = PickLocked();
                    glossary = _glossaryText;
                }
                if (work is null)
                {
                    break;
                }
                if (!await RunWorkAsync(work, glossary, token).ConfigureAwait(false))
                {
                    return;
                }
            }
        }
    }

    // Takes the next job: FINAL segments, then stable sentences, then tails. Under the gate.
    private Work? PickLocked()
    {
        if (_queue.First is { } first)
        {
            _queue.RemoveFirst();
            return first.Value;
        }
        if (!ProvisionalActiveLocked())
        {
            _sentences.Clear();
            return null;
        }
        while (_sentences.First is { } node)
        {
            _sentences.RemoveFirst();
            Work sentence = node.Value;
            Tracked owner = sentence.Follow;
            if (owner.Generation != sentence.Generation || !_follows.Contains(owner) || !owner.Wants(sentence.Key!))
            {
                owner.Queued.Remove(sentence.Key!);
                continue;
            }
            return sentence;
        }
        for (int step = 0; step < _follows.Count; step++)
        {
            int index = (_turn + step) % _follows.Count;
            Tracked owner = _follows[index];
            if (owner.NextTail() is { } tail)
            {
                _turn = index + 1;
                return Work.ForTail(owner, tail, owner.ContextBefore(tail.Index, SentenceContext), owner.Generation);
            }
        }
        return null;
    }

    // Translates one job and raises its result. False when the translator was cancelled.
    private async Task<bool> RunWorkAsync(Work work, string? glossary, CancellationToken token)
    {
        Translation? translation = null;
        string? error = null;
        try
        {
            translation = await _translator
                .TranslateAsync(work.Text, _target, _source, work.Context, glossary, token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
            return false;
        }
        catch (Exception ex)
        {
            error = ex.Message;
        }

        Tracked follow = work.Follow;
        if (work.Kind == WorkKind.Final)
        {
            Segment segment = work.Segment!;
            if (translation is not null)
            {
                Translation ready = translation;
                Raise(follow, work.Generation, () =>
                    Translated?.Invoke(this, new TranslatedEventArgs(follow.Transcript, follow.Track, segment, ready)));
            }
            else
            {
                Raise(follow, work.Generation, () =>
                    Skipped?.Invoke(this, new TranslationSkippedEventArgs(follow.Transcript, follow.Track, segment, error)));
            }
            return true;
        }

        // A sentence or a tail. A failure is not reported: the FINAL's translation is the one that
        // counts, and a failed sentence is not tried again.
        ProvisionalTranslatedEventArgs? update = null;
        lock (_gate)
        {
            if (follow.Generation == work.Generation)
            {
                if (work.Kind == WorkKind.Sentence)
                {
                    follow.Queued.Remove(work.Key!);
                    if (translation is not null)
                    {
                        follow.Remember(work.Key!, translation.Text, CacheLimit);
                    }
                    else
                    {
                        follow.Failed.Add(work.Key!);
                    }
                }
                else if (translation is not null)
                {
                    follow.TailTranslation = new TailTranslation(work.SegmentId, work.Index, work.Text, translation.Text);
                }
                update = ComposeLocked(follow);
            }
        }
        RaiseProvisional(follow, update);
        return true;
    }

    // Builds the provisional text of the follow's segment in progress from the kept translations,
    // or null when there is nothing new to show. A translation that covers less of the speech than
    // the one shown is held back. Under the gate.
    private ProvisionalTranslatedEventArgs? ComposeLocked(Tracked follow)
    {
        if (follow.Pieces.Count == 0)
        {
            return null;
        }
        var stable = new List<string>();
        string tail = string.Empty;
        double covered = 0;
        int complete = follow.Pieces.Count - 1;
        bool allKnown = true;
        TailTranslation? speculative =
            follow.TailTranslation is { } t && t.SegmentId == follow.PartialId ? t : null;
        for (int i = 0; i < complete; i++)
        {
            if (follow.Cache.TryGetValue(follow.Keys[i], out string? text))
            {
                stable.Add(text);
                covered += SentenceText.Words(follow.Pieces[i]);
                continue;
            }
            // A speculative translation made when this sentence was still the tail holds its place.
            if (speculative is not null && speculative.Index == i)
            {
                tail = speculative.Text;
                covered += SentenceText.Words(speculative.Source);
            }
            allKnown = false;
            break;
        }
        if (allKnown && speculative is not null && speculative.Index == complete)
        {
            tail = speculative.Text;
            covered += SentenceText.Words(speculative.Source);
        }

        string stableText = SentenceText.Join(stable, _target);
        if ((stableText.Length == 0 && tail.Length == 0)
            || covered < follow.ShownCover
            || (stableText == follow.ShownStable && tail == follow.ShownTail))
        {
            return null;
        }
        follow.ShownCover = covered;
        follow.ShownStable = stableText;
        follow.ShownTail = tail;
        return new ProvisionalTranslatedEventArgs(follow.Transcript, follow.Track, follow.PartialId, stableText, tail);
    }

    // Reads the follow's PARTIAL: finds the sentences that became stable, queues them, notes the
    // tail, and shows what is already translated of it.
    private void OnPartialChanged(Tracked follow)
    {
        Segment? partial = follow.Transcript.Partial;
        ProvisionalTranslatedEventArgs? update = null;
        lock (_gate)
        {
            if (_disposed || !_follows.Contains(follow))
            {
                return;
            }
            if (partial is null || string.IsNullOrWhiteSpace(partial.Text) || !ProvisionalActiveLocked())
            {
                follow.EndPartial();
                RemoveWork(_sentences, follow);
                follow.Queued.Clear();
                return;
            }
            if (partial.Id < follow.PartialId)
            {
                // Ids went back: a new session on the same transcript.
                follow.ResetProvisional();
                RemoveWork(_sentences, follow);
            }
            List<string> pieces = SentenceText.Split(partial.Text);
            follow.SetPartial(partial.Id, pieces);
            for (int i = 0; i < pieces.Count - 1; i++)
            {
                if (follow.IsStable(i) && follow.Wants(follow.Keys[i]) && follow.Queued.Add(follow.Keys[i]))
                {
                    _sentences.AddLast(Work.ForSentence(
                        follow, partial.Id, i, pieces[i], follow.Keys[i], follow.ContextBefore(i, SentenceContext), follow.Generation));
                }
            }
            follow.PrepareTail();
            update = ComposeLocked(follow);
            _worker ??= Task.Run(RunAsync);
        }
        try
        {
            _wake.Release();
        }
        catch (ObjectDisposedException)
        {
            // Disposed meanwhile.
        }
        RaiseProvisional(follow, update);
    }

    private void RaiseProvisional(Tracked follow, ProvisionalTranslatedEventArgs? update)
    {
        if (update is null)
        {
            return;
        }
        int generation;
        lock (_gate)
        {
            generation = follow.Generation;
        }
        // A provisional text is stale once the FINAL of its segment has been seen.
        Raise(follow, generation, () => ProvisionalTranslated?.Invoke(this, update), () => update.SegmentId > follow.LastId);
    }

    // Runs the raise on the follow's context, if the follow was not reset or stopped meanwhile. The
    // check runs on that context, which is also where Reset is called, so a result cannot arrive
    // after the reset that made it stale.
    private void Raise(Tracked follow, int generation, Action raise, Func<bool>? stillValid = null)
    {
        void Run()
        {
            lock (_gate)
            {
                if (_disposed || follow.Generation != generation || !_follows.Contains(follow)
                    || (stillValid is not null && !stillValid()))
                {
                    return;
                }
            }
            raise();
        }

        // On the worker's thread a throwing handler must not end the worker, which would stop all
        // translating without a sign. It is reported to Trace instead.
        void RunOnWorker()
        {
            try
            {
                Run();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.TraceError("A TranscriptTranslator event handler threw: " + ex);
            }
        }

        if (follow.Context is null)
        {
            RunOnWorker();
        }
        else
        {
            follow.Context.Post(static state => ((Action)state!)(), (Action)Run);
        }
    }

    private sealed class Tracked
    {
        public Tracked(Transcript transcript, string track, SynchronizationContext? context)
        {
            Transcript = transcript;
            Track = track;
            Context = context;
        }

        public Transcript Transcript { get; }
        public string Track { get; }
        public SynchronizationContext? Context { get; }
        public NotifyCollectionChangedEventHandler Handler { get; set; } = static (_, _) => { };
        public PropertyChangedEventHandler PartialHandler { get; set; } = static (_, _) => { };

        // Bumped by every drop. Under the gate.
        public int Generation { get; set; }

        // The id of the newest segment seen. Under the gate.
        public long LastId { get; set; } = long.MinValue;

        // Provisional translation, all under the gate. Cache holds sentence translations by key,
        // oldest first in _order. Queued and Failed are keys. _history holds the sentences of the
        // FINAL segments, for context.
        public Dictionary<string, string> Cache { get; } = new();
        private readonly Queue<string> _order = new();
        public HashSet<string> Queued { get; } = new();
        public HashSet<string> Failed { get; } = new();
        private readonly List<string> _history = new();

        // The sentences of the newest PARTIAL (the last is the tail), their keys, and the keys of
        // the PARTIAL before it.
        public long PartialId { get; private set; } = long.MinValue;
        public List<string> Pieces { get; private set; } = new();
        public List<string> Keys { get; private set; } = new();
        private List<string>? _previousKeys;

        private TailCandidate? _tail;
        private TailCandidate? _tailDone;
        public TailTranslation? TailTranslation { get; set; }

        // What the host was last shown for the segment, and how much speech it covers.
        public double ShownCover { get; set; }
        public string? ShownStable { get; set; }
        public string? ShownTail { get; set; }

        public void SetPartial(long id, List<string> pieces)
        {
            if (id != PartialId)
            {
                _previousKeys = null;
                TailTranslation = null;
                _tailDone = null;
                ShownCover = 0;
                ShownStable = null;
                ShownTail = null;
                PartialId = id;
            }
            else
            {
                _previousKeys = Keys;
            }
            Pieces = pieces;
            Keys = pieces.Select(SentenceText.Key).ToList();
        }

        // True when sentence i is complete and was the same in the PARTIAL before.
        public bool IsStable(int i)
            => _previousKeys is not null && i < _previousKeys.Count - 1 && _previousKeys[i] == Keys[i];

        // True when the sentence has a key worth translating, no translation yet, and is in the
        // newest PARTIAL.
        public bool Wants(string key)
            => key.Length > 0 && !Cache.ContainsKey(key) && !Failed.Contains(key) && Keys.Contains(key);

        public void PrepareTail()
        {
            _tail = Pieces.Count > 0 && Keys[^1].Length > 0
                ? new TailCandidate(PartialId, Pieces.Count - 1, Pieces[^1])
                : null;
        }

        // The tail if it should be translated now: 3 words, and 2 more than at its last
        // translation when that was the same sentence of the same segment.
        public TailCandidate? NextTail()
        {
            TailCandidate? tail = _tail;
            if (tail is null || SentenceText.Words(tail.Text) < TailMinWords)
            {
                return null;
            }
            if (_tailDone is { } done && done.SegmentId == tail.SegmentId && done.Index == tail.Index
                && SentenceText.Words(tail.Text) - SentenceText.Words(done.Text) < TailGrowWords)
            {
                return null;
            }
            _tailDone = tail;
            return tail;
        }

        // The sentences before sentence index: from the FINAL segments, then the PARTIAL.
        public string? ContextBefore(int index, int count)
        {
            var before = new List<string>(_history);
            before.AddRange(Pieces.Take(Math.Min(index, Pieces.Count)));
            string[] last = before.Skip(Math.Max(0, before.Count - count)).ToArray();
            return last.Length == 0 ? null : string.Join('\n', last);
        }

        public void Remember(string key, string translation, int limit)
        {
            if (Cache.TryAdd(key, translation))
            {
                _order.Enqueue(key);
                while (_order.Count > limit)
                {
                    Cache.Remove(_order.Dequeue());
                }
            }
        }

        public void NoteFinal(string text, int limit)
        {
            _history.AddRange(SentenceText.Split(text));
            if (_history.Count > limit)
            {
                _history.RemoveRange(0, _history.Count - limit);
            }
        }

        // The PARTIAL is gone (its FINAL came, or it was cleared): its state is stale, the kept
        // translations are not.
        public void EndPartial()
        {
            Pieces = new List<string>();
            Keys = new List<string>();
            _previousKeys = null;
            _tail = null;
            _tailDone = null;
            TailTranslation = null;
            ShownCover = 0;
            ShownStable = null;
            ShownTail = null;
        }

        public void ForgetTranslations()
        {
            Cache.Clear();
            _order.Clear();
            Failed.Clear();
            TailTranslation = null;
            _tailDone = null;
            ShownCover = 0;
            ShownStable = null;
            ShownTail = null;
        }

        // A clear, a reset or a new session: nothing of the old text is kept.
        public void ResetProvisional()
        {
            EndPartial();
            ForgetTranslations();
            Queued.Clear();
            _history.Clear();
            PartialId = long.MinValue;
        }
    }

    private sealed record TailCandidate(long SegmentId, int Index, string Text);

    private sealed record TailTranslation(long SegmentId, int Index, string Source, string Text);

    private enum WorkKind { Final, Sentence, Tail }

    private sealed record Work(
        WorkKind Kind, Tracked Follow, string Text, string? Context, int Generation,
        Segment? Segment = null, string? Key = null, long SegmentId = 0, int Index = 0)
    {
        public static Work ForFinal(Tracked follow, Segment segment, string? context, int generation)
            => new(WorkKind.Final, follow, segment.Text, context, generation, Segment: segment);

        public static Work ForSentence(
            Tracked follow, long segmentId, int index, string text, string key, string? context, int generation)
            => new(WorkKind.Sentence, follow, text, context, generation, Key: key, SegmentId: segmentId, Index: index);

        public static Work ForTail(Tracked follow, TailCandidate tail, string? context, int generation)
            => new(WorkKind.Tail, follow, tail.Text, context, generation, SegmentId: tail.SegmentId, Index: tail.Index);
    }
}
