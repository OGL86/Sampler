// Frittstående tester for motoren: pitch, loop, release, streaming, SFZ og round-robin.
// Bygges med -DSAMPLER_BUILD_TESTS=ON (se CMakeLists.txt).

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "../Source/Instrument.h"
#include "../Source/SamplerEngine.h"

namespace
{
    int failures = 0;
    int checks = 0;

    void check (bool ok, const juce::String& what)
    {
        ++checks;

        if (! ok)
        {
            ++failures;
            std::printf ("  FEIL: %s\n", what.toRawUTF8());
        }
        else
        {
            std::printf ("  ok:   %s\n", what.toRawUTF8());
        }
    }

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    void writeSine (const juce::File& file, double freq, double seconds, float amp = 0.8f, int channels = 1)
    {
        const int n = (int) (seconds * sr);
        juce::AudioBuffer<float> buf (channels, n);

        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
                buf.setSample (c, i, amp * (float) std::sin (6.283185307179586 * freq * i / sr));

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, (unsigned) channels, 16, {}, 0));

        if (writer != nullptr)
        {
            stream.release();
            writer->writeFromAudioSampleBuffer (buf, 0, n);
        }
    }

    struct Event { int sample; juce::MidiMessage msg; };

    // Rendrer totalSamples i blokker. sleepMs simulerer sanntid så disk-tråden rekker å fylle bufferne.
    std::vector<float> run (SamplerEngine& engine, std::vector<Event> events, int totalSamples, int sleepMs = 0)
    {
        std::vector<float> out;
        out.reserve ((size_t) totalSamples);

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer midi;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            buf.clear();
            midi.clear();

            for (const auto& e : events)
                if (e.sample >= start && e.sample < start + blockSize)
                    midi.addEvent (e.msg, e.sample - start);

            engine.render (buf, midi);

            for (int i = 0; i < blockSize; ++i)
                out.push_back (buf.getSample (0, i));

            if (sleepMs > 0)
                juce::Thread::sleep (sleepMs);
        }

        return out;
    }

    float peak (const std::vector<float>& v, size_t from, size_t to)
    {
        float p = 0.0f;
        for (size_t i = from; i < to && i < v.size(); ++i)
            p = std::fmax (p, std::fabs (v[i]));
        return p;
    }

    bool allFinite (const std::vector<float>& v)
    {
        for (float x : v)
            if (! std::isfinite (x))
                return false;
        return true;
    }

    double crossingsPerSample (const std::vector<float>& v, size_t from, size_t to)
    {
        int count = 0;
        for (size_t i = from + 1; i < to && i < v.size(); ++i)
            if (v[i - 1] <= 0.0f && v[i] > 0.0f)
                ++count;
        return (double) count / (double) (to - from);
    }

    std::unique_ptr<Instrument> load (const juce::File& f, juce::AudioFormatManager& fm, juce::String* msg = nullptr)
    {
        juce::String err;
        auto inst = loadInstrumentFromFile (f, fm, err);
        if (msg != nullptr) *msg = err;
        return inst;
    }

    juce::MidiMessage on (int note)  { return juce::MidiMessage::noteOn (1, note, (juce::uint8) 127); }
    juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }

    void setupEngine (SamplerEngine& e, std::unique_ptr<Instrument> inst)
    {
        e.prepare (sr, blockSize);
        e.params.attack.set (0.001f);
        e.params.release.set (0.05f);
        e.params.sustain.set (1.0f);
        e.loadInstrument (std::move (inst));
        run (e, {}, blockSize); // lydtråden bytter instrument i første blokk
    }
}

int main()
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sampler_tests");
    dir.deleteRecursively();
    dir.getChildFile ("samples").createDirectory();

    std::printf ("Noteparsing\n");
    check (noteNameToNumber ("c4") == 60, "c4 = 60");
    check (noteNameToNumber ("A0") == 21, "A0 = 21");
    check (noteNameToNumber ("c#4") == 61 && noteNameToNumber ("db4") == 61, "c#4 = db4 = 61");
    check (noteNameToNumber ("72") == 72, "tall tolkes som midi-note");
    check (noteNameToNumber ("xyz") == -1 && noteNameToNumber ("200") == -1, "ugyldig gir -1");

    // ------------------------------------------------------------- pitch
    std::printf ("Pitch\n");
    {
        const auto wav = dir.getChildFile ("sine440.wav");
        writeSine (wav, 440.0, 1.0);

        SamplerEngine e;
        setupEngine (e, load (wav, fm));

        auto a = run (e, { { 0, on (60) } }, 12000);
        check (allFinite (a), "ingen NaN/inf");
        check (peak (a, 1000, 5000) > 0.2f, "note 60 gir lyd");
        const double f60 = crossingsPerSample (a, 1000, 9000) * sr;
        check (std::fabs (f60 - 440.0) < 15.0, "note 60 spiller originalt tonehøyde (" + juce::String (f60, 1) + " Hz)");

        SamplerEngine e2;
        setupEngine (e2, load (wav, fm));
        auto b = run (e2, { { 0, on (72) } }, 12000);
        const double f72 = crossingsPerSample (b, 1000, 9000) * sr;
        check (std::fabs (f72 - 880.0) < 30.0, "note 72 er en oktav opp (" + juce::String (f72, 1) + " Hz)");
    }

    // ------------------------------------------------- release og sample-slutt
    std::printf ("Release og slutt\n");
    {
        const auto wav = dir.getChildFile ("sine440.wav");
        SamplerEngine e;
        setupEngine (e, load (wav, fm));

        auto a = run (e, { { 0, on (60) }, { 4096, off (60) } }, 44100);
        check (peak (a, 1000, 4000) > 0.2f, "lyd mens tasten holdes");
        check (peak (a, 20000, 44100) < 0.001f, "stillhet etter release");

        SamplerEngine e2;
        setupEngine (e2, load (wav, fm));
        auto b = run (e2, { { 0, on (60) } }, 60000);
        check (peak (b, 50000, 60000) < 0.001f, "one-shot slutter når samplet er ferdig");
    }

    // -------------------------------------------------------------- loop
    std::printf ("Loop\n");
    {
        const auto wav = dir.getChildFile ("sine440.wav");
        SamplerEngine e;
        e.params.loopEnabled.set (1.0f);
        e.params.loopStart.set (0.1f);
        e.params.loopEnd.set (0.9f);
        e.params.loopXfadeMs.set (10.0f);
        setupEngine (e, load (wav, fm));

        auto a = run (e, { { 0, on (60) } }, 44100 * 3);
        check (allFinite (a), "ingen NaN/inf i loop");
        check (peak (a, 44100 * 3 - 4096, 44100 * 3) > 0.2f, "loopen holder lyden gående etter 3 s");
        const double f = crossingsPerSample (a, 44100 * 2, 44100 * 2 + 8000) * sr;
        check (std::fabs (f - 440.0) < 20.0, "loopen beholder tonehøyden (" + juce::String (f, 1) + " Hz)");
    }

    // --------------------------------------------------------- streaming
    std::printf ("Streaming fra disk\n");
    {
        const auto wav = dir.getChildFile ("long.wav");
        writeSine (wav, 220.0, 13.0);

        auto inst = load (wav, fm);
        check (inst != nullptr && inst->samples[0]->streamed, "13 s-fil velges for streaming");

        SamplerEngine e;
        setupEngine (e, std::move (inst));

        const int total = (int) (12.8 * sr);
        auto a = run (e, { { 0, on (60) } }, total, 6);

        int silentBlocks = 0;

        for (int start = blockSize * 3; start + blockSize <= total; start += blockSize)
            if (peak (a, (size_t) start, (size_t) start + blockSize) < 0.05f)
                ++silentBlocks;

        check (allFinite (a), "ingen NaN/inf ved streaming");
        check (silentBlocks == 0, "ingen underruns på 12.8 s (" + juce::String (silentBlocks) + " stille blokker)");
        const double f = crossingsPerSample (a, (size_t) (11.0 * sr), (size_t) (11.0 * sr) + 8000) * sr;
        check (std::fabs (f - 220.0) < 10.0, "riktig tonehøyde sent i filen (" + juce::String (f, 1) + " Hz)");
    }

    // --------------------------------------------------- SFZ og round-robin
    std::printf ("SFZ og round-robin\n");
    {
        writeSine (dir.getChildFile ("samples/a.wav"), 440.0, 1.0);
        writeSine (dir.getChildFile ("samples/b.wav"), 880.0, 1.0);
        writeSine (dir.getChildFile ("samples/my sample.wav"), 330.0, 1.0);

        const auto sfz = dir.getChildFile ("test.sfz");
        sfz.replaceWithText (
            "// kommentar\n"
            "<control>\n"
            "default_path=samples/\n"
            "<global> volume=-6\n"
            "<group> lokey=c3 hikey=c5 pitch_keycenter=c4\n"
            "<region> sample=a.wav seq_length=2 seq_position=1\n"
            "<region> sample=b.wav seq_length=2 seq_position=2\n"
            "<group> key=g2 lovel=64\n"
            "<region> sample=my sample.wav tune=50 pan=-100 loop_mode=loop_continuous loop_start=100 loop_end=2000\n");

        juce::String msg;
        auto inst = load (sfz, fm, &msg);

        check (inst != nullptr && inst->zones.size() == 3, "SFZ gir 3 regioner " + msg);

        if (inst != nullptr && inst->zones.size() == 3)
        {
            const auto& z0 = inst->zones[0];
            const auto& z2 = inst->zones[2];
            check (z0.loKey == 48 && z0.hiKey == 72 && z0.rootKey == 60, "key-område og pitch_keycenter fra gruppe");
            check (z0.seqLength == 2 && z0.seqPosition == 1, "seq_length/seq_position");
            check (std::fabs (z0.volumeDb + 6.0f) < 0.01f, "volume arvet fra <global>");
            check (z2.loKey == 43 && z2.hiKey == 43 && z2.rootKey == 43 && z2.loVel == 64, "key= og lovel");
            check (z2.sample != nullptr && z2.sample->name == "my sample", "filnavn med mellomrom");
            check (std::fabs (z2.tuneCents - 50.0f) < 0.01f && std::fabs (z2.pan + 1.0f) < 0.01f, "tune og pan");
            check (z2.loopMode == LoopMode::on && z2.loopStart == 100 && z2.loopEnd == 2000, "loop_mode og loop-punkter");

            SamplerEngine e;
            setupEngine (e, std::move (inst));

            auto r = run (e, { { 0, on (60) }, { 6000, off (60) }, { 20000, on (60) } }, 30000);
            const double first  = crossingsPerSample (r, 1000, 5000) * sr;
            const double second = crossingsPerSample (r, 21000, 25000) * sr;
            check (std::fabs (first - 440.0) < 25.0, "første anslag bruker a.wav (" + juce::String (first, 1) + " Hz)");
            check (std::fabs (second - 880.0) < 50.0, "andre anslag bruker b.wav, round-robin (" + juce::String (second, 1) + " Hz)");

            SamplerEngine fresh;
            setupEngine (fresh, load (sfz, fm));
            auto none = run (fresh, { { 0, on (100) } }, 4096);
            check (peak (none, 0, 4096) < 0.001f, "note utenfor alle zones gir stillhet");
        }
    }

    dir.deleteRecursively();

    std::printf ("\n%d av %d sjekker feilet\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
