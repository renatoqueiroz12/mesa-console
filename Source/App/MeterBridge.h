#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "../Core/MixerEngine.h"

/** Ponte de medidores: dois blocos de OLED com as barras dos buses e do master,
    e o relogio a direita. Redesenhada pelo timer da tela, como no mockup. */
class MeterBridge : public juce::Component
{
public:
    explicit MeterBridge (mesa::MixerEngine& m) : mix (m) {}

    void paint (juce::Graphics& g) override
    {
        theme::drawOled (g, leftBox);
        theme::drawOled (g, rightBox);
        theme::drawOled (g, clockBox);

        auto l = leftBox.reduced (11, 8);
        drawRow (g, l.removeFromTop (16), "PGM 1", mix.busMeter[0].peakDb());
        l.removeFromTop (7);
        drawRow (g, l.removeFromTop (16), "PGM 2", mix.busMeter[1].peakDb());

        auto r = rightBox.reduced (11, 8);
        drawRow (g, r.removeFromTop (16), "PGM 3", mix.busMeter[2].peakDb());
        r.removeFromTop (7);
        drawRow (g, r.removeFromTop (16), "PGM 4", mix.busMeter[3].peakDb());

        auto c = clockBox.reduced (10, 6);
        g.setColour (theme::oled);
        g.setFont (theme::mono (23.0f));
        g.drawText (juce::Time::getCurrentTime().toString (false, true, true, true),
                    c.removeFromTop (26), juce::Justification::centred, false);
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (14.0f));
        const int secs = int (juce::Time::getMillisecondCounter() / 1000) % 3600;
        g.drawText (juce::String (secs / 60).paddedLeft ('0', 2) + ":"
                    + juce::String (secs % 60).paddedLeft ('0', 2),
                    c.removeFromTop (18), juce::Justification::centred, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        clockBox = r.removeFromRight (132);
        r.removeFromRight (8);
        const int half = (r.getWidth() - 8) / 2;
        leftBox  = r.removeFromLeft (half);
        rightBox = r.removeFromRight (half);
    }

private:
    void drawRow (juce::Graphics& g, juce::Rectangle<int> row,
                  const char* label, float db)
    {
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (12.0f));
        g.drawText (label, row.removeFromLeft (56), juce::Justification::centredLeft, false);
        row.removeFromLeft (9);

        auto valueArea = row.removeFromRight (50);
        row.removeFromRight (9);
        theme::drawBar (g, row.toFloat().withSizeKeepingCentre (float (row.getWidth()), 14.0f),
                        theme::dbToNorm (db));

        g.setColour (theme::oled);
        g.setFont (theme::mono (12.0f));
        g.drawText (theme::fmtDb (db), valueArea, juce::Justification::centredRight, false);
    }

    mesa::MixerEngine& mix;
    juce::Rectangle<int> leftBox, rightBox, clockBox;
};
