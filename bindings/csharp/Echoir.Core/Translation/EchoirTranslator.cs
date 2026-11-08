using System.Runtime.InteropServices;
using Echoir.Core.Native;

namespace Echoir.Core;

// The result of one translation.
//   Text          the translation, whole text.
//   Capped        the model stopped at its generation limit, so Text may end mid-sentence.
//   SourceTokens  tokens of the line; PromptTokens includes the context and the instruction.
//   Prefill       time to read the prompt and pick the first token; Decode is the rest.
public sealed record Translation(
    string Text,
    bool Capped,
    int SourceTokens,
    int PromptTokens,
    int OutputTokens,
    TimeSpan Prefill,
    TimeSpan Decode);

// Something that translates a line of text. EchoirTranslator is the one the library provides;
// TranscriptTranslator takes any, so a host or a test can stand one in.
public interface ITextTranslator
{
    // Translates text into targetLanguage (an ISO code such as "en", "vi", "ja", "zh"). The source
    // language is optional ("auto" or null: the model finds it). context holds earlier lines of the
    // same speaker in the source language, oldest first, separated by newlines.
    Task<Translation> TranslateAsync(
        string text,
        string targetLanguage,
        string? sourceLanguage = null,
        string? context = null,
        CancellationToken cancellationToken = default);
}

// A loaded translation model. It is separate from the speech engine: it has its own context on the
// GPU or CPU and never takes the engine's turn, so translating runs in parallel with transcribing.
// Calls run one at a time, from any thread. Dispose may come while a call runs: the model is freed
// when the call returns.
public sealed class EchoirTranslator : ITextTranslator, IDisposable
{
    private readonly TranslatorHandle _handle;

    private EchoirTranslator(TranslatorHandle handle, string modelId)
    {
        _handle = handle;
        ModelId = modelId;
    }

    // The registry id of the loaded model.
    public string ModelId { get; }

    // Loads a translator (modelId null or empty: the catalogue's default) from modelsDir, on a
    // thread pool thread. It also runs a few short translations to warm the GPU up, so loading
    // takes a few seconds. Model files that are missing or have the wrong size fail with an
    // EchoirException, and so does an id that is not a translator.
    public static Task<EchoirTranslator> LoadAsync(
        string? modelId,
        string modelsDir,
        bool useGpu = true,
        int threads = 0,
        CancellationToken cancellationToken = default)
        => LoadAsync(modelId, modelsDir, null, useGpu, threads, cancellationToken);

    // As above, with a catalogue text in the registry format (see EchoirModels) instead of the
    // built-in one; null or empty is the built-in catalogue. A null modelId is then that
    // catalogue's default_translator.
    public static async Task<EchoirTranslator> LoadAsync(
        string? modelId,
        string modelsDir,
        string? registryJson,
        bool useGpu = true,
        int threads = 0,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(modelsDir);
        ArgumentOutOfRangeException.ThrowIfNegative(threads);
        (TranslatorHandle handle, string id) = await Task.Run(
            () => Create(modelId, modelsDir, registryJson, useGpu, threads), cancellationToken);
        if (cancellationToken.IsCancellationRequested)
        {
            handle.Dispose();
            cancellationToken.ThrowIfCancellationRequested();
        }
        return new EchoirTranslator(handle, id);
    }

    // Translates on a thread pool thread. Throws EchoirException for a language the model does not
    // support, a blank text or one too long for the model, and a model failure. A cancel stops the
    // wait, not a call that already runs.
    public Task<Translation> TranslateAsync(
        string text,
        string targetLanguage,
        string? sourceLanguage = null,
        string? context = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentException.ThrowIfNullOrEmpty(targetLanguage);
        return Task.Run(() => Translate(text, targetLanguage, sourceLanguage, context), cancellationToken);
    }

    // Translates on the calling thread. The call takes a few hundred milliseconds on a GPU.
    public unsafe Translation Translate(
        string text,
        string targetLanguage,
        string? sourceLanguage = null,
        string? context = null)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentException.ThrowIfNullOrEmpty(targetLanguage);

        using var textUtf8 = Utf8String.Create(text);
        using var source = Utf8String.Create(string.IsNullOrEmpty(sourceLanguage) ? null : sourceLanguage);
        using var target = Utf8String.Create(targetLanguage);
        using var contextUtf8 = Utf8String.Create(string.IsNullOrEmpty(context) ? null : context);

        TranslateRequestData request = default;
        request.StructSize = (uint)sizeof(TranslateRequestData);
        request.Text = textUtf8.Pointer;
        request.Source = source.Pointer;
        request.Target = target.Pointer;
        request.Context = contextUtf8.Pointer;

        bool added = false;
        try
        {
            _handle.DangerousAddRef(ref added);
        }
        catch (ObjectDisposedException)
        {
            throw new ObjectDisposedException(nameof(EchoirTranslator));
        }
        try
        {
            TranslationData* result = NativeMethods.TranslatorTranslate(_handle.DangerousGetHandle(), &request);
            if (result == null)
            {
                throw EchoirException.FromLastFailure("echoir_translator_translate");
            }
            // The result is the library's own, valid until this thread's next translate call, so it
            // is copied here. A field is read only when the result's struct_size covers it, which
            // is how fields added to the end later are told from a library that lacks them.
            uint size = result->StructSize;
            return new Translation(
                Marshal.PtrToStringUTF8(result->Text) ?? string.Empty,
                result->Capped != 0,
                result->SourceTokens,
                result->PromptTokens,
                result->OutputTokens,
                Covers(size, nameof(TranslationData.PrefillMs), sizeof(double))
                    ? TimeSpan.FromMilliseconds(result->PrefillMs) : TimeSpan.Zero,
                Covers(size, nameof(TranslationData.DecodeMs), sizeof(double))
                    ? TimeSpan.FromMilliseconds(result->DecodeMs) : TimeSpan.Zero);
        }
        finally
        {
            if (added)
            {
                _handle.DangerousRelease();
            }
        }
    }

    // Frees the model once no call runs on it.
    public void Dispose()
    {
        _handle.Dispose();
    }

    // True when a native struct of the given size includes the field and all before it.
    private static bool Covers(uint structSize, string field, int fieldSize)
        => structSize >= (long)Marshal.OffsetOf<TranslationData>(field) + fieldSize;

    private static unsafe (TranslatorHandle Handle, string ModelId) Create(
        string? modelId, string modelsDir, string? registryJson, bool useGpu, int threads)
    {
        // The default id is resolved before the native create, so a failure here leaks no handle.
        string? registryText = string.IsNullOrEmpty(registryJson) ? null : registryJson;
        string id = string.IsNullOrEmpty(modelId)
            ? new EchoirModels(registryText).Catalogue().DefaultTranslator ?? string.Empty
            : modelId;
        using var model = Utf8String.Create(string.IsNullOrEmpty(modelId) ? null : modelId);
        using var dir = Utf8String.Create(modelsDir);
        using var registry = Utf8String.Create(registryText);

        TranslatorParams parameters = default;
        NativeMethods.TranslatorParamsInit(&parameters);
        parameters.ModelId = model.Pointer;
        parameters.ModelsDir = dir.Pointer;
        parameters.RegistryJson = registry.Pointer;
        parameters.UseGpu = useGpu ? 1 : 0;
        parameters.NThreads = threads;

        nint translator = NativeMethods.TranslatorCreateFromModel(&parameters);
        if (translator == 0)
        {
            // The call returns a pointer, so its status is the core's last status: ModelLoad for a
            // missing file, InvalidArgument for an unknown id or one that is not a translator.
            throw EchoirException.FromLastFailure("echoir_translator_create_from_model");
        }
        return (new TranslatorHandle(translator), id);
    }
}
