using System.Collections.Specialized;

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

// Follows the FINAL segments of one or more transcripts and translates them through one translator.
//
// One translation is in flight at a time, for all followed transcripts. While it runs, the segments
// that arrive wait in order; once a transcript has more than MaxWaiting of them, its oldest waiting
// segment is dropped and stays untranslated (Skipped), so the translations show the present rather
// than a backlog. A segment is translated with the two FINAL segments before it in its transcript
// as context, source text only, so a late or dropped translation changes nothing for the others.
// PARTIAL text is never translated.
//
// A clear of a followed transcript, Reset, and a restart of its segment ids (a new session on the
// same transcript) drop what waits for it, and the result of a translation that was already running
// is dropped when it returns. Translated and Skipped are raised on the SynchronizationContext that
// was current when the transcript was followed, in the order the segments were translated; with no
// context, on the worker's thread. Handlers must not throw.
//
// Follow, Unfollow, Reset and Submit are called on the transcript's context, like any read of its
// Finals. The translator is not owned: dispose it separately, after this.
public sealed class TranscriptTranslator : IDisposable
{
    // Segments of one transcript that may wait behind the one being translated.
    public const int MaxWaiting = 3;

    // Earlier FINAL segments of the same transcript passed to the translator as context.
    public const int ContextLines = 2;

    private readonly ITextTranslator _translator;
    private readonly string _target;
    private readonly string? _source;

    // Guards the queue, the follows and the flags. Never held while user code or a translation runs.
    private readonly object _gate = new();
    private readonly List<Tracked> _follows = new();
    private readonly LinkedList<Work> _queue = new();
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
        ((INotifyCollectionChanged)transcript.Finals).CollectionChanged += follow.Handler;
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
        }
        foreach (Tracked follow in follows)
        {
            ((INotifyCollectionChanged)follow.Transcript.Finals).CollectionChanged -= follow.Handler;
        }
        _stop.Cancel();
        _wake.Release();
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

        Segment? dropped = null;
        lock (_gate)
        {
            if (_disposed || !_follows.Contains(follow))
            {
                return;
            }
            _queue.AddLast(new Work(follow, segment, context, follow.Generation));
            int waiting = _queue.Count(w => w.Follow == follow);
            if (waiting > MaxWaiting)
            {
                LinkedListNode<Work>? oldest = _queue.First;
                while (oldest is not null && oldest.Value.Follow != follow)
                {
                    oldest = oldest.Next;
                }
                if (oldest is not null)
                {
                    dropped = oldest.Value.Segment;
                    _queue.Remove(oldest);
                }
            }
            _worker ??= Task.Run(RunAsync);
        }
        _wake.Release();
        if (dropped is not null)
        {
            Raise(follow, follow.Generation, () =>
                Skipped?.Invoke(this, new TranslationSkippedEventArgs(follow.Transcript, follow.Track, dropped, null)));
        }
    }

    // Removes what waits for the follow and makes a result still to come from it stale.
    private void DropAllLocked(Tracked follow)
    {
        follow.Generation++;
        for (LinkedListNode<Work>? node = _queue.First; node is not null;)
        {
            LinkedListNode<Work>? next = node.Next;
            if (node.Value.Follow == follow)
            {
                _queue.Remove(node);
            }
            node = next;
        }
    }

    private async Task RunAsync()
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

            Work? work;
            lock (_gate)
            {
                work = _queue.First?.Value;
                if (work is not null)
                {
                    _queue.RemoveFirst();
                }
            }
            if (work is null)
            {
                continue;
            }

            Translation? translation = null;
            string? error = null;
            try
            {
                translation = await _translator
                    .TranslateAsync(work.Segment.Text, _target, _source, work.Context, token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                error = ex.Message;
            }

            Tracked follow = work.Follow;
            Segment segment = work.Segment;
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
        }
    }

    // Runs the raise on the follow's context, if the follow was not reset or stopped meanwhile. The
    // check runs on that context, which is also where Reset is called, so a result cannot arrive
    // after the reset that made it stale.
    private void Raise(Tracked follow, int generation, Action raise)
    {
        void Run()
        {
            lock (_gate)
            {
                if (_disposed || follow.Generation != generation || !_follows.Contains(follow))
                {
                    return;
                }
            }
            raise();
        }

        if (follow.Context is null)
        {
            Run();
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

        // Bumped by every drop. Under the gate.
        public int Generation { get; set; }

        // The id of the newest segment seen. Under the gate.
        public long LastId { get; set; } = long.MinValue;
    }

    private sealed record Work(Tracked Follow, Segment Segment, string? Context, int Generation);
}
