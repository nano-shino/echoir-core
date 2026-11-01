using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Echoir.Core.Tests;

// The lag verdict from backlog readings alone; no native library is needed.
[TestClass]
public sealed class LagMonitorTests
{
    private const int MinChunkMs = 500;
    private const int StepMs = 33;

    [TestMethod]
    public void SawtoothThatKeepsUpIsLive()
    {
        var monitor = new LagMonitor(MinChunkMs);
        // Each tick: the backlog climbs to min_chunk plus a 300 ms decode, then drops to 0.
        TranscriberLag lag = Feed(monitor, 0, 20_000, t => t % 800);
        Assert.AreEqual(LagState.Live, lag.State);
        Assert.IsTrue(Math.Abs(lag.Trend) < 0.02, $"Trend {lag.Trend} for a steady sawtooth.");
        Assert.AreEqual(0, monitor.BehindShare);
    }

    [TestMethod]
    public void SteadyBacklogOfAFewSecondsIsStrained()
    {
        var monitor = new LagMonitor(MinChunkMs);
        TranscriberLag lag = Feed(monitor, 0, 12_000, _ => 2500);
        Assert.AreEqual(LagState.Strained, lag.State);
        Assert.AreEqual(TimeSpan.FromMilliseconds(2500), lag.Backlog);
    }

    [TestMethod]
    public void LargeBacklogIsBehind()
    {
        var monitor = new LagMonitor(MinChunkMs);
        TranscriberLag lag = Feed(monitor, 0, 3000, _ => 6000);
        Assert.AreEqual(LagState.Behind, lag.State);
    }

    [TestMethod]
    public void GrowingBacklogIsBehindBeforeItIsLarge()
    {
        var monitor = new LagMonitor(MinChunkMs);
        // Decoding 1.2 times slower than the audio: the backlog gains 0.2 s per second, from 0.5 s.
        TranscriberLag lag = Feed(monitor, 0, 8000, t => 500 + t / 5);
        Assert.AreEqual(LagState.Behind, lag.State);
        Assert.IsTrue(lag.Trend > 0.15 && lag.Trend < 0.25, $"Trend {lag.Trend}.");
        Assert.IsTrue(lag.Backlog < TimeSpan.FromMilliseconds(MinChunkMs + LagMonitor.BehindMarginMs));
    }

    [TestMethod]
    public void WorseStateIsHeldBrieflyThenEases()
    {
        var monitor = new LagMonitor(MinChunkMs);
        Feed(monitor, 0, 1000, _ => 6000);
        Assert.AreEqual(LagState.Behind, monitor.Current.State);
        // Caught up: the peak window (2 s) and the hold (2 s) keep Behind for a moment.
        Assert.AreEqual(LagState.Behind, Feed(monitor, 1000, 2500, _ => 0).State);
        Assert.AreEqual(LagState.Live, Feed(monitor, 2500, 8000, _ => 0).State);
        Assert.AreEqual(TimeSpan.FromMilliseconds(6000), monitor.MaxBacklog);
        Assert.IsTrue(monitor.BehindShare > 0.3 && monitor.BehindShare < 0.7, $"Behind share {monitor.BehindShare}.");
    }

    [TestMethod]
    public void TypicalBacklogLeavesOutAShortSpike()
    {
        var monitor = new LagMonitor(MinChunkMs);
        Assert.AreEqual(TimeSpan.Zero, monitor.TypicalBacklog);
        // 30 s of a healthy sawtooth up to 0.9 s, with a 2 s spike to 8 s in the middle.
        Feed(monitor, 0, 14_000, t => 400 + t % 500);
        Feed(monitor, 14_000, 16_000, _ => 8000);
        Feed(monitor, 16_000, 30_000, t => 400 + t % 500);
        Assert.AreEqual(TimeSpan.FromMilliseconds(8000), monitor.MaxBacklog);
        Assert.IsTrue(monitor.TypicalBacklog < TimeSpan.FromSeconds(1), $"Typical {monitor.TypicalBacklog}.");

        // A backlog that stays high for most of the run is typical.
        var slow = new LagMonitor(MinChunkMs);
        Feed(slow, 0, 30_000, _ => 5000);
        Assert.AreEqual(TimeSpan.FromMilliseconds(5000), slow.TypicalBacklog);
    }

    [TestMethod]
    public void StatsParseReadsEveryField()
    {
        TranscriberStats? stats = TranscriberStats.Parse(
            """{"rtf":0.12,"decode_ms":180,"window_ms":2600,"ticks":10,"new_ms":1000,"speech_ms":800,"decode_max_ms":400,"load":0.18,"speech_rtf":0.2,"backend":"qwen3-asr"}""");
        Assert.IsNotNull(stats);
        Assert.AreEqual(0.12, stats.Rtf, 1e-9);
        Assert.AreEqual(0.18, stats.Load, 1e-9);
        Assert.AreEqual(0.2, stats.SpeechRtf, 1e-9);
        Assert.AreEqual(10, stats.Ticks);
        Assert.AreEqual(180, stats.DecodeMs);
        Assert.AreEqual(2600, stats.WindowMs);
        Assert.AreEqual(1000, stats.NewMs);
        Assert.AreEqual(800, stats.SpeechMs);
        Assert.AreEqual(400, stats.DecodeMaxMs);
        Assert.AreEqual("qwen3-asr", stats.Backend);
    }

    [TestMethod]
    public void StatsParseKeepsMissingFieldsAtZeroAndRejectsNonObjects()
    {
        TranscriberStats? old = TranscriberStats.Parse("""{"rtf":0.05,"decode_ms":10,"window_ms":900,"backend":"whisper"}""");
        Assert.IsNotNull(old);
        Assert.AreEqual(0, old.Load);
        Assert.AreEqual(0, old.SpeechMs);
        Assert.AreEqual("whisper", old.Backend);
        Assert.IsNull(TranscriberStats.Parse("[1,2]"));
        Assert.IsNull(TranscriberStats.Parse("not json"));
    }

    // Feeds a reading every StepMs over [fromMs, toMs), with the backlog given by the time.
    private static TranscriberLag Feed(LagMonitor monitor, long fromMs, long toMs, Func<long, long> backlogAt)
    {
        TranscriberLag lag = monitor.Current;
        for (long t = fromMs; t < toMs; t += StepMs)
        {
            lag = monitor.Add(t, backlogAt(t));
        }
        return lag;
    }
}
