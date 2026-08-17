#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Core/Channel.h"

/** Strip minimo: nome, meter, fader, ON/OFF empilhados, CUE. */
class StripComponent : public juce::Component
{
public:
    StripComponent (mesa::Channel& c, int number) : ch (c), index (number)
    {
        name.setText ("CH " + juce::String (number + 1), juce::dontSendNotification);
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::Font (juce::FontOptions (11.0f)));
        addAndMakeVisible (name);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
        fader.setRange (-60.0, 10.0, 0.1);
        fader.setValue (0.0, juce::dontSendNotification);
        fader.onValueChange = [this] { ch.params.faderDb.store (float (fader.getValue())); };
        ch.params.faderDb.store (0.0f);
        addAndMakeVisible (fader);

        on .setButtonText ("ON");
        off.setButtonText ("OFF");
        on .onClick = [this] { ch.params.on.store (true);  repaint(); };
        off.onClick = [this] { ch.params.on.store (false); repaint(); };
        addAndMakeVisible (on);
        addAndMakeVisible (off);

        cue.setButtonText ("CUE");
        cue.setClickingTogglesState (true);
        cue.onClick = [this] { ch.params.cue.store (cue.getToggleState()); };
        addAndMakeVisible (cue);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1b1e22));
        g.setColour (juce::Colour (0xff0a0b0d));
        g.drawRect (getLocalBounds());

        // meter de saida
        auto r = meterArea.toFloat();
        g.setColour (juce::Colour (0xff07090b));
        g.fillRect (r);
        const float db = ch.meterOut.peakDb();
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -3.0f ? juce::Colours::red
                                : db > -12.0f ? juce::Colours::orange
                                              : juce::Colour (0xff8fe3ff));
        g.fillRect (r.withTop (r.getBottom() - r.getHeight() * norm));

        // lampada do ON
        g.setColour (ch.params.on.load() ? juce::Colours::red.withAlpha (0.9f)
                                         : juce::Colour (0xff2a2f35));
        g.fillRect (onLamp.toFloat());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        name.setBounds (r.removeFromTop (16));
        onLamp = r.removeFromTop (3);
        r.removeFromTop (4);
        cue.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        auto bottom = r.removeFromBottom (76);          // ON acima de OFF, botoes grandes
        on .setBounds (bottom.removeFromTop (36).reduced (0, 1));
        off.setBounds (bottom.removeFromTop (36).reduced (0, 1));

        meterArea = r.removeFromRight (10);
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    mesa::Channel& ch;
    int index;
    juce::Label name;
    juce::Slider fader;
    juce::TextButton on, off, cue;
    juce::Rectangle<int> meterArea, onLamp;
};
