#include "Instrument.h"

#include <map>

namespace
{
    std::unique_ptr<Sample> loadSampleFile (const juce::File& file,
                                            juce::AudioFormatManager& formats,
                                            juce::String& error)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            error = "Kunne ikke lese " + file.getFileName();
            return nullptr;
        }

        auto s = std::make_unique<Sample>();
        s->name = file.getFileNameWithoutExtension();
        s->file = file;
        s->sampleRate = reader->sampleRate;
        s->totalFrames = reader->lengthInSamples;
        s->numChannels = (int) juce::jmin (reader->numChannels, 2u);

        if (s->totalFrames > streamThresholdFrames)
        {
            s->streamed = true;
            s->data.setSize (s->numChannels, preloadFrames);
            reader->read (&s->data, 0, preloadFrames, 0, true, true);
            s->reader = std::move (reader);
        }
        else
        {
            s->data.setSize (s->numChannels, (int) s->totalFrames);
            reader->read (&s->data, 0, (int) s->totalFrames, 0, true, true);
        }

        return s;
    }

    using Opcodes = std::map<juce::String, juce::String>;

    void merge (Opcodes& dst, const Opcodes& src)
    {
        for (const auto& kv : src)
            dst[kv.first] = kv.second;
    }

    int intOp (const Opcodes& ops, const juce::String& key, int fallback)
    {
        auto it = ops.find (key);
        if (it == ops.end()) return fallback;
        const int n = noteNameToNumber (it->second);
        return n >= 0 ? n : fallback;
    }

    double numOp (const Opcodes& ops, const juce::String& key, double fallback)
    {
        auto it = ops.find (key);
        return it == ops.end() ? fallback : it->second.getDoubleValue();
    }

    juce::String strOp (const Opcodes& ops, const juce::String& key)
    {
        auto it = ops.find (key);
        return it == ops.end() ? juce::String() : it->second;
    }

    std::unique_ptr<Instrument> loadSfz (const juce::File& sfzFile,
                                         juce::AudioFormatManager& formats,
                                         juce::String& error)
    {
        const auto text = sfzFile.loadFileAsString();
        const auto baseDir = sfzFile.getParentDirectory();

        enum class Header { none, control, global, master, group, region };

        Opcodes control, global, master, group, region;
        Header current = Header::none;
        juce::String lastKey;

        auto instrument = std::make_unique<Instrument>();
        instrument->name = sfzFile.getFileNameWithoutExtension();

        std::map<juce::String, Sample*> cache;
        juce::StringArray warnings;
        bool haveRegion = false;

        auto flushRegion = [&]
        {
            if (! haveRegion)
                return;

            haveRegion = false;

            Opcodes ops;
            merge (ops, global);
            merge (ops, master);
            merge (ops, group);
            merge (ops, region);

            const auto samplePath = strOp (ops, "sample");

            if (samplePath.isEmpty())
                return;

            auto relative = (strOp (control, "default_path") + samplePath).replaceCharacter ('\\', '/');
            const auto file = baseDir.getChildFile (relative);
            const auto key = file.getFullPathName();

            Sample* sample = nullptr;

            if (auto it = cache.find (key); it != cache.end())
            {
                sample = it->second;
            }
            else
            {
                juce::String sampleError;

                if (auto loaded = loadSampleFile (file, formats, sampleError))
                {
                    sample = loaded.get();
                    instrument->samples.push_back (std::move (loaded));
                    cache[key] = sample;
                }
                else
                {
                    warnings.add (sampleError);
                    cache[key] = nullptr;
                }
            }

            if (sample == nullptr)
                return;

            Zone z;
            z.sample = sample;

            const int keyOp = intOp (ops, "key", -1);
            z.loKey = intOp (ops, "lokey", keyOp >= 0 ? keyOp : 0);
            z.hiKey = intOp (ops, "hikey", keyOp >= 0 ? keyOp : 127);
            z.rootKey = intOp (ops, "pitch_keycenter", keyOp >= 0 ? keyOp : 60);
            z.loVel = juce::jlimit (0, 127, intOp (ops, "lovel", 1));
            z.hiVel = juce::jlimit (0, 127, intOp (ops, "hivel", 127));
            z.tuneCents = (float) (numOp (ops, "tune", 0.0) + 100.0 * numOp (ops, "transpose", 0.0));
            z.volumeDb = (float) numOp (ops, "volume", 0.0);
            z.pan = juce::jlimit (-1.0f, 1.0f, (float) (numOp (ops, "pan", 0.0) / 100.0));
            z.seqLength = juce::jmax (1, (int) numOp (ops, "seq_length", 1));
            z.seqPosition = juce::jlimit (1, z.seqLength, (int) numOp (ops, "seq_position", 1));

            const auto loopMode = strOp (ops, "loop_mode");

            if (loopMode == "loop_continuous" || loopMode == "loop_sustain")
                z.loopMode = LoopMode::on;
            else if (loopMode == "no_loop" || loopMode == "one_shot")
                z.loopMode = LoopMode::off;

            if (ops.count ("loop_start")) z.loopStart = (std::int64_t) numOp (ops, "loop_start", -1);
            if (ops.count ("loop_end"))   z.loopEnd   = (std::int64_t) numOp (ops, "loop_end", -1);

            instrument->zones.push_back (z);
        };

        auto setOpcode = [&] (const juce::String& k, const juce::String& v)
        {
            lastKey = k;

            switch (current)
            {
                case Header::control: control[k] = v; break;
                case Header::global:  global[k]  = v; break;
                case Header::master:  master[k]  = v; break;
                case Header::group:   group[k]   = v; break;
                case Header::region:  region[k]  = v; break;
                case Header::none:    break;
            }
        };

        auto appendToLast = [&] (const juce::String& extra)
        {
            Opcodes* target = nullptr;

            switch (current)
            {
                case Header::control: target = &control; break;
                case Header::global:  target = &global;  break;
                case Header::master:  target = &master;  break;
                case Header::group:   target = &group;   break;
                case Header::region:  target = &region;  break;
                case Header::none:    break;
            }

            if (target != nullptr && lastKey.isNotEmpty())
                (*target)[lastKey] += " " + extra;
        };

        std::function<void (const juce::String&)> handleToken = [&] (const juce::String& token)
        {
            if (token.startsWithChar ('<'))
            {
                const int close = token.indexOfChar ('>');

                if (close < 0)
                    return;

                const auto name = token.substring (1, close).toLowerCase();
                const auto rest = token.substring (close + 1);

                if (name == "region")
                {
                    flushRegion();
                    region.clear();
                    current = Header::region;
                    haveRegion = true;
                }
                else if (name == "group")
                {
                    flushRegion();
                    group.clear();
                    current = Header::group;
                }
                else if (name == "master")
                {
                    flushRegion();
                    master.clear();
                    group.clear();
                    current = Header::master;
                }
                else if (name == "global")
                {
                    flushRegion();
                    global.clear();
                    master.clear();
                    group.clear();
                    current = Header::global;
                }
                else if (name == "control")
                {
                    flushRegion();
                    current = Header::control;
                }
                else
                {
                    flushRegion();
                    current = Header::none; // <curve>, <effect> osv. ignoreres
                }

                lastKey = {};

                if (rest.isNotEmpty())
                    handleToken (rest);

                return;
            }

            const int eq = token.indexOfChar ('=');

            if (eq > 0)
                setOpcode (token.substring (0, eq).toLowerCase(), token.substring (eq + 1));
            else if (token.isNotEmpty())
                appendToLast (token);
        };

        for (auto line : juce::StringArray::fromLines (text))
        {
            const int comment = line.indexOf ("//");

            if (comment >= 0)
                line = line.substring (0, comment);

            for (const auto& token : juce::StringArray::fromTokens (line, " \t", ""))
                handleToken (token);
        }

        flushRegion();

        if (instrument->zones.empty())
        {
            error = warnings.isEmpty() ? "Fant ingen regioner i " + sfzFile.getFileName()
                                       : warnings.joinIntoString ("; ");
            return nullptr;
        }

        if (warnings.size() > 0)
            error = warnings.joinIntoString ("; ");

        return instrument;
    }
}

int noteNameToNumber (const juce::String& textIn)
{
    const auto text = textIn.trim().toLowerCase();

    if (text.isEmpty())
        return -1;

    if (text.containsOnly ("-0123456789"))
    {
        const int n = text.getIntValue();
        return (n >= 0 && n <= 127) ? n : -1;
    }

    static const int semitones[7] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g

    const auto letter = text[0];

    if (letter < 'a' || letter > 'g')
        return -1;

    int semi = semitones[letter - 'a'];
    int pos = 1;

    if (text[pos] == '#')      { ++semi; ++pos; }
    else if (text[pos] == 'b') { --semi; ++pos; }

    const auto octaveText = text.substring (pos);

    if (octaveText.isEmpty() || ! octaveText.containsOnly ("-0123456789"))
        return -1;

    const int n = (octaveText.getIntValue() + 1) * 12 + semi;
    return (n >= 0 && n <= 127) ? n : -1;
}

std::unique_ptr<Instrument> loadInstrumentFromFile (const juce::File& file,
                                                    juce::AudioFormatManager& formats,
                                                    juce::String& error)
{
    error = {};

    if (file.hasFileExtension ("sfz"))
        return loadSfz (file, formats, error);

    auto sample = loadSampleFile (file, formats, error);

    if (sample == nullptr)
        return nullptr;

    auto instrument = std::make_unique<Instrument>();
    instrument->name = file.getFileNameWithoutExtension();

    Zone z;
    z.sample = sample.get();
    instrument->samples.push_back (std::move (sample));
    instrument->zones.push_back (z);
    return instrument;
}
