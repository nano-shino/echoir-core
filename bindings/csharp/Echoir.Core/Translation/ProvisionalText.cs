namespace Echoir.Core;

// One term and the way to write it in the target language, for TranscriptTranslator.Glossary.
public sealed record GlossaryEntry(string Term, string Translation);

// The glossary text that the translator takes: one "term<TAB>translation" pair per line.
public static class TranslationGlossary
{
    // Writes the entries in the format of echoir_translate_request.glossary. Tabs and line breaks
    // inside a term or a translation become spaces, the ends are trimmed, and an entry with a blank
    // part is left out. Returns null when no entry is left.
    public static string? Format(IEnumerable<GlossaryEntry>? entries)
    {
        if (entries is null)
        {
            return null;
        }
        var text = new System.Text.StringBuilder();
        foreach (GlossaryEntry entry in entries)
        {
            string term = Clean(entry.Term);
            string translation = Clean(entry.Translation);
            if (term.Length == 0 || translation.Length == 0)
            {
                continue;
            }
            text.Append(term).Append('\t').Append(translation).Append('\n');
        }
        return text.Length == 0 ? null : text.ToString();
    }

    private static string Clean(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }
        return string.Join(' ', value.Split(new[] { '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)).Trim();
    }
}

// Sentence splitting and comparison for the provisional translation of a PARTIAL.
internal static class SentenceText
{
    private const string Terminators = ".!?\u2026\u3002\uFF01\uFF1F";
    private const string Closers = "\"'\u2019\u201D)]\u300D\u300F\u00BB";

    // The sentences of text, in order, each with its end punctuation and closing quotes. The last
    // piece may be unfinished (the tail); a text that ends in a terminator has no unfinished piece,
    // and its last sentence is the last piece. A "." or "!" or "?" ends a sentence only before a
    // space or the end of the text (so "3.5" stays whole); "\u2026" and the CJK marks end one anywhere.
    public static List<string> Split(string text)
    {
        var pieces = new List<string>();
        if (string.IsNullOrWhiteSpace(text))
        {
            return pieces;
        }
        text = text.Trim();
        int start = 0;
        int i = 0;
        while (i < text.Length)
        {
            if (Terminators.IndexOf(text[i]) < 0)
            {
                i++;
                continue;
            }
            int run = i;
            while (run < text.Length && Terminators.IndexOf(text[run]) >= 0)
            {
                run++;
            }
            int end = run;
            while (end < text.Length && Closers.IndexOf(text[end]) >= 0)
            {
                end++;
            }
            bool wide = text[run - 1] > 0x7F;
            if (wide || end >= text.Length || char.IsWhiteSpace(text[end]))
            {
                string piece = text.Substring(start, end - start).Trim();
                if (HasLetterOrDigit(piece))
                {
                    pieces.Add(piece);
                    start = end;
                }
            }
            i = end;
        }
        string rest = text.Substring(start).Trim();
        if (rest.Length > 0)
        {
            pieces.Add(rest);
        }
        return pieces;
    }

    // The comparison key of a sentence: letters and digits only, in lower case. Whisper often
    // flips case and punctuation between two decodes of the same speech.
    public static string Key(string text)
    {
        var key = new System.Text.StringBuilder(text.Length);
        foreach (char c in text)
        {
            if (char.IsLetterOrDigit(c) || char.IsSurrogate(c))
            {
                key.Append(char.ToLowerInvariant(c));
            }
        }
        return key.ToString();
    }

    // Words of a text; in Japanese or Chinese text, two characters count as a word.
    public static double Words(string text)
    {
        bool cjk = false;
        int chars = 0;
        foreach (char c in text)
        {
            if (!char.IsWhiteSpace(c))
            {
                chars++;
                cjk |= (c >= 0x3040 && c <= 0x30FF) || (c >= 0x4E00 && c <= 0x9FFF);
            }
        }
        return cjk ? chars / 2.0 : text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).Length;
    }

    // Joins translated sentences: no space for languages that write without them.
    public static string Join(IEnumerable<string> parts, string target)
    {
        bool noSpace = target.StartsWith("ja", StringComparison.OrdinalIgnoreCase)
            || target.StartsWith("zh", StringComparison.OrdinalIgnoreCase)
            || target.StartsWith("yue", StringComparison.OrdinalIgnoreCase);
        return string.Join(noSpace ? "" : " ", parts);
    }

    private static bool HasLetterOrDigit(string text)
    {
        foreach (char c in text)
        {
            if (char.IsLetterOrDigit(c) || char.IsSurrogate(c))
            {
                return true;
            }
        }
        return false;
    }
}
