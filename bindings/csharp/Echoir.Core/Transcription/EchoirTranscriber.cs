using System.Runtime.InteropServices;
using Echoir.Core.Native;

namespace Echoir.Core;

// Live transcription over one loaded model. Load a model, Start a source, then read Transcript.
//
// The session runs in the core's queue mode. A background thread takes its events about 30
// times a second, and each batch is raised on the SynchronizationContext that was current at
// Start, in order. With no context, events are raised on the thread that took them. Handlers
// must not throw.
public sealed class EchoirTranscriber : IDisposable, IAsyncDisposable
{
    private const int PollIntervalMs = 33;

    // The envelope keeps one min/max pair per this many ms, so its count is in steps of it.
    private const int PeakStepMs = 10;

    // The window that Level reads.
    private const int LevelWindowMs = 50;

    // Guards the engine, the session and every call into the session. Never held while the
    // delivery lock is taken or user code runs.
    private readonly object _native = new();

    // Notifications waiting for delivery, in order.
    private readonly object _queue = new();

    // Held while notifications are raised, so they are raised one at a time and in order.
    private readonly object _deliver = new();

    private readonly Queue<Notification> _pending = new();
    private readonly SemaphoreSlim _loadGate = new(1, 1);
    private readonly Transcript _transcript = new();

    private EngineHandle? _engine;
    private SessionHandle? _session;
    private AudioSource _source;
    private bool _running;
    private bool _inputMuted;

    // The RetainAudio of the current session, in ms. Read with the session, under _native.
    private int _retainAudioMs;
    private Poller? _poller;
    private SynchronizationContext? _context;
    private bool _disposed;

    // The text of the session. Read it on the context the changes are raised on.
    public Transcript Transcript => _transcript;

    // Raised for an error of the session, such as a lost device.
    public event EventHandler<TranscriberErrorEventArgs>? Error;

    public bool IsLoaded
    {
        get
        {
            lock (_native)
            {
                return _engine is not null;
            }
        }
    }

    public bool IsRunning
    {
        get
        {
            lock (_native)
            {
                return _running;
            }
        }
    }

    // The loudest sample of the newest 50 ms, as a fraction of full scale. Zero with no session or
    // no audio yet.
    public float Level
    {
        get
        {
            lock (_native)
            {
                if (_session is null)
                {
                    return 0;
                }
                IntPtr handle = _session.DangerousGetHandle();
                long newest = EnvelopeEndLocked(handle);
                if (newest == 0)
                {
                    return 0;
                }
                Peak[] window = PeaksLocked(handle, Math.Max(0, newest - LevelWindowMs), newest, LevelWindowMs);
                return window.Length == 0 ? 0 : Math.Max(Math.Abs(window[0].Min), Math.Abs(window[0].Max));
            }
        }
    }

    // Replaces the audio entering the session with silence of the same length, so the timeline
    // keeps running and the VAD ends the ongoing segment. The silence is kept like any audio. Set
    // it while running: it is ignored otherwise, and it is cleared when the transcriber stops.
    public bool InputMuted
    {
        get
        {
            lock (_native)
            {
                return _inputMuted;
            }
        }
        set
        {
            lock (_native)
            {
                if (!_running || _session is null)
                {
                    return;
                }
                EchoirException.ThrowIfFailed(
                    NativeMethods.SessionSetInputMuted(_session.DangerousGetHandle(), value ? 1 : 0),
                    "echoir_session_set_input_muted");
                _inputMuted = value;
            }
        }
    }

    // The audio devices of this machine. Does not need a loaded model.
    public static AudioDevices Devices()
    {
        nint json = NativeMethods.DevicesJson();
        if (json == 0)
        {
            throw EchoirException.FromLastError(EchoirStatus.Internal, "echoir_devices_json");
        }
        return AudioDevices.Parse(Marshal.PtrToStringUTF8(json) ?? "{}");
    }

    // Loads a model and its VAD (vadId null: the model's default VAD) from modelsDir, on a thread
    // pool thread. A session that runs on the previous model is stopped first, and the previous
    // engine is freed. Model files that are missing or have the wrong size fail with an
    // EchoirException.
    public async Task LoadAsync(
        string modelId,
        string modelsDir,
        string? vadId = null,
        bool useGpu = true,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(modelId);
        ArgumentException.ThrowIfNullOrEmpty(modelsDir);
        ObjectDisposedException.ThrowIf(_disposed, this);

        await _loadGate.WaitAsync(cancellationToken);
        try
        {
            await StopCoreAsync();
            lock (_native)
            {
                ReleaseSessionLocked();
                _engine?.Dispose();
                _engine = null;
            }

            EngineHandle engine = await Task.Run(
                () => CreateEngine(modelId, modelsDir, vadId, useGpu), cancellationToken);
            bool keep;
            lock (_native)
            {
                keep = !_disposed && !cancellationToken.IsCancellationRequested;
                if (keep)
                {
                    _engine = engine;
                }
            }
            if (!keep)
            {
                engine.Dispose();
                cancellationToken.ThrowIfCancellationRequested();
                throw new ObjectDisposedException(nameof(EchoirTranscriber));
            }
        }
        finally
        {
            _loadGate.Release();
        }
    }

    // Starts a session on the loaded model. The mic and loopback sources open the device
    // (deviceId null: the default device, which follows the system default). The push source
    // takes audio through Push. Events are raised on the context current here.
    public void Start(AudioSource source, string? deviceId = null, TranscriberOptions? options = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        options ??= new TranscriberOptions();
        SynchronizationContext? context = SynchronizationContext.Current;

        lock (_native)
        {
            if (_engine is null)
            {
                throw new InvalidOperationException("No model is loaded. Call LoadAsync first.");
            }
            if (_running)
            {
                throw new InvalidOperationException("The transcriber is running. Call StopAsync first.");
            }

            // Each run gets its own session. The engine serves one session at a time, so the
            // stopped one goes first.
            ReleaseSessionLocked();
            _session = CreateSession(_engine, options);
            _retainAudioMs = RetainAudioMs(options);
            Volatile.Write(ref _context, context);

            int status;
            if (source == AudioSource.Push)
            {
                status = NativeMethods.SessionStart(_session.DangerousGetHandle());
            }
            else
            {
                using var device = Utf8String.Create(deviceId);
                int nativeSource = source == AudioSource.Loopback
                    ? NativeValues.SourceLoopback
                    : NativeValues.SourceMic;
                status = NativeMethods.SessionStartCapture(
                    _session.DangerousGetHandle(), nativeSource, device.Pointer);
            }
            EchoirException.ThrowIfFailed(status, source == AudioSource.Push
                ? "echoir_session_start"
                : "echoir_session_start_capture");

            _source = source;
            _running = true;
            _poller = new Poller(this);
            _poller.Start();
        }
    }

    // Sends audio to a running push session. Channels are interleaved in each frame.
    public unsafe void Push(ReadOnlySpan<float> samples, int sampleRate, int channels)
    {
        ValidateFrames(samples.Length, channels);
        lock (_native)
        {
            RequireRunningLocked();
            int status;
            fixed (float* pcm = samples)
            {
                status = NativeMethods.SessionPushF32(
                    _session!.DangerousGetHandle(), pcm, (nuint)(samples.Length / channels), sampleRate, channels);
            }
            EchoirException.ThrowIfFailed(status, "echoir_session_push_f32");
        }
    }

    // Sends 16-bit samples to a running push session. Full scale is 32768.
    public unsafe void Push(ReadOnlySpan<short> samples, int sampleRate, int channels)
    {
        ValidateFrames(samples.Length, channels);
        lock (_native)
        {
            RequireRunningLocked();
            int status;
            fixed (short* pcm = samples)
            {
                status = NativeMethods.SessionPushS16(
                    _session!.DangerousGetHandle(), pcm, (nuint)(samples.Length / channels), sampleRate, channels);
            }
            EchoirException.ThrowIfFailed(status, "echoir_session_push_s16");
        }
    }

    // Stops the session. A push session first decodes its buffered audio, and a capture session
    // closes its device and decodes what it captured. The FINAL events of that are raised before
    // the returned task completes.
    public Task StopAsync()
    {
        return StopCoreAsync();
    }

    // Drops the text in progress and empties the transcript. Timestamps keep running.
    public void Clear()
    {
        lock (_native)
        {
            if (_session is null)
            {
                EnqueueLocked(new Notification(NotificationKind.Cleared, 0, 0, 0, string.Empty));
            }
            else
            {
                EchoirException.ThrowIfFailed(
                    NativeMethods.SessionClear(_session.DangerousGetHandle()), "echoir_session_clear");
                CollectLocked();
            }
        }
        Notify();
    }

    // Decodes what is buffered, then starts the timeline over at 0. The FINAL events are raised
    // as well.
    public void Reset()
    {
        lock (_native)
        {
            if (_session is not null)
            {
                EchoirException.ThrowIfFailed(
                    NativeMethods.SessionReset(_session.DangerousGetHandle()), "echoir_session_reset");
                CollectLocked();
            }
        }
        Notify();
    }

    // Copies the audio of a segment as 16 kHz mono samples, with padMs of audio before and after
    // it. The padding is cut short at the start of the timeline and where the session no longer
    // keeps audio (RetainAudio). Returns false, with an empty array, when the session keeps no
    // audio, or when the audio of the segment itself is gone.
    public unsafe bool TryReadAudio(Segment segment, out short[] samples, int padMs = 150)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(padMs);
        samples = Array.Empty<short>();
        lock (_native)
        {
            if (_session is null || _retainAudioMs == 0)
            {
                return false;
            }
            IntPtr handle = _session.DangerousGetHandle();
            long newest = EnvelopeEndLocked(handle);
            // The store keeps the newest _retainAudioMs. The envelope counts whole steps, so its
            // newest end can be up to one step behind the audio, and the oldest kept ms is taken
            // as one step later. The estimate is never older than the store, so the core does not
            // evict a part that is read here.
            long oldest = newest + PeakStepMs - _retainAudioMs;
            long start = Math.Max(Math.Max(segment.StartMs - padMs, oldest), 0);
            if (start > segment.StartMs || segment.StartMs >= newest)
            {
                return false;
            }
            long end = Math.Max(start, Math.Min(segment.EndMs + padMs, newest));

            nuint frames;
            int status = NativeMethods.SessionReadAudio(handle, start, end, null, 0, &frames);
            if (status == NativeValues.StatusState)
            {
                return false;
            }
            EchoirException.ThrowIfFailed(status, "echoir_session_read_audio");
            if (frames == 0)
            {
                return true;
            }

            var copy = new short[(int)frames];
            nuint copied;
            fixed (short* pcm = copy)
            {
                status = NativeMethods.SessionReadAudio(handle, start, end, pcm, frames, &copied);
            }
            if (status == NativeValues.StatusState)
            {
                return false;
            }
            EchoirException.ThrowIfFailed(status, "echoir_session_read_audio");
            samples = copy;
            return true;
        }
    }

    // The level envelope of [startMs, endMs): the min and max of each bucket of bucketMs, which is
    // a positive multiple of 10. The end is clamped to the newest whole 10 ms, and the last bucket
    // may be shorter. Empty when there is no session.
    public Peak[] ReadPeaks(long startMs, long endMs, int bucketMs)
    {
        lock (_native)
        {
            if (_session is null)
            {
                return Array.Empty<Peak>();
            }
            IntPtr handle = _session.DangerousGetHandle();
            // The end is clamped here, not by the core, so the count and the copy below cover the
            // same buckets even while a capture adds audio. An end before the start is passed on
            // unchanged, and the core rejects it.
            long newest = EnvelopeEndLocked(handle);
            long clampedEnd = endMs < startMs ? endMs : Math.Max(startMs, Math.Min(endMs, newest));
            return PeaksLocked(handle, startMs, clampedEnd, bucketMs);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        Poller? poller;
        lock (_native)
        {
            poller = StopLocked(out _);
            ReleaseSessionLocked();
            _engine?.Dispose();
            _engine = null;
        }
        if (poller is not null)
        {
            poller.Stop();
            poller.Join();
        }
        Notify();
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }
        await StopCoreAsync();
        Dispose();
    }

    private async Task StopCoreAsync()
    {
        Poller? poller;
        int status;
        lock (_native)
        {
            poller = StopLocked(out status);
        }
        // Nothing was running, so there is nothing to raise and nothing to wait for.
        if (poller is null)
        {
            return;
        }
        poller.Stop();
        await DeliverAndWaitAsync();
        EchoirException.ThrowIfFailed(status, "echoir_session_stop");
    }

    // Stops the running session. Lock held. Returns the poller, which the caller stops after the
    // lock is released.
    private Poller? StopLocked(out int status)
    {
        status = NativeValues.StatusOk;
        if (!_running || _session is null)
        {
            return null;
        }

        IntPtr handle = _session.DangerousGetHandle();
        // The audio of a push session is not decoded by stop, so flush first. A flush error comes
        // as an ERROR event.
        if (_source == AudioSource.Push)
        {
            NativeMethods.SessionFlush(handle);
        }
        status = NativeMethods.SessionStop(handle);
        _running = false;
        _inputMuted = false;
        CollectLocked();

        Poller? poller = _poller;
        _poller = null;
        return poller;
    }

    // Lock held. Frees the session; the engine stays loaded.
    private void ReleaseSessionLocked()
    {
        SessionHandle? session = _session;
        _session = null;
        _running = false;
        session?.Dispose();
    }

    private void RequireRunningLocked()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!_running || _session is null)
        {
            throw new InvalidOperationException("The transcriber is not running. Call Start first.");
        }
    }

    private static void ValidateFrames(int length, int channels)
    {
        if (channels <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(channels), "Channels must be positive.");
        }
        if (length % channels != 0)
        {
            throw new ArgumentException("The sample count is not a whole number of frames.", nameof(channels));
        }
    }

    // Takes every queued event of the session into the pending list. Lock held. Returns the count.
    private unsafe int CollectLocked()
    {
        if (_session is null)
        {
            return 0;
        }

        int count = 0;
        IntPtr handle = _session.DangerousGetHandle();
        EventData ev = default;
        while (NativeMethods.SessionPoll(handle, &ev) == NativeValues.StatusOk)
        {
            if (FromNative(in ev) is Notification notification)
            {
                EnqueueLocked(notification);
                count++;
            }
        }
        return count;
    }

    private void EnqueueLocked(Notification notification)
    {
        lock (_queue)
        {
            _pending.Enqueue(notification);
        }
    }

    // Raises the pending notifications on the captured context, or here when there is none.
    private void Notify()
    {
        SynchronizationContext? context = Volatile.Read(ref _context);
        if (context is null || SynchronizationContext.Current == context)
        {
            Drain();
        }
        else
        {
            context.Post(static state => ((EchoirTranscriber)state!).Drain(), this);
        }
    }

    // Like Notify, but the returned task completes after the notifications are raised. Used by
    // StopAsync, so a caller that awaits it sees the final text.
    private Task DeliverAndWaitAsync()
    {
        SynchronizationContext? context = Volatile.Read(ref _context);
        if (context is null || SynchronizationContext.Current == context)
        {
            Drain();
            return Task.CompletedTask;
        }

        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        context.Post(_ =>
        {
            try
            {
                Drain();
                done.SetResult();
            }
            catch (Exception ex)
            {
                done.SetException(ex);
            }
        }, null);
        return done.Task;
    }

    private void Drain()
    {
        lock (_deliver)
        {
            while (true)
            {
                Notification next;
                lock (_queue)
                {
                    if (_pending.Count == 0)
                    {
                        return;
                    }
                    next = _pending.Dequeue();
                }
                Apply(next);
            }
        }
    }

    private void Apply(Notification notification)
    {
        switch (notification.Kind)
        {
            case NotificationKind.Partial:
                _transcript.ApplyPartial(new Segment(
                    notification.Id, notification.StartMs, notification.EndMs, notification.Text));
                break;
            case NotificationKind.Final:
                _transcript.ApplyFinal(new Segment(
                    notification.Id, notification.StartMs, notification.EndMs, notification.Text));
                break;
            case NotificationKind.Cleared:
                _transcript.ApplyCleared();
                break;
            case NotificationKind.Error:
                Error?.Invoke(this, new TranscriberErrorEventArgs(notification.Text));
                break;
        }
    }

    // Called by the poll thread about 30 times a second.
    private void PumpOnce()
    {
        int count;
        lock (_native)
        {
            count = CollectLocked();
        }
        if (count > 0)
        {
            Notify();
        }
    }

    private static Notification? FromNative(in EventData ev)
    {
        switch (ev.Type)
        {
            case NativeValues.EventPartial:
                return new Notification(NotificationKind.Partial, ev.SegmentId, ev.StartMs, ev.EndMs, Text(ev.Text));
            case NativeValues.EventFinal:
                return new Notification(NotificationKind.Final, ev.SegmentId, ev.StartMs, ev.EndMs, Text(ev.Text));
            case NativeValues.EventCleared:
                return new Notification(NotificationKind.Cleared, 0, 0, 0, string.Empty);
            case NativeValues.EventError:
                return new Notification(NotificationKind.Error, 0, 0, 0, Text(ev.Detail));
            default:
                // STATS is reserved and never sent by this core.
                return null;
        }
    }

    // Copies a native string. The pointer is valid only until the next poll.
    private static string Text(nint pointer)
    {
        return Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
    }

    private static unsafe EngineHandle CreateEngine(string modelId, string modelsDir, string? vadId, bool useGpu)
    {
        using var model = Utf8String.Create(modelId);
        using var dir = Utf8String.Create(modelsDir);
        using var vad = Utf8String.Create(vadId);

        ModelParams parameters = default;
        NativeMethods.ModelParamsInit(&parameters);
        parameters.ModelId = model.Pointer;
        parameters.ModelsDir = dir.Pointer;
        parameters.VadId = vad.Pointer;
        parameters.UseGpu = useGpu ? 1 : 0;

        nint engine = NativeMethods.EngineCreateFromModel(&parameters);
        if (engine == 0)
        {
            // The call returns no status, so the reason is in the message only.
            throw EchoirException.FromLastError(EchoirStatus.ModelLoad, "echoir_engine_create_from_model");
        }
        return new EngineHandle(engine);
    }

    private static unsafe SessionHandle CreateSession(EngineHandle engine, TranscriberOptions options)
    {
        using var language = Utf8String.Create(options.Language);

        SessionParams parameters = default;
        NativeMethods.SessionParamsInit(&parameters);
        parameters.Language = language.Pointer;
        parameters.MinChunkMs = options.MinChunkMs;
        parameters.MaxSegmentMs = options.MaxSegmentMs;
        parameters.EmitPartials = options.EmitPartials ? 1 : 0;
        parameters.StreamPolicy = (int)options.Policy;
        parameters.RetainAudioMs = RetainAudioMs(options);

        nint session = NativeMethods.SessionCreate(engine.DangerousGetHandle(), &parameters, 0, 0);
        if (session == 0)
        {
            throw EchoirException.FromLastError(EchoirStatus.InvalidArgument, "echoir_session_create");
        }
        return new SessionHandle(engine, session);
    }

    // The RetainAudio option in ms, as the core takes it.
    private static int RetainAudioMs(TranscriberOptions options)
    {
        double ms = options.RetainAudio.TotalMilliseconds;
        if (ms < 0 || ms > int.MaxValue)
        {
            throw new ArgumentOutOfRangeException(
                nameof(options), "RetainAudio must be from zero to int.MaxValue milliseconds.");
        }
        return (int)ms;
    }

    // Lock held. The end of the level envelope in ms: its count of 10 ms steps. Zero before any
    // audio.
    private static unsafe long EnvelopeEndLocked(IntPtr session)
    {
        nuint steps;
        int status = NativeMethods.SessionReadPeaks(session, 0, long.MaxValue, PeakStepMs, null, 0, &steps);
        EchoirException.ThrowIfFailed(status, "echoir_session_read_peaks");
        return (long)steps * PeakStepMs;
    }

    // Lock held. The envelope of [startMs, endMs) in buckets of bucketMs. The end must not be past
    // EnvelopeEndLocked, so the copy has the buckets that the count gave.
    private static unsafe Peak[] PeaksLocked(IntPtr session, long startMs, long endMs, int bucketMs)
    {
        nuint buckets;
        int status = NativeMethods.SessionReadPeaks(session, startMs, endMs, bucketMs, null, 0, &buckets);
        EchoirException.ThrowIfFailed(status, "echoir_session_read_peaks");
        if (buckets == 0)
        {
            return Array.Empty<Peak>();
        }

        var raw = new float[2 * (int)buckets];
        nuint written;
        fixed (float* pairs = raw)
        {
            status = NativeMethods.SessionReadPeaks(session, startMs, endMs, bucketMs, pairs, buckets, &written);
        }
        EchoirException.ThrowIfFailed(status, "echoir_session_read_peaks");

        var peaks = new Peak[(int)written];
        for (int i = 0; i < peaks.Length; i++)
        {
            peaks[i] = new Peak(raw[2 * i], raw[2 * i + 1]);
        }
        return peaks;
    }

    private enum NotificationKind
    {
        Partial,
        Final,
        Cleared,
        Error,
    }

    // One event waiting to be raised. For an error, Text holds the message.
    private readonly record struct Notification(
        NotificationKind Kind,
        long Id,
        long StartMs,
        long EndMs,
        string Text);

    // The poll thread. It is stopped by a flag, and joined by Dispose.
    private sealed class Poller
    {
        private readonly EchoirTranscriber _owner;
        private readonly ManualResetEventSlim _stop = new(false);

        public Poller(EchoirTranscriber owner)
        {
            _owner = owner;
            Worker = new Thread(Run)
            {
                IsBackground = true,
                Name = "Echoir transcriber poll",
            };
        }

        public Thread Worker { get; }

        public void Start()
        {
            Worker.Start();
        }

        public void Stop()
        {
            _stop.Set();
        }

        public void Join()
        {
            if (Worker.IsAlive && Worker != Thread.CurrentThread)
            {
                Worker.Join();
            }
        }

        private void Run()
        {
            while (!_stop.IsSet)
            {
                _owner.PumpOnce();
                _stop.Wait(PollIntervalMs);
            }
        }
    }
}
