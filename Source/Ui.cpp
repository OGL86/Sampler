#include "Ui.h"

// ------------------------------------------------------------ LookAndFeel

SamplerLookAndFeel::SamplerLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, Theme::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Theme::textDim);
    setColour (juce::TextButton::buttonColourId, Theme::panel);
    setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    setColour (juce::TextButton::textColourOffId, Theme::accent);
    setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
    setColour (juce::PopupMenu::textColourId, Theme::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.withAlpha (0.35f));
}

void SamplerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float startAngle, float endAngle,
                                           juce::Slider&)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const float arcRadius = radius - 3.0f;

    const juce::PathStrokeType stroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Theme::track);
    g.strokePath (track, stroke);

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
    g.setColour (Theme::accent);
    g.strokePath (value, stroke);

    const float bodyRadius = radius - 9.0f;
    g.setColour (Theme::knobBody);
    g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -bodyRadius, 3.0f, bodyRadius * 0.55f, 1.5f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (Theme::text);
    g.fillPath (pointer);
}

// --------------------------------------------------------------- KnobPanel

KnobPanel::KnobPanel (const juce::String& panelTitle, const std::vector<ParamInfo>& infos)
    : title (panelTitle)
{
    for (const auto& info : infos)
    {
        auto k = std::make_unique<Knob>();
        k->info = info;

        auto& s = k->slider;
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 72, 16);
        s.setRange (info.min, info.max, info.step);

        if (info.midpoint > info.min && info.midpoint < info.max)
            s.setSkewFactorFromMidPoint (info.midpoint);

        s.setDoubleClickReturnValue (true, info.def);
        s.setTextValueSuffix (info.suffix);
        s.setNumDecimalPlacesToDisplay (info.step >= 1.0f ? 0 : (info.max >= 100.0f ? 0 : (info.max >= 10.0f ? 1 : 2)));
        s.setValue (info.value->get(), juce::dontSendNotification);

        const juce::String id (info.id);

        if (id == "filtertype")
        {
            s.textFromValueFunction = [] (double v)
            {
                static const char* names[] = { "Av", "Lavpass", "Høypass" };
                return juce::String (juce::CharPointer_UTF8 (names[juce::jlimit (0, 2, (int) std::lround (v))]));
            };
            s.valueFromTextFunction = [] (const juce::String& t)
            {
                return t.startsWithIgnoreCase ("l") ? 1.0 : (t.startsWithIgnoreCase ("h") ? 2.0 : 0.0);
            };
        }
        else if (id == "loop")
        {
            s.textFromValueFunction = [] (double v) { return juce::String (juce::CharPointer_UTF8 (v > 0.5 ? "På" : "Av")); };
            s.valueFromTextFunction = [] (const juce::String& t) { return t.startsWithIgnoreCase ("p") ? 1.0 : 0.0; };
        }

        Knob* raw = k.get();
        s.onValueChange = [raw] { raw->info.value->set ((float) raw->slider.getValue()); };

        k->label.setText (juce::String (juce::CharPointer_UTF8 (info.label)), juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setFont (juce::FontOptions (12.0f));

        addAndMakeVisible (k->slider);
        addAndMakeVisible (k->label);
        knobs.push_back (std::move (k));
    }
}

void KnobPanel::refreshFromParams()
{
    for (auto& k : knobs)
        if (! k->slider.isMouseButtonDown())
            k->slider.setValue (k->info.value->get(), juce::dontSendNotification);
}

void KnobPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (b, 8.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (b, 8.0f, 1.0f);

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (title, getLocalBounds().reduced (12, 6).removeFromTop (14), juce::Justification::centredLeft);
}

void KnobPanel::resized()
{
    if (knobs.empty())
        return;

    auto area = getLocalBounds().reduced (6, 4);
    area.removeFromTop (18);

    const int cellWidth = area.getWidth() / (int) knobs.size();

    for (auto& k : knobs)
    {
        auto cell = area.removeFromLeft (cellWidth);
        k->label.setBounds (cell.removeFromTop (16));
        k->slider.setBounds (cell);
    }
}

// ------------------------------------------------------------ WaveformView

WaveformView::WaveformView (juce::AudioFormatManager& formats, Params& p)
    : params (p), thumbnail (512, formats, cache)
{
    thumbnail.addChangeListener (this);
    startTimerHz (15);
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener (this);
}

void WaveformView::setFile (const juce::File& file)
{
    fileName = file.getFileName();
    thumbnail.setSource (new juce::FileInputSource (file));
    repaint();
}

float WaveformView::xToFraction (float x) const
{
    return juce::jlimit (0.0f, 1.0f, (x - 2.0f) / juce::jmax (1.0f, (float) getWidth() - 4.0f));
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto full = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (full, 8.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (full.reduced (0.5f), 8.0f, 1.0f);

    const auto area = getLocalBounds().reduced (2, 14);

    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour (Theme::accent.withAlpha (0.85f));
        thumbnail.drawChannels (g, area, 0.0, thumbnail.getTotalLength(), 1.0f);
    }
    else
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("Ingen sample lastet")), getLocalBounds(), juce::Justification::centred);
    }

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (fileName, getLocalBounds().reduced (12, 2).removeFromTop (14), juce::Justification::centredLeft);

    // Loop-markører
    const bool loopOn = params.loopEnabled.get() > 0.5f;
    const float w = (float) getWidth() - 4.0f;
    const float x0 = 2.0f + params.loopStart.get() * w;
    const float x1 = 2.0f + params.loopEnd.get() * w;
    const auto marker = Theme::accent2.withAlpha (loopOn ? 1.0f : 0.35f);

    if (loopOn)
    {
        g.setColour (Theme::accent2.withAlpha (0.14f));
        g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()));
    }

    g.setColour (marker);
    g.drawLine (x0, (float) area.getY(), x0, (float) area.getBottom(), 2.0f);
    g.drawLine (x1, (float) area.getY(), x1, (float) area.getBottom(), 2.0f);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    const float f = xToFraction ((float) e.x);
    draggingStart = std::abs (f - params.loopStart.get()) < std::abs (f - params.loopEnd.get());
    mouseDrag (e);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    const float f = xToFraction ((float) e.x);

    if (draggingStart)
        params.loopStart.set (juce::jlimit (0.0f, params.loopEnd.get() - 0.001f, f));
    else
        params.loopEnd.set (juce::jlimit (params.loopStart.get() + 0.001f, 1.0f, f));

    params.loopEnabled.set (1.0f);
}

// ----------------------------------------------------------------- ZoneMap

void ZoneMap::setInstrument (const Instrument* inst)
{
    instrument = inst;
    selected = 0;
    repaint();
}

juce::Rectangle<float> ZoneMap::zoneBounds (const Zone& z) const
{
    const auto area = getLocalBounds().toFloat().reduced (6.0f);
    const float keyW = area.getWidth() / 128.0f;
    const float velH = area.getHeight() / 127.0f;

    return { area.getX() + (float) z.loKey * keyW,
             area.getY() + (float) (127 - z.hiVel) * velH,
             (float) (z.hiKey - z.loKey + 1) * keyW,
             (float) (z.hiVel - z.loVel + 1) * velH };
}

void ZoneMap::paint (juce::Graphics& g)
{
    const auto full = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (full, 8.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (full.reduced (0.5f), 8.0f, 1.0f);

    const auto area = full.reduced (6.0f);

    // Oktavlinjer
    g.setColour (Theme::panelEdge);
    for (int key = 0; key <= 128; key += 12)
    {
        const float x = area.getX() + area.getWidth() * (float) key / 128.0f;
        g.drawLine (x, area.getY(), x, area.getBottom(), 1.0f);
    }

    if (instrument == nullptr || instrument->zones.empty())
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("Ingen zones")), getLocalBounds(), juce::Justification::centred);
        return;
    }

    for (size_t i = 0; i < instrument->zones.size(); ++i)
    {
        const auto& z = instrument->zones[i];
        size_t sampleIndex = 0;

        for (size_t s = 0; s < instrument->samples.size(); ++s)
            if (instrument->samples[s].get() == z.sample)
                sampleIndex = s;

        const auto colour = juce::Colour::fromHSV (std::fmod ((float) sampleIndex * 0.137f + 0.48f, 1.0f), 0.55f, 0.9f, 1.0f);
        const auto r = zoneBounds (z);

        g.setColour (colour.withAlpha (0.28f));
        g.fillRect (r);
        g.setColour (colour.withAlpha (0.85f));
        g.drawRect (r, 1.0f);

        if (r.getWidth() > 46.0f && z.sample != nullptr)
        {
            g.setColour (Theme::text.withAlpha (0.8f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (z.sample->name, r.reduced (3.0f, 1.0f), juce::Justification::topLeft, true);
        }
    }

    if (selected >= 0 && selected < (int) instrument->zones.size())
    {
        g.setColour (Theme::text);
        g.drawRect (zoneBounds (instrument->zones[(size_t) selected]), 2.0f);
    }
}

void ZoneMap::mouseDown (const juce::MouseEvent& e)
{
    if (instrument == nullptr)
        return;

    const auto p = e.position;

    for (int i = (int) instrument->zones.size() - 1; i >= 0; --i)
    {
        if (zoneBounds (instrument->zones[(size_t) i]).contains (p))
        {
            selected = i;

            if (onZoneSelected)
                onZoneSelected (i);

            repaint();
            return;
        }
    }
}

// -------------------------------------------------------------- LevelMeter

void LevelMeter::setLevel (float newPeak)
{
    const float decayed = level * 0.86f;
    const float next = juce::jmax (newPeak, decayed);

    if (std::abs (next - level) > 0.002f)
    {
        level = next;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (b, 4.0f);

    const float db = juce::Decibels::gainToDecibels (level, -60.0f);
    const float frac = juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f);
    const auto inner = b.reduced (3.0f);
    const float h = inner.getHeight() * juce::jlimit (0.0f, 1.0f, frac);

    g.setColour (level >= 0.99f ? juce::Colours::red : (frac > 0.85f ? Theme::accent2 : Theme::accent));
    g.fillRoundedRectangle (inner.getX(), inner.getBottom() - h, inner.getWidth(), h, 2.0f);
}
