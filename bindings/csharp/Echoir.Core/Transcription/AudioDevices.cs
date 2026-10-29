using System.Text.Json;

namespace Echoir.Core;

// An audio device. Id is stable across runs and is passed to Start.
public sealed record AudioDevice(string Id, string Name, bool IsDefault);

// The capture devices (microphones) and the loopback devices (what the machine plays).
public sealed record AudioDevices(IReadOnlyList<AudioDevice> Capture, IReadOnlyList<AudioDevice> Loopback)
{
    // Parses echoir_devices_json.
    internal static AudioDevices Parse(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        return new AudioDevices(
            ReadList(document.RootElement, "capture"),
            ReadList(document.RootElement, "loopback"));
    }

    private static IReadOnlyList<AudioDevice> ReadList(JsonElement root, string name)
    {
        var list = new List<AudioDevice>();
        if (root.TryGetProperty(name, out JsonElement array))
        {
            foreach (JsonElement item in array.EnumerateArray())
            {
                list.Add(new AudioDevice(
                    Id: item.GetProperty("id").GetString() ?? string.Empty,
                    Name: item.GetProperty("name").GetString() ?? string.Empty,
                    IsDefault: item.TryGetProperty("default", out JsonElement isDefault) && isDefault.GetBoolean()));
            }
        }
        return list.AsReadOnly();
    }
}
