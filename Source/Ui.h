#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <vector>

#include "Instrument.h"
#include "Params.h"

namespace Theme
{
    inline const juce::Colour background { 0xff12141a };
    inline const juce::Colour panel      { 0xff1c1f27 };
    inline const juce::Colour panelEdge  { 0xff2a2e39 };
    inline const juce::Colour track      { 0xff343948 };
    inline const juce::Colour knobBody   { 0xff262a35 };
    inline const juce::Colour accent     { 0xff4fd1c5 };
    inline const juce::Colour accent2    { 0xfff6ad55 };
    inline const juce::Colour text       { 0xffe6e8ee };
    inline const juce::Colour textDim    { 0xff8b91a3 };
}

class SamplerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SamplerLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;
};

// En gruppe knotter for én seksjon (ENVELOPE, FILTER, ...), bygget fra ParamInfo.
class KnobPanel : public juce::Component
{
public:
    KnobPanel (const juce::String& title, const std::vector<ParamInfo>& infos);

    void refreshFromParams();   // synkroniserer knotter med parametrene (f.eks. etter preset)

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        ParamInfo info;
        juce::Slider slider;
        juce::Label label;
    };

    juce::String title;
    std::vector<std::unique_ptr<Knob>> knobs;
};

// Viser bølgeformen til valgt sample og loop-området. Loop-punktene kan dras med musen.
class WaveformView : public juce::Component,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    WaveformView (juce::AudioFormatManager& formats, Params& params);
    ~WaveformView() override;

    void setFile (const juce::File& file);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void timerCallback() override { repaint(); }
    float xToFraction (float x) const;

    Params& params;
    juce::AudioThumbnailCache cache { 4 };
    juce::AudioThumbnail thumbnail;
    juce::String fileName;
    bool draggingStart = true;
};

// Kart over zones: taster langs x-aksen, velocity langs y-aksen.
class ZoneMap : public juce::Component
{
public:
    std::function<void (int zoneIndex)> onZoneSelected;

    void setInstrument (const Instrument* instrument);
    int getSelectedZone() const noexcept { return selected; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> zoneBounds (const Zone&) const;

    const Instrument* instrument = nullptr;
    int selected = 0;
};

class LevelMeter : public juce::Component
{
public:
    void setLevel (float newPeak);
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f;
};
