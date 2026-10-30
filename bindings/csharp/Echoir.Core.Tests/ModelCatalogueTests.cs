using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// The default VAD lookup works on the catalogue's values alone, so it needs no native library.
[TestClass]
public sealed class ModelCatalogueTests
{
    [TestMethod]
    public void DefaultVadIdFindsTheVadOfEachSpeechBackend()
    {
        var defaultVad = new Dictionary<string, string>
        {
            ["whisper"] = "silero",
            ["qwen"] = "firered",
        };
        var catalogue = new ModelCatalogue([], defaultVad, []);

        Assert.AreEqual("silero", catalogue.DefaultVadId(Model("whisper")));
        Assert.AreEqual("firered", catalogue.DefaultVadId(Model("qwen3-asr")));
    }

    [TestMethod]
    public void DefaultVadIdIsNullWithoutADefault()
    {
        var catalogue = new ModelCatalogue([], new Dictionary<string, string> { ["whisper"] = "silero" }, []);

        Assert.IsNull(catalogue.DefaultVadId(Model("qwen3-asr")));
        Assert.IsNull(catalogue.DefaultVadId(Model("vad")));
    }

    private static ModelInfo Model(string backend) =>
        new("fake", backend, "Fake", null, false, true, []);
}
