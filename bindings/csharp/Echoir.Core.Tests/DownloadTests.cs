using System.Net;
using System.Net.Sockets;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// Downloads from a local HttpListener, against a registry that lists one model with one file.
[TestClass]
public sealed class DownloadTests
{
    private const string ModelId = "fake-asr";
    private const string FileName = "fake.bin";
    private const string QwenModelId = "fake-qwen";
    private const string VadId = "fake-vad";
    private const string VadFileName = "vad.bin";

    private string _dir = string.Empty;
    private HttpListener? _listener;

    [TestInitialize]
    public void Setup()
    {
        _dir = Path.Combine(Path.GetTempPath(), "echoir-download-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    [TestCleanup]
    public void Cleanup()
    {
        _listener?.Close();
        if (Directory.Exists(_dir))
        {
            Directory.Delete(_dir, recursive: true);
        }
    }

    [TestMethod]
    public async Task FileOfTheCatalogueSizeIsDownloaded()
    {
        SkipOnAndroid();
        byte[] payload = Payload(100_000);
        string baseUrl = Serve(response =>
        {
            response.ContentLength64 = payload.Length;
            response.OutputStream.Write(payload, 0, payload.Length);
            response.Close();
        });

        var models = new EchoirModels(Registry(baseUrl, payload.Length));
        await models.DownloadAsync(ModelId, _dir, includeVad: false);

        string final = Path.Combine(_dir, FileName);
        Assert.IsTrue(File.Exists(final));
        Assert.AreEqual(payload.Length, new FileInfo(final).Length);
        Assert.IsFalse(File.Exists(final + ".part"));
    }

    [TestMethod]
    public async Task ShortBodyFailsAndLeavesNoFileOrPartFile()
    {
        SkipOnAndroid();
        byte[] payload = Payload(100_000);
        string baseUrl = Serve(response =>
        {
            // Chunked, so the client sees a complete body that is shorter than the catalogue says.
            response.SendChunked = true;
            response.OutputStream.Write(payload, 0, payload.Length - 5);
            response.Close();
        });

        var models = new EchoirModels(Registry(baseUrl, payload.Length));
        await Assert.ThrowsExceptionAsync<IOException>(
            () => models.DownloadAsync(ModelId, _dir, includeVad: false));

        string final = Path.Combine(_dir, FileName);
        Assert.IsFalse(File.Exists(final));
        Assert.IsFalse(File.Exists(final + ".part"));
    }

    [TestMethod]
    public async Task QwenModelDownloadsItsDefaultVad()
    {
        SkipOnAndroid();
        byte[] payload = Payload(100_000);
        string baseUrl = Serve(response =>
        {
            response.ContentLength64 = payload.Length;
            response.OutputStream.Write(payload, 0, payload.Length);
            response.Close();
        });

        var models = new EchoirModels(QwenRegistry(baseUrl, payload.Length));
        await models.DownloadAsync(QwenModelId, _dir);

        Assert.IsTrue(File.Exists(Path.Combine(_dir, FileName)));
        Assert.IsTrue(File.Exists(Path.Combine(_dir, VadFileName)));
        Assert.IsFalse(File.Exists(Path.Combine(_dir, VadFileName + ".part")));
    }

    // On the Android device the client cannot reach the local HttpListener (Connection failure),
    // so these run on desktop only. The download code itself is the same on every platform.
    private static void SkipOnAndroid()
    {
        if (OperatingSystem.IsAndroid())
        {
            Assert.Inconclusive("HttpListener on localhost is not reachable from the Android device.");
        }
    }

    private static byte[] Payload(int length)
    {
        var bytes = new byte[length];
        new Random(7).NextBytes(bytes);
        return bytes;
    }

    // Registry text with one model whose single file is under baseUrl and has the given size.
    private static string Registry(string baseUrl, long size) => $$"""
        {
          "version": 1,
          "default_vad": {},
          "models": [
            {
              "id": "{{ModelId}}",
              "backend": "whisper",
              "display": "Fake download",
              "files": [
                { "role": "model", "name": "{{FileName}}", "url": "{{baseUrl}}{{FileName}}", "size": {{size}} }
              ]
            }
          ]
        }
        """;

    // Registry text with a Qwen model (backend qwen3-asr) and its default VAD, both served under baseUrl.
    private static string QwenRegistry(string baseUrl, long size) => $$"""
        {
          "version": 1,
          "default_vad": { "qwen": "{{VadId}}" },
          "models": [
            {
              "id": "{{QwenModelId}}",
              "backend": "qwen3-asr",
              "display": "Fake Qwen download",
              "files": [
                { "role": "model", "name": "{{FileName}}", "url": "{{baseUrl}}{{FileName}}", "size": {{size}} }
              ]
            },
            {
              "id": "{{VadId}}",
              "backend": "vad",
              "display": "Fake VAD",
              "files": [
                { "role": "vad", "name": "{{VadFileName}}", "url": "{{baseUrl}}{{VadFileName}}", "size": {{size}} }
              ]
            }
          ]
        }
        """;

    // Starts a listener on a free local port. Each request is answered by respond, which must
    // close the response. Returns the base URL: a file is at it plus its name.
    private string Serve(Action<HttpListenerResponse> respond)
    {
        int port = FreePort();
        _listener = new HttpListener();
        _listener.Prefixes.Add($"http://localhost:{port}/");
        _listener.Start();

        HttpListener listener = _listener;
        _ = Task.Run(async () =>
        {
            while (listener.IsListening)
            {
                HttpListenerContext context;
                try
                {
                    context = await listener.GetContextAsync();
                }
                catch (Exception)
                {
                    return;
                }
                respond(context.Response);
            }
        });

        return $"http://localhost:{port}/";
    }

    private static int FreePort()
    {
        var probe = new TcpListener(IPAddress.Loopback, 0);
        probe.Start();
        int port = ((IPEndPoint)probe.LocalEndpoint).Port;
        probe.Stop();
        return port;
    }
}
