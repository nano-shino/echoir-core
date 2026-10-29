using System.Collections.Concurrent;

namespace Echoir.Core.Tests;

// A SynchronizationContext whose posted work runs only when the test thread pumps it. It stands
// in for a UI thread: the transcriber's events must arrive here, on the pumping thread.
internal sealed class PumpContext : SynchronizationContext
{
    private readonly BlockingCollection<(SendOrPostCallback Callback, object? State)> _queue = new();

    public override void Post(SendOrPostCallback d, object? state)
    {
        _queue.Add((d, state));
    }

    // Runs posted work on the calling thread until the condition holds.
    public void PumpUntil(Func<bool> condition, TimeSpan timeout)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                throw new TimeoutException("The condition did not hold before the timeout.");
            }
            if (_queue.TryTake(out var item, 10))
            {
                item.Callback(item.State);
            }
        }
    }
}
