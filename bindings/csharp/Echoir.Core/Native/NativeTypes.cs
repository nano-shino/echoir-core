using System.Runtime.InteropServices;

namespace Echoir.Core.Native;

// Mirrors of the structs in echoir.h, in the same field order. The native structs use the C
// default alignment, which these sequential layouts match on every 64-bit target we ship.

[StructLayout(LayoutKind.Sequential)]
internal struct SessionParams
{
    public uint StructSize;
    public nint Language;
    public int MinChunkMs;
    public int MaxSegmentMs;
    public int EmitPartials;
    public int StreamPolicy;
    public int RetainAudioMs;
    public nint Tape;
    public nint TapeTrack;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ModelParams
{
    public uint StructSize;
    public nint RegistryJson;
    public nint ModelsDir;
    public nint ModelId;
    public nint VadId;
    public int UseGpu;
    public int NThreads;
}

[StructLayout(LayoutKind.Sequential)]
internal struct EventData
{
    public int Type;
    public long SegmentId;
    public long StartMs;
    public long EndMs;
    public nint Text;
    public nint Detail;
}

[StructLayout(LayoutKind.Sequential)]
internal struct LagData
{
    public uint StructSize;
    public long HeadMs;
    public long DecodedMs;
    public long BacklogMs;
    public long TickMs;
}

// Values of echoir_status, echoir_event_type, echoir_source and echoir_capture_state.
internal static class NativeValues
{
    internal const int StatusOk = 0;

    internal const int EventPartial = 1;
    internal const int EventFinal = 2;
    internal const int EventCleared = 3;
    internal const int EventError = 4;
    internal const int EventStats = 5;

    internal const int StatusInvalidArgument = 1;
    internal const int StatusState = 4;

    internal const int SourceMic = 1;
    internal const int SourceLoopback = 2;

    internal const int CaptureOff = 0;
    internal const int CaptureLive = 1;
    internal const int CaptureWaiting = 2;
    internal const int CaptureEnded = 3;
}
