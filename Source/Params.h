#pragma once

#include <atomic>
#include <vector>

// Alle parametere som GUI kan endre mens lyden går. Lydtråden leser
// verdiene med relaxed atomics; ingen låser.
struct AtomicFloat
{
    AtomicFloat (float v = 0.0f) : value (v) {}
    float get() const noexcept          { return value.load (std::memory_order_relaxed); }
    void set (float v) noexcept         { value.store (v, std::memory_order_relaxed); }
    std::atomic<float> value;
};

// Beskrivelse av én parameter. Brukes til å bygge GUI og presets automatisk.
struct ParamInfo
{
    const char* id;
    const char* label;
    const char* section;
    AtomicFloat* value;
    float min, max, def;
    float midpoint; // verdi som havner midt på knotten (gir logaritmisk følelse), <= min betyr lineær
    float step;
    const char* suffix;
};

struct Params
{
    // Amplitude-envelope (sekunder, sustain 0..1)
    AtomicFloat attack  { 0.005f };
    AtomicFloat decay   { 0.20f };
    AtomicFloat sustain { 0.80f };
    AtomicFloat release { 0.25f };

    // Velocity: 0 = ingen følsomhet (alltid full), 1 = full kurve (v^2)
    AtomicFloat velocitySens { 0.7f };

    // Filter: type 0 = av, 1 = lavpass, 2 = høypass
    AtomicFloat filterType   { 0.0f };
    AtomicFloat cutoff       { 12000.0f };
    AtomicFloat resonance    { 0.707f };

    // Pan -1..1
    AtomicFloat pan { 0.0f };

    // Loop (posisjoner som andel 0..1 av samplets lengde, crossfade i ms)
    AtomicFloat loopEnabled { 0.0f };
    AtomicFloat loopStart   { 0.0f };
    AtomicFloat loopEnd     { 1.0f };
    AtomicFloat loopXfadeMs { 10.0f };

    // LFO
    AtomicFloat lfoRate        { 4.0f };  // Hz
    AtomicFloat lfoPitchCents  { 0.0f };  // vibrato-dybde
    AtomicFloat lfoFilterOct   { 0.0f };  // filter-modulasjon i oktaver
    AtomicFloat lfoAmp         { 0.0f };  // tremolo 0..1

    // Master og effekter
    AtomicFloat masterGain     { 0.8f };
    AtomicFloat reverbMix      { 0.0f };
    AtomicFloat reverbSize     { 0.5f };
    AtomicFloat delayMix       { 0.0f };
    AtomicFloat delayTimeMs    { 350.0f };
    AtomicFloat delayFeedback  { 0.35f };

    std::vector<ParamInfo> describe()
    {
        return {
            { "attack",   "Attack",   "ENVELOPE", &attack,   0.001f, 5.0f,  0.005f, 0.1f,  0.0f, " s" },
            { "decay",    "Decay",    "ENVELOPE", &decay,    0.001f, 5.0f,  0.2f,   0.3f,  0.0f, " s" },
            { "sustain",  "Sustain",  "ENVELOPE", &sustain,  0.0f,   1.0f,  0.8f,   0.0f,  0.0f, "" },
            { "release",  "Release",  "ENVELOPE", &release,  0.005f, 8.0f,  0.25f,  0.4f,  0.0f, " s" },
            { "velsens",  "Velocity", "ENVELOPE", &velocitySens, 0.0f, 1.0f, 0.7f,  0.0f,  0.0f, "" },

            { "filtertype", "Type",   "FILTER",   &filterType, 0.0f, 2.0f,  0.0f,   0.0f,  1.0f, "" },
            { "cutoff",   "Cutoff",   "FILTER",   &cutoff,   20.0f,  20000.0f, 12000.0f, 1000.0f, 0.0f, " Hz" },
            { "resonance","Reso",     "FILTER",   &resonance, 0.5f,  12.0f, 0.707f, 2.0f,  0.0f, "" },
            { "pan",      "Pan",      "FILTER",   &pan,      -1.0f,  1.0f,  0.0f,   0.0f,  0.0f, "" },

            { "loop",     "Loop",     "LOOP",     &loopEnabled, 0.0f, 1.0f, 0.0f,   0.0f,  1.0f, "" },
            { "loopstart","Start",    "LOOP",     &loopStart, 0.0f,  1.0f,  0.0f,   0.0f,  0.0f, "" },
            { "loopend",  "End",      "LOOP",     &loopEnd,   0.0f,  1.0f,  1.0f,   0.0f,  0.0f, "" },
            { "loopxfade","Xfade",    "LOOP",     &loopXfadeMs, 0.0f, 500.0f, 10.0f, 50.0f, 0.0f, " ms" },

            { "lforate",  "Rate",     "LFO",      &lfoRate,  0.05f,  20.0f, 4.0f,   2.0f,  0.0f, " Hz" },
            { "lfopitch", "Pitch",    "LFO",      &lfoPitchCents, 0.0f, 200.0f, 0.0f, 0.0f, 0.0f, " ct" },
            { "lfofilter","Filter",   "LFO",      &lfoFilterOct,  0.0f, 4.0f,  0.0f,  0.0f, 0.0f, " oct" },
            { "lfoamp",   "Tremolo",  "LFO",      &lfoAmp,   0.0f,   1.0f,  0.0f,   0.0f,  0.0f, "" },

            { "reverbmix","Reverb",   "EFFEKTER", &reverbMix,  0.0f, 1.0f,  0.0f,   0.0f,  0.0f, "" },
            { "reverbsize","Størrelse","EFFEKTER",&reverbSize, 0.0f, 1.0f,  0.5f,   0.0f,  0.0f, "" },
            { "delaymix", "Delay",    "EFFEKTER", &delayMix,   0.0f, 1.0f,  0.0f,   0.0f,  0.0f, "" },
            { "delaytime","Tid",      "EFFEKTER", &delayTimeMs, 20.0f, 1500.0f, 350.0f, 300.0f, 0.0f, " ms" },
            { "delayfb",  "Feedback", "EFFEKTER", &delayFeedback, 0.0f, 0.95f, 0.35f, 0.0f, 0.0f, "" },

            { "master",   "Master",   "MASTER",   &masterGain, 0.0f, 1.5f,  0.8f,   0.0f,  0.0f, "" },
        };
    }
};
