using Microsoft.Win32.SafeHandles;

namespace Echoir.Core.Native;

// Owns an echoir_engine. Freed after every session created from it.
internal sealed class EngineHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public EngineHandle(nint engine) : base(ownsHandle: true)
    {
        SetHandle(engine);
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.EngineFree(handle);
        return true;
    }
}

// Owns an echoir_session. It holds a reference to its engine, so the engine handle is only
// released after the session is gone. Teardown: stop, then session free, then engine free.
internal sealed class SessionHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private EngineHandle? _engine;

    public SessionHandle(EngineHandle engine, nint session) : base(ownsHandle: true)
    {
        bool added = false;
        engine.DangerousAddRef(ref added);
        _engine = engine;
        SetHandle(session);
    }

    protected override bool ReleaseHandle()
    {
        // Stopping a started session first makes the free safe. A stopped session ignores this.
        NativeMethods.SessionStop(handle);
        NativeMethods.SessionFree(handle);
        _engine?.DangerousRelease();
        _engine = null;
        return true;
    }
}

// Owns an echoir_translator. A translate call holds a reference, so a Dispose during a call frees
// the translator when the call returns.
internal sealed class TranslatorHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public TranslatorHandle(nint translator) : base(ownsHandle: true)
    {
        SetHandle(translator);
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.TranslatorFree(handle);
        return true;
    }
}

// Owns an echoir_tape. Sessions that write the tape hold their own native reference, so the
// handle may be released before them.
internal sealed class TapeHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public TapeHandle(nint tape) : base(ownsHandle: true)
    {
        SetHandle(tape);
    }

    protected override bool ReleaseHandle()
    {
        // Closes the tape first if it is still open.
        NativeMethods.TapeFree(handle);
        return true;
    }
}
