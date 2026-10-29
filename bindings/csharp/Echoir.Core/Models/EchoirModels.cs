using System.Buffers;
using System.Runtime.InteropServices;
using Echoir.Core.Native;

namespace Echoir.Core;

// The model catalogue and its download. The catalogue comes from the core (echoir_models_json);
// the download runs here, over HttpClient.
public sealed class EchoirModels
{
    private const int BufferSize = 81920;

    // Downloads have no overall timeout: a large model can take many minutes.
    private static readonly HttpClient Http = new() { Timeout = Timeout.InfiniteTimeSpan };

    private readonly string? _registryJson;

    // Uses the built-in catalogue.
    public EchoirModels() : this(registryJson: null)
    {
    }

    // Uses the catalogue text given in the registry format, instead of the built-in one.
    public EchoirModels(string? registryJson)
    {
        _registryJson = registryJson;
    }

    // Lists the catalogue, with files present in modelsDir (null: none). A memory budget, in MB,
    // replaces the core's fits check.
    public ModelCatalogue Catalogue(string? modelsDir = null, long? memoryBudgetMb = null)
    {
        using var registry = Utf8String.Create(_registryJson);
        using var dir = Utf8String.Create(modelsDir);
        nint json = NativeMethods.ModelsJson(registry.Pointer, dir.Pointer);
        if (json == 0)
        {
            throw EchoirException.FromLastError(EchoirStatus.InvalidArgument, "echoir_models_json");
        }
        string text = Marshal.PtrToStringUTF8(json) ?? string.Empty;
        return ModelCatalogue.Parse(text, memoryBudgetMb);
    }

    // Downloads the files of a model into modelsDir. Files already present are skipped. With
    // includeVad, the model's default VAD is fetched too. Each file goes to <name>.part, is checked
    // against the catalogue size, then renamed. On failure the .part file is removed and the
    // exception is rethrown. Progress is the fraction of the bytes to fetch, from 0 to 1.
    public async Task DownloadAsync(
        string id,
        string modelsDir,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default,
        bool includeVad = true)
    {
        ArgumentException.ThrowIfNullOrEmpty(id);
        ArgumentException.ThrowIfNullOrEmpty(modelsDir);

        ModelCatalogue catalogue = Catalogue(modelsDir);
        ModelInfo model = catalogue.Find(id)
            ?? throw new ArgumentException($"'{id}' is not an id in the catalogue.", nameof(id));

        var files = new List<ModelFile>(model.Files);
        if (includeVad
            && catalogue.DefaultVad.TryGetValue(model.Backend, out string? vadId)
            && vadId is not null
            && catalogue.Find(vadId) is ModelInfo vad)
        {
            files.AddRange(vad.Files);
        }

        Directory.CreateDirectory(modelsDir);

        // Bytes of the files that finished, and of the file in progress, against the bytes to fetch.
        long total = files.Where(file => !file.Present && file.Size > 0).Sum(file => file.Size);
        long finished = 0;
        void Report(long inProgress)
        {
            if (total > 0 && progress is not null)
            {
                progress.Report(Math.Clamp((double)(finished + inProgress) / total, 0, 1));
            }
        }

        foreach (ModelFile file in files)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (file.Present)
            {
                continue;
            }
            await DownloadFileAsync(file, modelsDir, Report, cancellationToken).ConfigureAwait(false);
            finished += Math.Max(file.Size, 0);
            Report(0);
        }
    }

    private static async Task DownloadFileAsync(
        ModelFile file,
        string modelsDir,
        Action<long> onBytes,
        CancellationToken cancellationToken)
    {
        string finalPath = Path.Combine(modelsDir, file.Name);
        string partPath = finalPath + ".part";
        try
        {
            using HttpResponseMessage response = await Http
                .GetAsync(file.Url, HttpCompletionOption.ResponseHeadersRead, cancellationToken)
                .ConfigureAwait(false);
            response.EnsureSuccessStatusCode();

            long written = 0;
            byte[] buffer = ArrayPool<byte>.Shared.Rent(BufferSize);
            try
            {
                await using Stream input = await response.Content
                    .ReadAsStreamAsync(cancellationToken)
                    .ConfigureAwait(false);
                await using FileStream output = new(
                    partPath, FileMode.Create, FileAccess.Write, FileShare.None, BufferSize, FileOptions.Asynchronous);
                int read;
                while ((read = await input.ReadAsync(buffer.AsMemory(0, BufferSize), cancellationToken)
                    .ConfigureAwait(false)) > 0)
                {
                    await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
                    written += read;
                    onBytes(written);
                }
            }
            finally
            {
                ArrayPool<byte>.Shared.Return(buffer);
            }

            if (file.Size >= 0 && written != file.Size)
            {
                throw new IOException(
                    $"{file.Name} is {written} bytes, the catalogue says {file.Size}.");
            }
            File.Move(partPath, finalPath, overwrite: true);
        }
        catch
        {
            TryDelete(partPath);
            throw;
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
