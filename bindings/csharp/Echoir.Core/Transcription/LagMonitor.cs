namespace Echoir.Core;

// Turns readings of the session backlog into a LagState. EchoirTranscriber feeds it about 30 times
// a second while a session runs; a host may also feed it readings of its own.
//
// While a decoder keeps up, the backlog is a sawtooth: it climbs to min_chunk_ms plus one decode,
// then drops when the decode ends. So the verdict reads the peak of the last two seconds against
// that floor, and the slope of the last ten seconds, which is near zero for a sawtooth and
// positive when the decoder falls behind. Not thread-safe.
public sealed class LagMonitor
{
    // A backlog peak this far above min_chunk_ms is Strained, and this far is Behind.
    public const int StrainedMarginMs = 1500;
    public const int BehindMarginMs = 4000;

    // A slope above these (backlog ms per ms) is Strained or Behind, once the window spans
    // TrendMinSpanMs and the peak is past a smaller margin.
    public const double StrainedTrend = 0.05;
    public const double BehindTrend = 0.10;
    private const int TrendStrainedMarginMs = 500;
    private const int TrendBehindMarginMs = 1000;

    private const int PeakWindowMs = 2000;
    private const int TrendWindowMs = 10_000;
    private const int TrendMinSpanMs = 5000;

    // A worse state is shown at least this long, so the display does not flicker.
    private const int HoldMs = 2000;

    private const int SecondMs = 1000;

    private readonly Queue<(long AtMs, long BacklogMs)> _samples = new();
    private readonly List<long> _secondPeaks = new();
    private long _secondStartMs = long.MinValue;
    private long _secondPeakMs;
    private readonly int _floorMs;
    private LagState _state = LagState.Live;
    private long _heldUntilMs;
    private long? _lastAtMs;
    private long _runMs;
    private long _behindMs;

    public LagMonitor(int minChunkMs)
    {
        _floorMs = Math.Max(0, minChunkMs);
    }

    public TranscriberLag Current { get; private set; } =
        new(LagState.Live, TimeSpan.Zero, TimeSpan.Zero, 0, TimeSpan.Zero);

    // The largest backlog seen.
    public TimeSpan MaxBacklog { get; private set; }

    // The 90th percentile of the backlog peak of each second so far: how far behind the captions
    // were for most of the run, with a short busy spell left out. Zero before the first reading.
    public TimeSpan TypicalBacklog
    {
        get
        {
            var peaks = new List<long>(_secondPeaks);
            if (_secondStartMs != long.MinValue)
            {
                peaks.Add(_secondPeakMs);
            }
            if (peaks.Count == 0)
            {
                return TimeSpan.Zero;
            }
            peaks.Sort();
            int index = Math.Clamp((int)Math.Ceiling(0.9 * peaks.Count) - 1, 0, peaks.Count - 1);
            return TimeSpan.FromMilliseconds(peaks[index]);
        }
    }

    // The share of the time between the first and the last reading spent Behind.
    public double BehindShare => _runMs > 0 ? (double)_behindMs / _runMs : 0;

    // Adds a reading taken at atMs (any monotonic clock in ms) and returns the new verdict.
    public TranscriberLag Add(long atMs, long backlogMs, long tickMs = 0)
    {
        if (_lastAtMs is long last && atMs > last)
        {
            long step = atMs - last;
            _runMs += step;
            if (_state == LagState.Behind)
            {
                _behindMs += step;
            }
        }
        _lastAtMs = atMs;

        backlogMs = Math.Max(0, backlogMs);
        _samples.Enqueue((atMs, backlogMs));
        while (_samples.Count > 0 && _samples.Peek().AtMs < atMs - TrendWindowMs)
        {
            _samples.Dequeue();
        }
        if (backlogMs > MaxBacklog.TotalMilliseconds)
        {
            MaxBacklog = TimeSpan.FromMilliseconds(backlogMs);
        }
        if (_secondStartMs == long.MinValue || atMs >= _secondStartMs + SecondMs)
        {
            if (_secondStartMs != long.MinValue)
            {
                _secondPeaks.Add(_secondPeakMs);
            }
            _secondStartMs = atMs;
            _secondPeakMs = backlogMs;
        }
        else
        {
            _secondPeakMs = Math.Max(_secondPeakMs, backlogMs);
        }

        long peak = 0;
        foreach ((long at, long value) in _samples)
        {
            if (at >= atMs - PeakWindowMs && value > peak)
            {
                peak = value;
            }
        }
        double trend = Slope(out long spanMs);
        bool trendKnown = spanMs >= TrendMinSpanMs;

        LagState verdict = LagState.Live;
        if (peak > _floorMs + BehindMarginMs
            || (trendKnown && trend > BehindTrend && peak > _floorMs + TrendBehindMarginMs))
        {
            verdict = LagState.Behind;
        }
        else if (peak > _floorMs + StrainedMarginMs
            || (trendKnown && trend > StrainedTrend && peak > _floorMs + TrendStrainedMarginMs))
        {
            verdict = LagState.Strained;
        }

        if (verdict >= _state)
        {
            _state = verdict;
            _heldUntilMs = atMs + HoldMs;
        }
        else if (atMs >= _heldUntilMs)
        {
            _state = verdict;
        }

        Current = new TranscriberLag(
            _state,
            TimeSpan.FromMilliseconds(peak),
            TimeSpan.FromMilliseconds(backlogMs),
            trend,
            TimeSpan.FromMilliseconds(Math.Max(0, tickMs)));
        return Current;
    }

    // Least-squares slope of the backlog over the window, in ms per ms. spanMs is the time the
    // window covers.
    private double Slope(out long spanMs)
    {
        spanMs = 0;
        int n = _samples.Count;
        if (n < 2)
        {
            return 0;
        }
        long first = _samples.Peek().AtMs;
        double sumX = 0, sumY = 0, sumXx = 0, sumXy = 0;
        long lastAt = first;
        foreach ((long at, long value) in _samples)
        {
            double x = at - first;
            sumX += x;
            sumY += value;
            sumXx += x * x;
            sumXy += x * value;
            lastAt = at;
        }
        spanMs = lastAt - first;
        double denominator = n * sumXx - sumX * sumX;
        return denominator > 0 ? (n * sumXy - sumX * sumY) / denominator : 0;
    }
}
