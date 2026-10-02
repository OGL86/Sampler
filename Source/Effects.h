#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Stereo-delay med feedback. Bufferne allokeres i prepare(), aldri i process().
class StereoDelay
{
public:
    void prepare (double sampleRate, float maxSeconds = 2.0f)
    {
        rate = sampleRate;
        size = (int) (sampleRate * maxSeconds) + 4;
        bufL.assign ((size_t) size, 0.0f);
        bufR.assign ((size_t) size, 0.0f);
        writePos = 0;
    }

    void process (float* left, float* right, int numSamples,
                  float timeMs, float feedback, float mix) noexcept
    {
        if (size == 0)
            return;

        const int delaySamples = juce::jlimit (1, size - 2, (int) (timeMs * 0.001 * rate));

        for (int i = 0; i < numSamples; ++i)
        {
            int readPos = writePos - delaySamples;
            if (readPos < 0)
                readPos += size;

            // Ping-pong: venstre feeder høyre og omvendt
            const float dl = bufL[(size_t) readPos];
            const float dr = bufR[(size_t) readPos];

            bufL[(size_t) writePos] = left[i]  + dr * feedback;
            bufR[(size_t) writePos] = right[i] + dl * feedback;

            left[i]  += dl * mix;
            right[i] += dr * mix;

            if (++writePos >= size)
                writePos = 0;
        }
    }

private:
    std::vector<float> bufL, bufR;
    double rate = 44100.0;
    int size = 0;
    int writePos = 0;
};
