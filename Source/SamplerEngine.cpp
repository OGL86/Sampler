#include "SamplerEngine.h"

#include <algorithm>
#include <cmath>

SamplerEngine::SamplerEngine()
{
    voices.resize ((size_t) numVoices);
}

SamplerEngine::~SamplerEngine()
{
    streamer.stopStreaming();
}

void SamplerEngine::prepare (double deviceSampleRate, int maxBlockSize)
{
    deviceRate = deviceSampleRate;
    maxBlock = juce::jmax (16, maxBlockSize);

    voiceL.assign ((size_t) maxBlock, 0.0f);
    voiceR.assign ((size_t) maxBlock, 0.0f);
    gainBuf.assign ((size_t) maxBlock, 0.0f);
    lfoBuf.assign ((size_t) maxBlock, 0.0f);
    scratchR.assign ((size_t) maxBlock, 0.0f);

    for (int i = 0; i < numVoices; ++i)
    {
        voices[(size_t) i] = Voice();
        voices[(size_t) i].env.setSampleRate (deviceRate);
        streamer.slot (i).stop();
    }

    delay.prepare (deviceRate);
    reverb.setSampleRate (deviceRate);
    reverb.reset();

    voiceCounter = 0;
    sustainDown = false;
    bendFactor = 1.0f;
    lfoPhase = 0.0;
    lastMaster = params.masterGain.get();
    std::fill (std::begin (roundRobin), std::end (roundRobin), 0);

    streamer.startStreaming();
}

void SamplerEngine::loadInstrument (std::unique_ptr<Instrument> instrument)
{
    if (instrument == nullptr)
        return;

    instrument->epoch = ++epochCounter;
    const Instrument* raw = instrument.get();
    instruments.push_back (std::move (instrument));
    current.store (raw, std::memory_order_release);
}

void SamplerEngine::collectGarbage()
{
    const Instrument* cur = current.load (std::memory_order_acquire);

    // Vent til lydtråden har byttet til gjeldende instrument og stoppet stemmene på de gamle.
    if (cur == nullptr || ackEpoch.load (std::memory_order_acquire) < cur->epoch)
        return;

    // Disk-tråden kan fortsatt være midt i en lesing fra et gammelt sample.
    const juce::ScopedLock sl (streamer.getReaderLock());

    instruments.erase (std::remove_if (instruments.begin(), instruments.end(),
                                       [cur] (const std::unique_ptr<Instrument>& p) { return p.get() != cur; }),
                       instruments.end());
}

// ---------------------------------------------------------------- MIDI

void SamplerEngine::handleMessage (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        noteOn (m.getNoteNumber(), m.getVelocity());
    }
    else if (m.isNoteOff())
    {
        noteOff (m.getNoteNumber());
    }
    else if (m.isPitchWheel())
    {
        const float semitones = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
        bendFactor = std::exp2 (semitones / 12.0f);
    }
    else if (m.isController() && m.getControllerNumber() == 64)
    {
        setSustain (m.getControllerValue() >= 64);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
        {
            if (v.active && ! v.releasing)
            {
                v.releasing = true;
                v.sustainHeld = false;
                v.env.noteOff();
            }
        }
    }
}

void SamplerEngine::noteOn (int note, int velocity)
{
    if (audioInstrument == nullptr || note < 0 || note > 127)
        return;

    const int rr = roundRobin[note];

    for (const auto& zone : audioInstrument->zones)
    {
        if (note < zone.loKey || note > zone.hiKey || velocity < zone.loVel || velocity > zone.hiVel)
            continue;

        if ((rr % zone.seqLength) + 1 != zone.seqPosition)
            continue;

        if (Voice* v = allocateVoice())
            startVoice (*v, (int) (v - voices.data()), zone, note, velocity);
    }

    roundRobin[note] = (rr + 1) & 0xFFFFF;
}

void SamplerEngine::noteOff (int note)
{
    for (auto& v : voices)
    {
        if (! v.active || v.releasing || v.sustainHeld || v.note != note)
            continue;

        if (sustainDown)
        {
            v.sustainHeld = true;
        }
        else
        {
            v.releasing = true;
            v.env.noteOff();
        }
    }
}

void SamplerEngine::setSustain (bool down)
{
    sustainDown = down;

    if (down)
        return;

    for (auto& v : voices)
    {
        if (v.active && v.sustainHeld)
        {
            v.sustainHeld = false;
            v.releasing = true;
            v.env.noteOff();
        }
    }
}

void SamplerEngine::killAllVoices()
{
    for (int i = 0; i < numVoices; ++i)
    {
        voices[(size_t) i].active = false;
        streamer.slot (i).stop();
    }
}

SamplerEngine::Voice* SamplerEngine::allocateVoice()
{
    // 1) ledig stemme, 2) eldste stemme i release, 3) eldste stemme
    for (auto& v : voices)
        if (! v.active)
            return &v;

    Voice* victim = nullptr;

    for (auto& v : voices)
        if (v.releasing && (victim == nullptr || v.age < victim->age))
            victim = &v;

    if (victim == nullptr)
    {
        victim = &voices[0];

        for (auto& v : voices)
            if (v.age < victim->age)
                victim = &v;
    }

    return victim;
}

void SamplerEngine::startVoice (Voice& v, int index, const Zone& zone, int note, int velocity)
{
    const Sample* s = zone.sample;

    if (s->streamed)
        streamer.slot (index).start (s);
    else
        streamer.slot (index).stop();

    v.active = true;
    v.releasing = false;
    v.sustainHeld = false;
    v.note = note;
    v.age = ++voiceCounter;
    v.sample = s;
    v.position = 0.0;

    const double semitones = (note - zone.rootKey) + zone.tuneCents / 100.0;
    v.baseIncrement = (s->sampleRate / deviceRate) * std::exp2 (semitones / 12.0);

    const float velNorm = (float) velocity / 127.0f;
    const float velFactor = 1.0f + block.velocitySens * (velNorm * velNorm - 1.0f);
    v.gain = 0.5f * velFactor * juce::Decibels::decibelsToGain (zone.volumeDb);
    v.zonePan = zone.pan;

    // Loop
    bool loopOn = zone.loopMode == LoopMode::on
                  || (zone.loopMode == LoopMode::fromParams && params.loopEnabled.get() > 0.5f);

    if (s->streamed)
        loopOn = false; // loop støttes bare for samples som ligger helt i RAM

    if (loopOn)
    {
        const double total = (double) s->totalFrames;
        double ls, le;

        if (zone.loopStart >= 0 && zone.loopEnd > zone.loopStart)
        {
            ls = (double) zone.loopStart;
            le = (double) zone.loopEnd;
        }
        else
        {
            ls = params.loopStart.get() * total;
            le = params.loopEnd.get() * total;
        }

        le = std::min (le, total - 1.0);
        ls = std::max (0.0, std::min (ls, le - 16.0));

        if (le - ls < 16.0)
        {
            loopOn = false;
        }
        else
        {
            const double xf = params.loopXfadeMs.get() * 0.001 * s->sampleRate;
            v.loopStart = ls;
            v.loopEnd = le;
            v.xfadeFrames = std::min ({ xf, ls, (le - ls) * 0.5 });
        }
    }

    v.looping = loopOn;

    v.env.setSampleRate (deviceRate);
    v.env.setParameters (block.adsr);
    v.env.reset();
    v.env.noteOn();

    v.filter[0].reset();
    v.filter[1].reset();
}
