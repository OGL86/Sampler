#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Lydfiler lengre enn dette (i frames) strømmes fra disk. Starten
// (preloadFrames) ligger alltid i RAM slik at avspilling starter umiddelbart.
constexpr int streamThresholdFrames = 44100 * 12;
constexpr int preloadFrames = 65536;

// Et innlastet sample. Uforanderlig etter publisering til lydtråden.
struct Sample
{
    juce::String name;
    juce::File file;
    juce::AudioBuffer<float> data;       // hele samplet, eller bare starten hvis streamed
    std::int64_t totalFrames = 0;
    double sampleRate = 44100.0;
    bool streamed = false;

    // Brukes kun av disk-tråden når streamed == true
    std::unique_ptr<juce::AudioFormatReader> reader;
    int numChannels = 1;
};

enum class LoopMode { fromParams, off, on };

// En region: hvilket sample som spilles for hvilke taster/velocity.
struct Zone
{
    const Sample* sample = nullptr;
    int loKey = 0, hiKey = 127;
    int loVel = 1, hiVel = 127;
    int rootKey = 60;
    float tuneCents = 0.0f;
    float volumeDb = 0.0f;
    float pan = 0.0f;               // -1..1
    LoopMode loopMode = LoopMode::fromParams;
    std::int64_t loopStart = -1;    // -1 = bruk parametrene
    std::int64_t loopEnd = -1;
    int seqLength = 1;              // round-robin
    int seqPosition = 1;
};

struct Instrument
{
    juce::String name;
    int epoch = 0;
    std::vector<std::unique_ptr<Sample>> samples;
    std::vector<Zone> zones;
};

// Laster en enkelt lydfil (ett zone over hele tastaturet) eller en SFZ-fil.
// Kjører på meldingstråden. Returnerer nullptr og en feilmelding ved feil.
std::unique_ptr<Instrument> loadInstrumentFromFile (const juce::File& file,
                                                    juce::AudioFormatManager& formats,
                                                    juce::String& error);

// Eksponert for testing
int noteNameToNumber (const juce::String& text); // "c4" -> 60, "60" -> 60, ugyldig -> -1
