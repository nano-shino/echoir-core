using System.Buffers.Binary;

namespace Echoir.Core;

// One column of a level envelope: the lowest and the highest sample of a bucket, at full scale
// ±1.
public readonly record struct Peak(float Min, float Max);

// Helpers for the audio that TryReadAudio returns.
public static class AudioClip
{
    private const int HeaderSize = 44;

    // Encodes 16-bit mono samples as a canonical RIFF/WAVE file (44-byte header, PCM, little
    // endian).
    public static byte[] ToWav(ReadOnlySpan<short> samples, int sampleRate = 16000)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(sampleRate);
        if (samples.Length > (int.MaxValue - HeaderSize) / 2)
        {
            throw new ArgumentException("The clip is too long for a WAVE file.", nameof(samples));
        }

        int dataSize = samples.Length * 2;
        var wav = new byte[HeaderSize + dataSize];
        Span<byte> header = wav.AsSpan(0, HeaderSize);
        "RIFF"u8.CopyTo(header[0..]);
        BinaryPrimitives.WriteInt32LittleEndian(header[4..], HeaderSize - 8 + dataSize);
        "WAVE"u8.CopyTo(header[8..]);
        "fmt "u8.CopyTo(header[12..]);
        BinaryPrimitives.WriteInt32LittleEndian(header[16..], 16);
        BinaryPrimitives.WriteInt16LittleEndian(header[20..], 1);
        BinaryPrimitives.WriteInt16LittleEndian(header[22..], 1);
        BinaryPrimitives.WriteInt32LittleEndian(header[24..], sampleRate);
        BinaryPrimitives.WriteInt32LittleEndian(header[28..], sampleRate * 2);
        BinaryPrimitives.WriteInt16LittleEndian(header[32..], 2);
        BinaryPrimitives.WriteInt16LittleEndian(header[34..], 16);
        "data"u8.CopyTo(header[36..]);
        BinaryPrimitives.WriteInt32LittleEndian(header[40..], dataSize);

        for (int i = 0; i < samples.Length; i++)
        {
            BinaryPrimitives.WriteInt16LittleEndian(wav.AsSpan(HeaderSize + 2 * i), samples[i]);
        }
        return wav;
    }
}
