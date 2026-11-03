using Echoir.Core.Native;

namespace Echoir.Core;

// A loaded model and its VAD that several transcribers share, one per audio source. Each
// transcriber made with new EchoirTranscriber(engine) runs its own session on it; their decodes
// take turns on the model in the order they ask, so each adds to the others' lag (Lag, and WaitMs
// in TranscriberStats). The model is loaded once, however many transcribers use it.
//
// Dispose may come before the transcribers are disposed. The model stays loaded while any of their
// sessions exists (a transcriber keeps its last session until it starts again or is disposed), and
// after Dispose no transcriber can start a new session on it.
public sealed class EchoirEngine : IDisposable
{
    private readonly EngineHandle _handle;

    // Set by Dispose. The handle itself stays open while sessions hold it.
    private volatile bool _disposed;

    private EchoirEngine(EngineHandle handle, string modelId)
    {
        _handle = handle;
        ModelId = modelId;
    }

    // The registry id of the loaded model.
    public string ModelId { get; }

    internal EngineHandle Handle => _handle;

    internal bool IsDisposed => _disposed;

    // Loads a model and its VAD (vadId null: the model's default VAD) from modelsDir, on a thread
    // pool thread. Model files that are missing or have the wrong size fail with an
    // EchoirException.
    public static async Task<EchoirEngine> LoadAsync(
        string modelId,
        string modelsDir,
        string? vadId = null,
        bool useGpu = true,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(modelId);
        ArgumentException.ThrowIfNullOrEmpty(modelsDir);
        EngineHandle handle = await Task.Run(
            () => EchoirTranscriber.CreateEngine(modelId, modelsDir, vadId, useGpu), cancellationToken);
        if (cancellationToken.IsCancellationRequested)
        {
            handle.Dispose();
            cancellationToken.ThrowIfCancellationRequested();
        }
        return new EchoirEngine(handle, modelId);
    }

    // Frees the model once no session runs on it any more.
    public void Dispose()
    {
        _disposed = true;
        _handle.Dispose();
    }
}
