using System.Runtime.InteropServices;

namespace Echoir.Core.Native;

// A NUL-terminated UTF-8 copy of a string, for one native call. Dispose it after the call; the
// native side reads it only during the call. A null string gives a null pointer.
internal readonly struct Utf8String : IDisposable
{
    private readonly nint _pointer;

    private Utf8String(nint pointer)
    {
        _pointer = pointer;
    }

    public nint Pointer => _pointer;

    public static Utf8String Create(string? text)
    {
        return new Utf8String(text is null ? 0 : Marshal.StringToCoTaskMemUTF8(text));
    }

    public void Dispose()
    {
        if (_pointer != 0)
        {
            Marshal.FreeCoTaskMem(_pointer);
        }
    }
}
