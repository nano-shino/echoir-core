using System.Buffers.Binary;
using System.Text;

namespace Echoir.Core.Tests;

// Reads 16-bit PCM WAV files as mono floats in [-1, 1]. Stereo is averaged to mono.
internal static class WavFile
{
    public static (float[] Samples, int SampleRate) ReadMono(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length < 12
            || Encoding.ASCII.GetString(bytes, 0, 4) != "RIFF"
            || Encoding.ASCII.GetString(bytes, 8, 4) != "WAVE")
        {
            throw new InvalidDataException($"{path} is not a RIFF/WAVE file.");
        }

        int channels = 0;
        int sampleRate = 0;
        int bitsPerSample = 0;
        int format = 0;
        int dataStart = -1;
        int dataLength = 0;

        int pos = 12;
        while (pos + 8 <= bytes.Length)
        {
            string id = Encoding.ASCII.GetString(bytes, pos, 4);
            int size = BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(pos + 4));
            int body = pos + 8;
            if (id == "fmt ")
            {
                format = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(body));
                channels = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(body + 2));
                sampleRate = BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(body + 4));
                bitsPerSample = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(body + 14));
            }
            else if (id == "data")
            {
                dataStart = body;
                dataLength = Math.Min(size, bytes.Length - body);
                break;
            }
            pos = body + size + (size & 1);
        }

        if (format != 1 || bitsPerSample != 16 || channels <= 0 || dataStart < 0)
        {
            throw new InvalidDataException($"{path} must be 16-bit PCM with a data chunk.");
        }

        int frames = dataLength / (2 * channels);
        var samples = new float[frames];
        for (int frame = 0; frame < frames; frame++)
        {
            float sum = 0;
            for (int c = 0; c < channels; c++)
            {
                int offset = dataStart + (frame * channels + c) * 2;
                sum += BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(offset)) / 32768f;
            }
            samples[frame] = sum / channels;
        }
        return (samples, sampleRate);
    }
}
