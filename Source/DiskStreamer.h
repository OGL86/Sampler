#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "Instrument.h"

// Ringbuffer per stemme for samples som strømmes fra disk.
//
// Lydtråden eier stemmen og nullstiller sporet ved note-on. Disk-tråden fyller
// ringbufferet fremover. All kommunikasjon går via atomics:
//   state = (generasjon << 40) | writeFrame
// Disk-tråden publiserer med compare_exchange, så en skrivning som startet for
// en gammel note blir forkastet hvis stemmen er retriggret underveis.
struct StreamSlot
{
    static constexpr int ringFrames = 1 << 15; // 32768 frames (~0.74 s ved 44.1 kHz)
    static constexpr int chunkFrames = 4096;

    std::vector<float> ring[2];
    std::atomic<std::uint64_t> state { 0 };
    std::atomic<std::int64_t> readFrame { 0 };      // lydtrådens posisjon, brukes til å vite hvor mye plass som er ledig
    std::atomic<const Sample*> sample { nullptr };  // nullptr = sporet er ledig

    void allocate()
    {
        ring[0].assign ((size_t) ringFrames, 0.0f);
        ring[1].assign ((size_t) ringFrames, 0.0f);
    }

    static std::uint64_t pack (std::uint64_t generation, std::int64_t writeFrame) noexcept
    {
        return (generation << 40) | (std::uint64_t) writeFrame;
    }

    static std::int64_t writeFrameOf (std::uint64_t s) noexcept   { return (std::int64_t) (s & ((1ull << 40) - 1)); }
    static std::uint64_t generationOf (std::uint64_t s) noexcept  { return s >> 40; }

    // Lydtråd: start strømming for en ny note. Starter etter preload-delen.
    void start (const Sample* s) noexcept
    {
        const auto generation = generationOf (state.load (std::memory_order_relaxed)) + 1;
        readFrame.store (0, std::memory_order_relaxed);
        sample.store (s, std::memory_order_release);
        state.store (pack (generation, preloadFrames), std::memory_order_release);
    }

    // Lydtråd: frigjør sporet
    void stop() noexcept { sample.store (nullptr, std::memory_order_release); }
};

class DiskStreamer : private juce::Thread
{
public:
    static constexpr int numSlots = 64;

    DiskStreamer();
    ~DiskStreamer() override;

    void startStreaming();
    void stopStreaming();

    StreamSlot& slot (int index) noexcept { return slots[(size_t) index]; }

    // Meldingstråd: hold denne rundt sletting av samples som disk-tråden kan bruke.
    juce::CriticalSection& getReaderLock() noexcept { return readerLock; }

private:
    void run() override;
    bool serviceSlot (StreamSlot& s);

    std::array<StreamSlot, numSlots> slots;
    juce::CriticalSection readerLock;
    juce::AudioBuffer<float> temp;
};
