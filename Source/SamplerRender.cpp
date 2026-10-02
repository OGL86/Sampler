#include "SamplerEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float halfPi = 1.57079632679f;
    constexpr int filterSubBlock = 32;

    // Lesevisning av ett sample, fra RAM eller ringbuffer
    struct SrcView
    {
        const float* l = nullptr;
        const float* r = nullptr;
        const float* ringL = nullptr;
        const float* ringR = nullptr;
        std::int64_t total = 0;
        std::int64_t preload = 0;  // frames tilgjengelig i l/r
        std::int64_t written = 0;  // frames skrevet av disk-tråden (kun streamed)
    };

    inline void fetchFrame (const SrcView& s, std::int64_t f, float& l, float& r) noexcept
    {
        if (f < 0)
            f = 0;

        if (f >= s.total)
        {
            l = r = 0.0f;
            return;
        }

        if (f < s.preload)
        {
            l = s.l[f];
            r = s.r[f];
            return;
        }

        if (f >= s.written || s.ringL == nullptr) // underrun: stillhet fremfor å blokkere
        {
            l = r = 0.0f;
            return;
        }

        const auto k = (std::size_t) (f & (StreamSlot::ringFrames - 1));
        l = s.ringL[k];
        r = s.ringR[k];
    }

    inline float hermite (float ym1, float y0, float y1, float y2, float t) noexcept
    {
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    inline void readInterpolated (const SrcView& s, double position, float& l, float& r) noexcept
    {
        const auto idx = (std::int64_t) position;
        const float t = (float) (position - (double) idx);

        float l0, r0, l1, r1, l2, r2, l3, r3;
        fetchFrame (s, idx - 1, l0, r0);
        fetchFrame (s, idx,     l1, r1);
        fetchFrame (s, idx + 1, l2, r2);
        fetchFrame (s, idx + 2, l3, r3);

        l = hermite (l0, l1, l2, l3, t);
        r = hermite (r0, r1, r2, r3, t);
    }
}

void SamplerEngine::snapshotParams()
{
    block.adsr.attack  = juce::jmax (0.001f, params.attack.get());
    block.adsr.decay   = juce::jmax (0.001f, params.decay.get());
    block.adsr.sustain = juce::jlimit (0.0f, 1.0f, params.sustain.get());
    block.adsr.release = juce::jmax (0.005f, params.release.get());

    block.velocitySens = juce::jlimit (0.0f, 1.0f, params.velocitySens.get());
    block.filterType   = (int) std::lround (params.filterType.get());
    block.cutoff       = params.cutoff.get();
    block.resonance    = params.resonance.get();
    block.pan          = params.pan.get();
    block.lfoRate      = params.lfoRate.get();
    block.lfoPitchCents = params.lfoPitchCents.get();
    block.lfoFilterOct = params.lfoFilterOct.get();
    block.lfoAmp       = params.lfoAmp.get();
}

void SamplerEngine::render (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi)
{
    // Bytt instrument hvis meldingstråden har publisert et nytt
    const Instrument* inst = current.load (std::memory_order_acquire);

    if (inst != audioInstrument)
    {
        killAllVoices();
        audioInstrument = inst;
        std::fill (std::begin (roundRobin), std::end (roundRobin), 0);

        if (inst != nullptr)
            ackEpoch.store (inst->epoch, std::memory_order_release);
    }

    snapshotParams();

    for (auto& v : voices)
        if (v.active)
            v.env.setParameters (block.adsr);

    int pos = 0;
    const int total = out.getNumSamples();

    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, total, meta.samplePosition);

        if (eventPos > pos)
        {
            renderRange (out, pos, eventPos - pos);
            pos = eventPos;
        }

        handleMessage (meta.getMessage());
    }

    if (pos < total)
        renderRange (out, pos, total - pos);

    applyEffects (out);

    const float peak = out.getMagnitude (0, total);

    if (peak > peakLevel.load (std::memory_order_relaxed))
        peakLevel.store (peak, std::memory_order_relaxed);
}

void SamplerEngine::renderRange (juce::AudioBuffer<float>& out, int start, int count)
{
    while (count > 0)
    {
        const int n = juce::jmin (count, maxBlock);
        renderSegment (out, start, n);
        start += n;
        count -= n;
    }
}

void SamplerEngine::renderSegment (juce::AudioBuffer<float>& out, int start, int count)
{
    // Én LFO delt av alle stemmer
    const double phaseInc = (double) block.lfoRate / deviceRate;

    for (int i = 0; i < count; ++i)
    {
        lfoBuf[(size_t) i] = (float) std::sin (6.283185307179586 * lfoPhase);
        lfoPhase += phaseInc;

        if (lfoPhase >= 1.0)
            lfoPhase -= 1.0;
    }

    float* outL = out.getWritePointer (0) + start;
    float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1) + start : scratchR.data();

    for (int i = 0; i < numVoices; ++i)
        if (voices[(size_t) i].active)
            renderVoice (voices[(size_t) i], i, count, outL, outR);
}

void SamplerEngine::renderVoice (Voice& v, int index, int count, float* outL, float* outR)
{
    const Sample& s = *v.sample;
    StreamSlot& slot = streamer.slot (index);

    SrcView src;
    src.l = s.data.getReadPointer (0);
    src.r = s.data.getNumChannels() > 1 ? s.data.getReadPointer (1) : src.l;
    src.total = s.totalFrames;

    if (s.streamed)
    {
        src.preload = preloadFrames;
        src.written = StreamSlot::writeFrameOf (slot.state.load (std::memory_order_acquire));
        src.ringL = slot.ring[0].data();
        src.ringR = s.numChannels > 1 ? slot.ring[1].data() : src.ringL;
    }
    else
    {
        src.preload = src.total;
        src.written = src.total;
    }

    float* const vl = voiceL.data();
    float* const vr = voiceR.data();
    float* const gains = gainBuf.data();

    const bool vibrato = block.lfoPitchCents > 0.01f;
    const float vibDepth = block.lfoPitchCents / 1200.0f;
    const double loopLength = v.loopEnd - v.loopStart;

    int produced = count;
    bool ended = false;

    // 1) Hent og interpoler sampledata
    for (int i = 0; i < count; ++i)
    {
        if (v.looping)
        {
            while (v.position >= v.loopEnd)
                v.position -= loopLength;
        }
        else if (v.position >= (double) src.total - 1.0)
        {
            produced = i;
            ended = true;
            break;
        }

        float l, r;
        readInterpolated (src, v.position, l, r);

        if (v.looping && v.xfadeFrames > 1.0)
        {
            const double fadeStart = v.loopEnd - v.xfadeFrames;

            if (v.position >= fadeStart)
            {
                // Equal-power crossfade mot materialet foran loop-starten
                const float t = (float) ((v.position - fadeStart) / v.xfadeFrames);
                float l2, r2;
                readInterpolated (src, v.position - loopLength, l2, r2);
                const float a = std::cos (t * halfPi);
                const float b = std::sin (t * halfPi);
                l = l * a + l2 * b;
                r = r * a + r2 * b;
            }
        }

        vl[i] = l;
        vr[i] = r;

        double inc = v.baseIncrement * (double) bendFactor;

        if (vibrato)
            inc *= std::exp2 (vibDepth * lfoBuf[(size_t) i]);

        v.position += inc;
    }

    // 2) Envelope og tremolo
    const bool tremolo = block.lfoAmp > 0.001f;

    for (int i = 0; i < produced; ++i)
    {
        const float g = v.env.getNextSample();

        if (! v.env.isActive())
        {
            produced = i;
            ended = true;
            break;
        }

        gains[i] = tremolo ? g * (1.0f - block.lfoAmp * (0.5f + 0.5f * lfoBuf[(size_t) i])) : g;
    }

    // 3) Filter (koeffisienter oppdateres per delblokk)
    if (block.filterType != 0 && produced > 0)
    {
        const auto type = block.filterType == 1 ? Biquad::Type::lowpass : Biquad::Type::highpass;

        for (int i = 0; i < produced; i += filterSubBlock)
        {
            const int m = juce::jmin (filterSubBlock, produced - i);
            float cutoff = block.cutoff;

            if (block.lfoFilterOct > 0.0f)
                cutoff *= std::exp2 (block.lfoFilterOct * lfoBuf[(size_t) i]);

            Biquad& f0 = v.filter[0];
            Biquad& f1 = v.filter[1];
            f0.setCoefficients (type, deviceRate, cutoff, block.resonance);
            f1.c0 = f0.c0; f1.c1 = f0.c1; f1.c2 = f0.c2; f1.c3 = f0.c3; f1.c4 = f0.c4;

            for (int j = 0; j < m; ++j)
            {
                vl[i + j] = f0.process (vl[i + j]);
                vr[i + j] = f1.process (vr[i + j]);
            }
        }
    }

    // 4) Miks inn med pan. Vektoriserte operasjoner (SSE/NEON via JUCE).
    if (produced > 0)
    {
        juce::FloatVectorOperations::multiply (vl, gains, produced);
        juce::FloatVectorOperations::multiply (vr, gains, produced);

        const float p = juce::jlimit (-1.0f, 1.0f, block.pan + v.zonePan);
        const float angle = (p + 1.0f) * (halfPi * 0.5f);
        const float gl = std::cos (angle) * 1.41421356f * v.gain;
        const float gr = std::sin (angle) * 1.41421356f * v.gain;

        juce::FloatVectorOperations::addWithMultiply (outL, vl, gl, produced);
        juce::FloatVectorOperations::addWithMultiply (outR, vr, gr, produced);
    }

    if (s.streamed)
        slot.readFrame.store ((std::int64_t) v.position, std::memory_order_relaxed);

    if (ended)
    {
        v.active = false;
        slot.stop();
    }
}

void SamplerEngine::applyEffects (juce::AudioBuffer<float>& out)
{
    const int n = out.getNumSamples();

    if (out.getNumChannels() < 2 || n == 0)
        return;

    float* L = out.getWritePointer (0);
    float* R = out.getWritePointer (1);

    const float delayMix = params.delayMix.get();

    if (delayMix > 0.001f)
        delay.process (L, R, n, params.delayTimeMs.get(), params.delayFeedback.get(), delayMix);

    const float reverbMix = params.reverbMix.get();

    if (reverbMix > 0.001f)
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = params.reverbSize.get();
        rp.damping = 0.5f;
        rp.wetLevel = reverbMix * 0.3f;
        rp.dryLevel = 0.5f; // JUCE skalerer dry med 2 => enhetsforsterkning
        rp.width = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
        reverb.processStereo (L, R, n);
    }

    const float master = params.masterGain.get();
    out.applyGainRamp (0, n, lastMaster, master);
    lastMaster = master;

    juce::FloatVectorOperations::clip (L, L, -1.0f, 1.0f, n);
    juce::FloatVectorOperations::clip (R, R, -1.0f, 1.0f, n);
}
