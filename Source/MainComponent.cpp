#include "MainComponent.h"

namespace
{
    juce::File presetDirectory()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Sampler presets");
        dir.createDirectory();
        return dir;
    }

    double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }
}

MainComponent::MainComponent()
    : waveform (formats, engine.params)
{
    formats.registerBasicFormats();
    setLookAndFeel (&lookAndFeel);

    loadButton.onClick   = [this] { chooseInstrument(); };
    saveButton.onClick   = [this] { savePreset(); };
    presetButton.onClick = [this] { loadPreset(); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (presetButton);

    statusLabel.setText ("Slipp en lydfil eller .sfz her, eller trykk Last inn", juce::dontSendNotification);
    statusLabel.setFont (juce::FontOptions (14.0f));
    addAndMakeVisible (statusLabel);

    addAndMakeVisible (waveform);
    addAndMakeVisible (zoneMap);
    addAndMakeVisible (meter);

    zoneMap.onZoneSelected = [this] (int index)
    {
        if (const auto* inst = engine.getInstrument())
            if (index >= 0 && index < (int) inst->zones.size() && inst->zones[(size_t) index].sample != nullptr)
                waveform.setFile (inst->zones[(size_t) index].sample->file);
    };

    // Bygg knott-paneler automatisk fra parameterbeskrivelsen
    const auto infos = engine.params.describe();
    const std::vector<std::pair<juce::String, juce::String>> sections {
        { "ENVELOPE", "ENVELOPE" }, { "FILTER", "FILTER & PAN" }, { "LOOP", "LOOP" },
        { "LFO", "LFO" }, { "EFFEKTER", "EFFEKTER" }, { "MASTER", "MASTER" } };

    for (const auto& [id, title] : sections)
    {
        std::vector<ParamInfo> subset;

        for (const auto& info : infos)
            if (id == info.section)
                subset.push_back (info);

        panels.push_back (std::make_unique<KnobPanel> (title, subset));
        addAndMakeVisible (*panels.back());
    }

    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Theme::accent.withAlpha (0.7f));
    keyboard.setAvailableRange (36, 96);
    keyboard.setLowestVisibleKey (36);
    keyboard.setWantsKeyboardFocus (true);
    addAndMakeVisible (keyboard);

    keyboardState.addListener (this);

    // Aktiver alle tilkoblede MIDI-enheter
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled (device.identifier, true);

    deviceManager.addMidiInputDeviceCallback ({}, this);

    addMouseListener (this, true); // for å gi tastaturet fokus igjen etter klikk på knotter

    setSize (1100, 700);
    setAudioChannels (0, 2);
    startTimerHz (30);
    keyboard.grabKeyboardFocus();
}

MainComponent::~MainComponent()
{
    stopTimer();
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    keyboardState.removeListener (this);
    shutdownAudio();
    setLookAndFeel (nullptr);
}

// ------------------------------------------------------------------ Lyd

void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    engine.prepare (sampleRate, juce::jmax (samplesPerBlockExpected, 512));
    midiCollector.reset (sampleRate);
    incomingMidi.ensureSize (4096);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    incomingMidi.clear();
    midiCollector.removeNextBlockOfMessages (incomingMidi, info.numSamples);

    juce::AudioBuffer<float> view (info.buffer->getArrayOfWritePointers(),
                                   info.buffer->getNumChannels(),
                                   info.startSample,
                                   info.numSamples);

    engine.render (view, incomingMidi);
}

void MainComponent::releaseResources() {}

// ----------------------------------------------------------------- MIDI

void MainComponent::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    // Noter går via tastaturtilstanden (oppdaterer visningen og kommer til motoren via listeneren).
    // Alt annet (pitch bend, sustain-pedal, ...) går rett til motoren.
    if (message.isNoteOnOrOff())
        keyboardState.processNextMidiEvent (message);
    else
        midiCollector.addMessageToQueue (message);
}

void MainComponent::handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    midiCollector.addMessageToQueue (juce::MidiMessage::noteOn (channel, note, velocity).withTimeStamp (nowSeconds()));
}

void MainComponent::handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    midiCollector.addMessageToQueue (juce::MidiMessage::noteOff (channel, note, velocity).withTimeStamp (nowSeconds()));
}

// ----------------------------------------------------- Filer og instrument

bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const juce::File file (f);

        if (file.hasFileExtension ("sfz") || file.hasFileExtension ("samplerpreset")
            || formats.findFormatForFileExtension (file.getFileExtension()) != nullptr)
            return true;
    }

    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.isEmpty())
        return;

    const juce::File file (files[0]);

    if (file.hasFileExtension ("samplerpreset"))
        readPreset (file);
    else
        loadInstrumentFile (file);
}

void MainComponent::chooseInstrument()
{
    chooser = std::make_unique<juce::FileChooser> ("Velg en lydfil eller SFZ-instrument",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   formats.getWildcardForAllFormats() + ";*.sfz");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto result = fc.getResult();

                              if (result.existsAsFile())
                                  loadInstrumentFile (result);
                          });
}

void MainComponent::loadInstrumentFile (const juce::File& file)
{
    if (loading.exchange (true))
        return;

    statusLabel.setText ("Laster " + file.getFileName() + " ...", juce::dontSendNotification);

    juce::Component::SafePointer<MainComponent> safe (this);

    // Lasting skjer i bakgrunnen med egen format-manager, så UI-et henger ikke på store SFZ-biblioteker.
    juce::Thread::launch ([safe, file]
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        juce::String message;
        auto instrument = loadInstrumentFromFile (file, fm, message);
        auto holder = std::make_shared<std::unique_ptr<Instrument>> (std::move (instrument));

        juce::MessageManager::callAsync ([safe, holder, message, file]
        {
            if (safe != nullptr)
                safe->finishLoading (std::move (*holder), message, file);
        });
    });
}

void MainComponent::finishLoading (std::unique_ptr<Instrument> instrument, const juce::String& message, const juce::File& file)
{
    loading = false;

    if (instrument == nullptr)
    {
        statusLabel.setText (message, juce::dontSendNotification);
        return;
    }

    const int zones = (int) instrument->zones.size();
    const auto name = instrument->name;
    const auto firstFile = instrument->zones[0].sample->file;

    engine.loadInstrument (std::move (instrument));
    zoneMap.setInstrument (engine.getInstrument());
    waveform.setFile (firstFile);
    currentInstrumentFile = file;

    statusLabel.setText (name + "  |  " + juce::String (zones) + (zones == 1 ? " zone" : " zones")
                             + (message.isNotEmpty() ? "  |  " + message : juce::String()),
                         juce::dontSendNotification);
    keyboard.grabKeyboardFocus();
}

// -------------------------------------------------------------- Presets

void MainComponent::savePreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Lagre preset", presetDirectory(), "*.samplerpreset");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto result = fc.getResult();

                              if (result != juce::File())
                                  writePreset (result.withFileExtension ("samplerpreset"));
                          });
}

void MainComponent::loadPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Last preset", presetDirectory(), "*.samplerpreset");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto result = fc.getResult();

                              if (result.existsAsFile())
                                  readPreset (result);
                          });
}

void MainComponent::writePreset (const juce::File& file)
{
    juce::XmlElement xml ("SamplerPreset");
    xml.setAttribute ("version", 1);

    if (currentInstrumentFile != juce::File())
        xml.setAttribute ("instrument", currentInstrumentFile.getFullPathName());

    for (const auto& p : engine.params.describe())
        xml.setAttribute (p.id, (double) p.value->get());

    statusLabel.setText (xml.writeTo (file) ? "Preset lagret: " + file.getFileName()
                                            : "Kunne ikke lagre " + file.getFileName(),
                         juce::dontSendNotification);
}

void MainComponent::readPreset (const juce::File& file)
{
    const auto xml = juce::parseXML (file);

    if (xml == nullptr || xml->getTagName() != "SamplerPreset")
    {
        statusLabel.setText ("Ugyldig preset: " + file.getFileName(), juce::dontSendNotification);
        return;
    }

    for (const auto& p : engine.params.describe())
        if (xml->hasAttribute (p.id))
            p.value->set (juce::jlimit (p.min, p.max, (float) xml->getDoubleAttribute (p.id)));

    refreshKnobs();

    const juce::File instrumentFile (xml->getStringAttribute ("instrument"));

    if (instrumentFile.existsAsFile() && instrumentFile != currentInstrumentFile)
        loadInstrumentFile (instrumentFile);
    else
        statusLabel.setText ("Preset lastet: " + file.getFileName(), juce::dontSendNotification);
}

void MainComponent::refreshKnobs()
{
    for (auto& panel : panels)
        panel->refreshFromParams();
}

// ----------------------------------------------------------------- GUI

void MainComponent::timerCallback()
{
    meter.setLevel (engine.fetchPeakLevel());
    engine.collectGarbage();

    if ((++tick & 1) == 0)
        refreshKnobs();
}

void MainComponent::mouseUp (const juce::MouseEvent&)
{
    keyboard.grabKeyboardFocus();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto top = area.removeFromTop (40);
    loadButton.setBounds (top.removeFromLeft (210));
    top.removeFromLeft (8);
    saveButton.setBounds (top.removeFromLeft (130));
    top.removeFromLeft (8);
    presetButton.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (14);
    statusLabel.setBounds (top);
    area.removeFromTop (10);

    auto keyboardArea = area.removeFromBottom (100);
    area.removeFromBottom (10);
    auto rowB = area.removeFromBottom (128);
    area.removeFromBottom (8);
    auto rowA = area.removeFromBottom (128);
    area.removeFromBottom (8);

    // Midtre rad: bølgeform, zone-kart og nivåmåler
    meter.setBounds (area.removeFromRight (18));
    area.removeFromRight (8);
    zoneMap.setBounds (area.removeFromRight (juce::roundToInt ((float) area.getWidth() * 0.36f)));
    area.removeFromRight (8);
    waveform.setBounds (area);

    keyboard.setBounds (keyboardArea);
    keyboard.setKeyWidth ((float) keyboardArea.getWidth() / 36.0f);

    auto layoutRow = [this] (juce::Rectangle<int> row, std::initializer_list<int> indices, std::initializer_list<int> weights)
    {
        int totalWeight = 0;
        for (int w : weights) totalWeight += w;

        const int totalWidth = row.getWidth();
        auto wi = weights.begin();

        for (int index : indices)
        {
            const int width = juce::roundToInt ((float) totalWidth * (float) *wi / (float) totalWeight);
            panels[(size_t) index]->setBounds (row.removeFromLeft (width).reduced (4, 0));
            ++wi;
        }
    };

    layoutRow (rowA, { 0, 1, 2 }, { 5, 4, 4 });
    layoutRow (rowB, { 3, 4, 5 }, { 4, 5, 2 });
}
