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

// Values of echoir_status, echoir_event_type and echoir_source.
internal static class NativeValues
{
    internal const int StatusOk = 0;

    internal const int EventPartial = 1;
    internal const int EventFinal = 2;
    internal const int EventCleared = 3;
    internal const int EventError = 4;

    internal const int SourceMic = 1;
    internal const int SourceLoopback = 2;
}
