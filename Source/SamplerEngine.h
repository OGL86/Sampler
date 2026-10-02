#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Biquad.h"
#include "DiskStreamer.h"
#include "Effects.h"
#include "Instrument.h"
#include "Params.h"

class SamplerEngine
{
public:
    static constexpr int numVoices = DiskStreamer::numSlots;

    SamplerEngine();
    ~SamplerEngine();

    // Parametere. GUI skriver, lydtråden leser.
    Params params;

    // ---- Meldingstråd ----
    void prepare (double deviceSampleRate, int maxBlockSize);

    // Publiserer et nytt instrument til lydtråden. Eierskap overføres.
    void loadInstrument (std::unique_ptr<Instrument> instrument);

    // Frigjør instrumenter lydtråden er ferdig med. Kall med jevne mellomrom (f.eks. fra en Timer).
    void collectGarbage();

    // Gjeldende instrument (kun trygt å bruke på meldingstråden).
    const Instrument* getInstrument() const noexcept { return current.load (std::memory_order_acquire); }

    // Topp-nivå siden forrige kall, for nivåmåler.
    float fetchPeakLevel() noexcept { return peakLevel.exchange (0.0f, std::memory_order_relaxed); }

    // ---- Lydtråd. Ingen allokering, ingen låser, ingen I/O. ----
    void render (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi);

private:
    struct Voice
    {
        bool active = false;
        bool releasing = false;    // note-off mottatt
        bool sustainHeld = false;  // note-off mottatt mens pedalen var nede
        int note = -1;
        std::uint64_t age = 0;

        const Sample* sample = nullptr;
        double position = 0.0;
        double baseIncrement = 1.0;
        float gain = 1.0f;         // velocity og zone-volum
        float zonePan = 0.0f;

        bool looping = false;
        double loopStart = 0.0, loopEnd = 0.0;
        double xfadeFrames = 0.0;

        juce::ADSR env;
        Biquad filter[2];
    };

    struct BlockState
    {
        juce::ADSR::Parameters adsr;
        float velocitySens = 0.7f;
        int filterType = 0;
        float cutoff = 12000.0f, resonance = 0.707f;
        float pan = 0.0f;
        float lfoRate = 4.0f, lfoPitchCents = 0.0f, lfoFilterOct = 0.0f, lfoAmp = 0.0f;
    };

    void handleMessage (const juce::MidiMessage& m);
    void noteOn (int note, int velocity);
    void noteOff (int note);
    void setSustain (bool down);
    void killAllVoices();
    Voice* allocateVoice();
    void startVoice (Voice& v, int index, const Zone& zone, int note, int velocity);

    void snapshotParams();
    void renderRange (juce::AudioBuffer<float>& out, int start, int count);
    void renderSegment (juce::AudioBuffer<float>& out, int start, int count);
    void renderVoice (Voice& v, int index, int count, float* outL, float* outR);
    void applyEffects (juce::AudioBuffer<float>& out);

    double deviceRate = 44100.0;
    int maxBlock = 512;
    std::uint64_t voiceCounter = 0;
    bool sustainDown = false;
    float bendFactor = 1.0f;
    int roundRobin[128] = {};

    std::vector<Voice> voices;
    DiskStreamer streamer;

    BlockState block;
    double lfoPhase = 0.0;
    float lastMaster = 0.8f;

    // Arbeidsbuffere (allokert i prepare)
    std::vector<float> voiceL, voiceR, gainBuf, lfoBuf, scratchR;

    // Effekter
    StereoDelay delay;
    juce::Reverb reverb;

    // Instrument-utveksling mellom meldingstråd og lydtråd
    std::atomic<const Instrument*> current { nullptr };
    std::atomic<int> ackEpoch { 0 };
    const Instrument* audioInstrument = nullptr; // lydtrådens kopi
    int epochCounter = 0;
    std::vector<std::unique_ptr<Instrument>> instruments; // meldingstråd

    std::atomic<float> peakLevel { 0.0f };
};
