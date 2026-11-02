using System.Runtime.InteropServices;
using Echoir.Core.Native;

namespace Echoir.Core;

// Result of a native call, mirroring echoir_status.
public enum EchoirStatus
{
    Ok = 0,
    InvalidArgument,
    ModelLoad,
    Backend,
    State,
    NotImplemented,
    Internal,
}

// A native call failed. Message is the core's explanation (echoir_last_error).
public sealed class EchoirException : Exception
{
    public EchoirException(EchoirStatus status, string message) : base(message)
    {
        Status = status;
    }

    public EchoirStatus Status { get; }

    // The error of the calling thread's last failed native call. Read it right after the call.
    internal static EchoirException FromLastError(EchoirStatus status, string call)
    {
        string? text = Marshal.PtrToStringUTF8(NativeMethods.LastError());
        return new EchoirException(status, string.IsNullOrEmpty(text) ? call + " failed" : text);
    }

    // The error of the calling thread's last failed native call, with the status the core recorded
    // for it (echoir_last_status). For calls that return a pointer, which report only NULL. Read it
    // right after the call.
    internal static EchoirException FromLastFailure(string call)
    {
        var status = (EchoirStatus)NativeMethods.LastStatus();
        return FromLastError(status == EchoirStatus.Ok ? EchoirStatus.Internal : status, call);
    }

    // Throws when a native call returned a status other than ECHOIR_OK.
    internal static void ThrowIfFailed(int status, string call)
    {
        if (status != NativeValues.StatusOk)
        {
            throw FromLastError((EchoirStatus)status, call);
        }
    }
}
