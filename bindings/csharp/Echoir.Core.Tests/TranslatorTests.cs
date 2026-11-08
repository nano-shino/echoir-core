using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// EchoirTranslator on the scripted translator of the fake build. Its text is "[<target>] <text>".
[TestClass]
[TestCategory("fake")]
public sealed class FakeTranslatorTests
{
    private static string NoModelsDir => Path.Combine(Path.GetTempPath(), "echoir-no-models");

    [TestMethod]
    public async Task TranslatesWithTheDefaultModel()
    {
        using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, NoModelsDir);
        Assert.AreEqual("hy-mt2-1.8b-q4_k_m", translator.ModelId);

        Translation result = await translator.TranslateAsync("Good morning.", "vi", "en", "Earlier line.");
        Assert.AreEqual("[vi] Good morning.", result.Text);
        Assert.IsFalse(result.Capped);
        Assert.AreEqual("[ja] Hello.", translator.Translate("Hello.", "ja").Text);
    }

    [TestMethod]
    public async Task TwoThreadsOnOneTranslatorEachReadTheirOwnText()
    {
        using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, NoModelsDir);
        int wrong = 0;
        Thread[] threads = Enumerable.Range(0, 4).Select(i => new Thread(() =>
        {
            for (int n = 0; n < 100; n++)
            {
                string line = $"line {i} {n}";
                if (translator.Translate(line, "fr").Text != "[fr] " + line)
                {
                    Interlocked.Increment(ref wrong);
                }
            }
        })).ToArray();
        foreach (Thread thread in threads)
        {
            thread.Start();
        }
        foreach (Thread thread in threads)
        {
            thread.Join();
        }
        Assert.AreEqual(0, wrong);
    }

    [TestMethod]
    public async Task LoadsFromACatalogueGivenAsText()
    {
        string registry = """
            {"version":1,"default_vad":{"whisper":"","qwen":""},"default_translator":"mine","models":[
              {"id":"mine","backend":"translator","display":"Mine","prompt_style":"hy-mt2-1.8b",
               "files":[{"role":"model","name":"mine.gguf","url":"https://example.invalid/mine.gguf","size":1000}]}
            ]}
            """;
        EchoirModels models = new(registry);
        string? defaultId = models.Catalogue().DefaultTranslator;
        using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, NoModelsDir, registry);
        Assert.AreEqual(defaultId, translator.ModelId);
        Assert.AreEqual("[de] Hallo", translator.Translate("Hallo", "de").Text);
    }

    [TestMethod]
    public async Task RefusesAModelThatIsNotATranslator()
    {
        EchoirException ex = await Assert.ThrowsExceptionAsync<EchoirException>(
            () => EchoirTranslator.LoadAsync("whisper-tiny-en", NoModelsDir));
        Assert.AreEqual(EchoirStatus.InvalidArgument, ex.Status);
        StringAssert.Contains(ex.Message, "is not a translator");
    }

    [TestMethod]
    public async Task ReportsABadRequestAsEchoirException()
    {
        using EchoirTranslator translator = await EchoirTranslator.LoadAsync("hy-mt2-7b-q4_k_m", NoModelsDir);
        EchoirException blank = Assert.ThrowsException<EchoirException>(() => translator.Translate("  ", "vi"));
        Assert.AreEqual(EchoirStatus.InvalidArgument, blank.Status);
        EchoirException language = await Assert.ThrowsExceptionAsync<EchoirException>(
            () => translator.TranslateAsync("Hello.", "xx"));
        StringAssert.Contains(language.Message, "unsupported target language");
        Assert.ThrowsException<ArgumentException>(() => translator.Translate("Hello.", ""));
    }

    [TestMethod]
    public async Task DisposeDuringCallsAndAfter()
    {
        EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, NoModelsDir);
        translator.Dispose();
        Assert.ThrowsException<ObjectDisposedException>(() => translator.Translate("Hello.", "vi"));
        translator.Dispose();
    }

    [TestMethod]
    public async Task FollowsATranscriptThroughTheNativeTranslator()
    {
        using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, NoModelsDir);
        using var follower = new TranscriptTranslator(translator, "fr");
        var seen = new System.Collections.Concurrent.ConcurrentQueue<string>();
        follower.Translated += (_, e) => seen.Enqueue(e.Text);
        var transcript = new Transcript();
        follower.Follow(transcript);

        transcript.ApplyFinal(new Segment(1, 0, 900, "Hello there."));
        transcript.ApplyFinal(new Segment(2, 1000, 1900, "How are you?"));
        Wait.Until(() => seen.Count == 2, "two translations");
        CollectionAssert.AreEqual(new[] { "[fr] Hello there.", "[fr] How are you?" }, seen.ToArray());
    }

    [TestMethod]
    public void TheCatalogueListsTheTranslators()
    {
        ModelCatalogue catalogue = new EchoirModels().Catalogue();
        Assert.AreEqual("hy-mt2-1.8b-q4_k_m", catalogue.DefaultTranslator);
        ModelInfo[] translators = catalogue.Models.Where(m => m.Backend == "translator").ToArray();
        CollectionAssert.AreEquivalent(
            new[] { "hy-mt2-1.8b-q4_k_m", "hy-mt2-7b-q4_k_m" }, translators.Select(m => m.Id).ToArray());
        Assert.IsNull(catalogue.DefaultVadId(translators[0]));
    }
}

// EchoirTranslator with the real Hy-MT2 1.8B model. Reported as inconclusive when the model is not
// in models/.
[TestClass]
[TestCategory("real")]
public sealed class RealTranslatorTests
{
    private const string ModelFile = "Hy-MT2-1.8B-Q4_K_M.gguf";

    [TestMethod]
    public async Task TranslatesWithTheRealModel()
    {
        if (!File.Exists(Path.Combine(TestPaths.ModelsDir, ModelFile)))
        {
            Assert.Inconclusive(ModelFile + " is not in models/.");
        }

        using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, TestPaths.ModelsDir);
        Translation result = await translator.TranslateAsync("Good morning, everyone.", "fr", "en");
        StringAssert.Contains(result.Text.ToLowerInvariant(), "bonjour");
        Assert.IsFalse(result.Capped);
        Assert.IsTrue(result.OutputTokens > 0);

        using var follower = new TranscriptTranslator(translator, "fr", "en");
        var seen = new System.Collections.Concurrent.ConcurrentQueue<TranslatedEventArgs>();
        follower.Translated += (_, e) => seen.Enqueue(e);
        var transcript = new Transcript();
        follower.Follow(transcript, "mic");
        transcript.ApplyFinal(new Segment(1, 0, 900, "Thank you very much."));
        Wait.Until(() => seen.Count == 1, "a translation", 30_000);
        StringAssert.Contains(seen.Single().Text.ToLowerInvariant(), "merci");
    }
}
