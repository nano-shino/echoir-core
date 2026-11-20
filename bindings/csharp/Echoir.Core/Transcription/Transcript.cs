using System.Collections.ObjectModel;
using System.ComponentModel;

namespace Echoir.Core;

// One segment of text. A PARTIAL and the FINAL that replaces it share Id. Language is the ISO code
// of the language the model detected for the text ("en", "zh", "yue", ...), or the language the
// session was started with when it is not "auto"; empty when the model named none.
public sealed record Segment(long Id, long StartMs, long EndMs, string Text, string Language = "");

// The text of a session: the finished segments, and the segment in progress. Changes are raised
// on the context the transcriber captured at Start.
public sealed class Transcript : INotifyPropertyChanged
{
    private readonly ObservableCollection<Segment> _finals = new();
    private Segment? _partial;

    public Transcript()
    {
        Finals = new ReadOnlyObservableCollection<Segment>(_finals);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    // Finished segments, in order. Raises CollectionChanged.
    public ReadOnlyObservableCollection<Segment> Finals { get; }

    // The segment in progress, or null. Raises PropertyChanged.
    public Segment? Partial
    {
        get => _partial;
        private set
        {
            if (Equals(_partial, value))
            {
                return;
            }
            _partial = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Partial)));
        }
    }

    // A FINAL appends its text and clears the PARTIAL with the same id. A FINAL with no text only
    // clears that PARTIAL.
    internal void ApplyFinal(Segment segment)
    {
        if (Partial?.Id == segment.Id)
        {
            Partial = null;
        }
        if (segment.Text.Length > 0)
        {
            _finals.Add(segment);
        }
    }

    internal void ApplyPartial(Segment segment)
    {
        Partial = segment;
    }

    internal void ApplyCleared()
    {
        _finals.Clear();
        Partial = null;
    }
}
