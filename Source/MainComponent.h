#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

#include "SamplerEngine.h"
#include "Ui.h"

class MainComponent : public juce::AudioAppComponent,
                      public juce::MidiInputCallback,
                      public juce::MidiKeyboardState::Listener,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    // AudioAppComponent
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;
    void releaseResources() override;

    // Component
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    // MidiInputCallback (kalles fra MIDI-tråd)
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;

    // MidiKeyboardState::Listener (skjermtastatur og maskinvare-noter)
    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;

    void chooseInstrument();
    void loadInstrumentFile (const juce::File& file);
    void finishLoading (std::unique_ptr<Instrument> instrument, const juce::String& message, const juce::File& file);

    void savePreset();
    void loadPreset();
    void writePreset (const juce::File& file);
    void readPreset (const juce::File& file);
    void refreshKnobs();

    SamplerEngine engine;
    juce::AudioFormatManager formats;
    SamplerLookAndFeel lookAndFeel;

    juce::MidiKeyboardState keyboardState;
    juce::MidiMessageCollector midiCollector;
    juce::MidiBuffer incomingMidi;

    juce::TextButton loadButton   { "Last inn sample / SFZ..." };
    juce::TextButton saveButton   { "Lagre preset..." };
    juce::TextButton presetButton { "Last preset..." };
    juce::Label statusLabel;

    LevelMeter meter;
    WaveformView waveform;
    ZoneMap zoneMap;
    std::vector<std::unique_ptr<KnobPanel>> panels;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };

    std::unique_ptr<juce::FileChooser> chooser;
    juce::File currentInstrumentFile;
    std::atomic<bool> loading { false };
    int tick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
