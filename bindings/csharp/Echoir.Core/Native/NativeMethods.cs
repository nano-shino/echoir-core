using System.Runtime.InteropServices;

namespace Echoir.Core.Native;

// The C ABI of echoir.h. Every signature is blittable; strings are passed as UTF-8 pointers (see
// Utf8String). Callers go through EchoirTranscriber and EchoirModels, not these.
internal static unsafe partial class NativeMethods
{
#if IOS
    // iOS links Echoir.xcframework statically into the app.
    private const string Library = "__Internal";
#else
    private const string Library = "echoir";
#endif

    [LibraryImport(Library, EntryPoint = "echoir_last_error")]
    internal static partial nint LastError();

    [LibraryImport(Library, EntryPoint = "echoir_session_params_init")]
    internal static partial void SessionParamsInit(SessionParams* p);

    [LibraryImport(Library, EntryPoint = "echoir_engine_free")]
    internal static partial void EngineFree(nint engine);

    [LibraryImport(Library, EntryPoint = "echoir_session_create")]
    internal static partial nint SessionCreate(nint engine, SessionParams* p, nint callback, nint user);

    [LibraryImport(Library, EntryPoint = "echoir_session_free")]
    internal static partial void SessionFree(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_poll")]
    internal static partial int SessionPoll(nint session, EventData* ev);

    [LibraryImport(Library, EntryPoint = "echoir_session_push_f32")]
    internal static partial int SessionPushF32(nint session, float* pcm, nuint frames, int sampleRate, int channels);

    [LibraryImport(Library, EntryPoint = "echoir_session_push_s16")]
    internal static partial int SessionPushS16(nint session, short* pcm, nuint frames, int sampleRate, int channels);

    [LibraryImport(Library, EntryPoint = "echoir_session_start")]
    internal static partial int SessionStart(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_start_capture")]
    internal static partial int SessionStartCapture(nint session, int source, nint deviceId);

    [LibraryImport(Library, EntryPoint = "echoir_session_stop")]
    internal static partial int SessionStop(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_flush")]
    internal static partial int SessionFlush(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_reset")]
    internal static partial int SessionReset(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_clear")]
    internal static partial int SessionClear(nint session);

    [LibraryImport(Library, EntryPoint = "echoir_session_read_audio")]
    internal static partial int SessionReadAudio(nint session, long startMs, long endMs, short* output, nuint cap, nuint* nFrames);

    [LibraryImport(Library, EntryPoint = "echoir_session_read_peaks")]
    internal static partial int SessionReadPeaks(nint session, long startMs, long endMs, int bucketMs, float* outMinMax, nuint cap, nuint* nBuckets);

    [LibraryImport(Library, EntryPoint = "echoir_session_set_input_muted")]
    internal static partial int SessionSetInputMuted(nint session, int muted);

    [LibraryImport(Library, EntryPoint = "echoir_devices_json")]
    internal static partial nint DevicesJson();

    [LibraryImport(Library, EntryPoint = "echoir_model_params_init")]
    internal static partial void ModelParamsInit(ModelParams* p);

    [LibraryImport(Library, EntryPoint = "echoir_engine_create_from_model")]
    internal static partial nint EngineCreateFromModel(ModelParams* p);

    [LibraryImport(Library, EntryPoint = "echoir_models_json")]
    internal static partial nint ModelsJson(nint registryJson, nint modelsDir);
}
