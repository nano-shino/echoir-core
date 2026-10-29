using System.Collections.ObjectModel;
using System.Text.Json;

namespace Echoir.Core;

// The catalogue as echoir_models_json reports it: the registry entries with their files, and the
// machine's devices.
public sealed class ModelCatalogue
{
    public ModelCatalogue(
        IReadOnlyList<DeviceInfo> devices,
        IReadOnlyDictionary<string, string> defaultVad,
        IReadOnlyList<ModelInfo> models)
    {
        Devices = devices;
        DefaultVad = defaultVad;
        Models = models;
    }

    // The compute devices that ggml sees. Empty in a build without the backends.
    public IReadOnlyList<DeviceInfo> Devices { get; }

    // The default VAD id per backend key ("whisper", "qwen").
    public IReadOnlyDictionary<string, string> DefaultVad { get; }

    public IReadOnlyList<ModelInfo> Models { get; }

    public ModelInfo? Find(string id)
    {
        foreach (ModelInfo model in Models)
        {
            if (model.Id == id)
            {
                return model;
            }
        }
        return null;
    }

    // Parses the echoir_models_json text. A memory budget, when given, replaces the core's fits.
    internal static ModelCatalogue Parse(string json, long? memoryBudgetMb)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement root = document.RootElement;

        var devices = new List<DeviceInfo>();
        foreach (JsonElement device in root.GetProperty("devices").EnumerateArray())
        {
            devices.Add(new DeviceInfo(
                Name: device.GetProperty("name").GetString() ?? string.Empty,
                IsGpu: device.GetProperty("type").GetString() == "gpu",
                MemoryTotalMb: device.GetProperty("memory_total_mb").GetInt64(),
                MemoryFreeMb: device.GetProperty("memory_free_mb").GetInt64()));
        }

        var defaultVad = new Dictionary<string, string>();
        foreach (JsonProperty vad in root.GetProperty("default_vad").EnumerateObject())
        {
            defaultVad[vad.Name] = vad.Value.GetString() ?? string.Empty;
        }

        var models = new List<ModelInfo>();
        foreach (JsonElement model in root.GetProperty("models").EnumerateArray())
        {
            models.Add(ParseModel(model, memoryBudgetMb));
        }

        return new ModelCatalogue(
            devices.AsReadOnly(),
            new ReadOnlyDictionary<string, string>(defaultVad),
            models.AsReadOnly());
    }

    private static ModelInfo ParseModel(JsonElement model, long? memoryBudgetMb)
    {
        long? minVram = model.TryGetProperty("min_vram_mb", out JsonElement min) && min.ValueKind == JsonValueKind.Number
            ? min.GetInt64()
            : null;
        bool fits = memoryBudgetMb is long budget
            ? minVram is not long needed || needed <= budget
            : model.GetProperty("fits").GetBoolean();

        var files = new List<ModelFile>();
        foreach (JsonElement file in model.GetProperty("files").EnumerateArray())
        {
            files.Add(new ModelFile(
                Role: file.GetProperty("role").GetString() ?? string.Empty,
                Name: file.GetProperty("name").GetString() ?? string.Empty,
                Url: file.GetProperty("url").GetString() ?? string.Empty,
                Size: file.GetProperty("size").GetInt64(),
                Present: file.GetProperty("present").GetBoolean()));
        }

        return new ModelInfo(
            Id: model.GetProperty("id").GetString() ?? string.Empty,
            Backend: model.GetProperty("backend").GetString() ?? string.Empty,
            Display: model.GetProperty("display").GetString() ?? string.Empty,
            MinVramMb: minVram,
            Present: model.GetProperty("present").GetBoolean(),
            Fits: fits,
            Files: files.AsReadOnly());
    }
}

// A compute device that ggml reports.
public sealed record DeviceInfo(string Name, bool IsGpu, long MemoryTotalMb, long MemoryFreeMb);

// A catalogue entry. Present is true when every file is in the models directory with its size.
// Fits is advisory: the core compares MinVramMb with the best device's free memory.
public sealed record ModelInfo(
    string Id,
    string Backend,
    string Display,
    long? MinVramMb,
    bool Present,
    bool Fits,
    IReadOnlyList<ModelFile> Files);

// One file of an entry. Size is the exact byte count the catalogue gives.
public sealed record ModelFile(string Role, string Name, string Url, long Size, bool Present);
