#include "DiskStreamer.h"

DiskStreamer::DiskStreamer() : juce::Thread ("Sampler disk streamer")
{
    for (auto& s : slots)
        s.allocate();

    temp.setSize (2, StreamSlot::chunkFrames);
}

DiskStreamer::~DiskStreamer()
{
    stopStreaming();
}

void DiskStreamer::startStreaming()
{
    if (! isThreadRunning())
        startThread();
}

void DiskStreamer::stopStreaming()
{
    stopThread (2000);
}

void DiskStreamer::run()
{
    while (! threadShouldExit())
    {
        bool didWork = false;

        for (auto& s : slots)
        {
            if (threadShouldExit())
                return;

            didWork |= serviceSlot (s);
        }

        if (! didWork)
            wait (2);
    }
}

bool DiskStreamer::serviceSlot (StreamSlot& s)
{
    // Lås først, last deretter sample-pekeren: da kan ikke meldingstråden
    // slette samplet mellom at vi leser pekeren og at vi bruker den.
    const juce::ScopedLock lock (readerLock);

    const Sample* smp = s.sample.load (std::memory_order_acquire);

    if (smp == nullptr || ! smp->streamed || smp->reader == nullptr)
        return false;

    const auto st = s.state.load (std::memory_order_acquire);
    const auto writeFrame = StreamSlot::writeFrameOf (st);

    if (writeFrame >= smp->totalFrames)
        return false;

    // Behold litt bak lesehodet for interpolasjon
    const auto oldestNeeded = juce::jmax<std::int64_t> (s.readFrame.load (std::memory_order_relaxed) - 4,
                                                        preloadFrames);

    if (StreamSlot::ringFrames - (writeFrame - oldestNeeded) < StreamSlot::chunkFrames)
        return false; // ringen er full nok

    const int n = (int) juce::jmin<std::int64_t> (StreamSlot::chunkFrames, smp->totalFrames - writeFrame);
    float* dest[2] = { temp.getWritePointer (0), temp.getWritePointer (1) };

    smp->reader->read (dest, smp->numChannels, writeFrame, n);

    const int channels = smp->numChannels;

    for (int ch = 0; ch < channels; ++ch)
    {
        float* ring = s.ring[ch].data();
        const float* src = dest[ch];

        for (int i = 0; i < n; ++i)
            ring[(writeFrame + i) & (StreamSlot::ringFrames - 1)] = src[i];
    }

    // Publiser. Feiler hvis stemmen ble retriggret mens vi leste: da forkastes arbeidet.
    auto expected = st;
    s.state.compare_exchange_strong (expected,
                                     StreamSlot::pack (StreamSlot::generationOf (st), writeFrame + n),
                                     std::memory_order_release,
                                     std::memory_order_relaxed);
    return true;
}
