using System.Runtime.InteropServices;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// Capture against the null audio backend, which has one silent device and needs no hardware.
[TestClass]
public sealed class CaptureTests
{
    private const string BackendVariable = "ECHOIR_AUDIO_BACKEND";

    [TestMethod]
    public async Task NullBackendListsOneDeviceAndStartsAndStopsMicCapture()
    {
        if (!TestPaths.WhisperModelsPresent)
        {
            Assert.Inconclusive("whisper-tiny-en and its VAD are not in models/.");
        }

        string? previous = Environment.GetEnvironmentVariable(BackendVariable);
        SetEnvironment(BackendVariable, "null");
        try
        {
            AudioDevices devices = EchoirTranscriber.Devices();
            Assert.AreEqual(1, devices.Capture.Count);

            await using var transcriber = new EchoirTranscriber();
            await transcriber.LoadAsync("whisper-tiny-en", TestPaths.ModelsDir);
            transcriber.Start(AudioSource.Mic, devices.Capture[0].Id);
            Assert.IsTrue(transcriber.IsRunning);

            await transcriber.StopAsync();
            Assert.IsFalse(transcriber.IsRunning);
        }
        finally
        {
            SetEnvironment(BackendVariable, previous);
        }
    }

    // The native library reads the C runtime's environment. On Windows, .NET's SetEnvironmentVariable
    // changes only the Win32 copy, so the value is set through the UCRT as well.
    private static void SetEnvironment(string name, string? value)
    {
        Environment.SetEnvironmentVariable(name, value);
#if ANDROID
        // Android's C runtime is bionic, which .NET's SetEnvironmentVariable does not update.
        if (value is null)
        {
            Android.Systems.Os.Unsetenv(name);
        }
        else
        {
            Android.Systems.Os.Setenv(name, value, true);
        }
#else
        if (OperatingSystem.IsWindows())
        {
            _putenv_s(name, value ?? string.Empty);
        }
#endif
    }

    [DllImport("ucrtbase.dll", EntryPoint = "_putenv_s", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern int _putenv_s(string name, string value);
}
