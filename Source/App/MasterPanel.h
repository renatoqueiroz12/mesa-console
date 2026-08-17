#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "ChannelStrip.h"
#include "../Core/MixerEngine.h"
#include "../Core/AutomationEngine.h"

/** Fader pequeno da secao de monitoracao: rotulo em cima, leitura embaixo.
    Encoder rotativo e ruim em tela de toque — por isso fader, nao knob. */
class MiniFader : public juce::Component
{
public:
    MiniFader (juce::String lbl, std::atomic<float>& target, float initialDb)
        : label (std::move (lbl)), param (target)
    {
        pos = juce::jlimit (0.0f, 1.0f, (initialDb + 60.0f) / 60.0f);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();

        g.setColour (theme::textDim);
        g.setFont (theme::mono (8.5f));
        g.drawText (label, r.removeFromTop (12), juce::Justification::centred, false);

        auto valueArea = r.removeFromBottom (14);
        auto box = r.reduced (2, 2).toFloat();

        g.setColour (theme::faderWell);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        const float top = box.getY() + 13.0f, travel = box.getHeight() - 26.0f;
        g.setColour (theme::slot);
        g.fillRoundedRectangle (box.getCentreX() - 3.0f, top, 6.0f, travel, 2.0f);

        const float y = top + travel * (1.0f - pos);
        juce::Rectangle<float> cap (box.getCentreX() - 15.0f, y - 9.0f, 30.0f, 18.0f);
        g.setGradientFill (juce::ColourGradient (theme::capTop, 0.0f, cap.getY(),
                                                 theme::capBot, 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 6.0f, 2.0f);

        g.setColour (theme::text);
        g.setFont (theme::mono (10.0f));
        g.drawText (pos <= 0.01f ? juce::String::fromUTF8 ("-\xe2\x88\x9e")
                                 : juce::String (int (-60.0f + pos * 60.0f)) + " dB",
                    valueArea, juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }

private:
    void drag (const juce::MouseEvent& e)
    {
        const float top = 15.0f, travel = float (getHeight()) - 12.0f - 14.0f - 30.0f;
        if (travel <= 0.0f) return;
        pos = juce::jlimit (0.0f, 1.0f, 1.0f - (float (e.y) - top) / travel);
        param.store (pos <= 0.01f ? -100.0f : -60.0f + pos * 60.0f);
        repaint();
    }

    juce::String label;
    std::atomic<float>& param;
    float pos = 0.7f;
};

/** Coluna da direita: monitoracao do controle, timer do estudio e automacao. */
class MasterPanel : public juce::Component
{
public:
    MasterPanel (mesa::MixerEngine& m, mesa::AutomationEngine& a) : mix (m), autom (a)
    {
        static const char* monLabels[] = { "PGM 1", "PGM 2", "PGM 3", "PGM 4", "EXT 1", "EXT 2" };
        for (int i = 0; i < 6; ++i)
        {
            monSrc[i] = std::make_unique<SurfaceButton> (monLabels[i], theme::busGreen, 11.0f);
            monSrc[i]->onClick = [this, i]
            {
                mix.monitor.source.store (i);
                for (int k = 0; k < 6; ++k) monSrc[k]->setActive (k == i);
                repaint();
            };
            addAndMakeVisible (*monSrc[i]);
        }
        monSrc[juce::jlimit (0, 5, mix.monitor.source.load())]->setActive (true);

        monitorFader = std::make_unique<MiniFader> ("MONITOR", mix.monitor.monitorDb, mix.monitor.monitorDb.load());
        phonesFader  = std::make_unique<MiniFader> ("FONE",    mix.monitor.phonesDb,  mix.monitor.phonesDb.load());
        cueFader     = std::make_unique<MiniFader> ("CUE",     mix.monitor.cueDb,     mix.monitor.cueDb.load());
        addAndMakeVisible (*monitorFader);
        addAndMakeVisible (*phonesFader);
        addAndMakeVisible (*cueFader);

        static const char* keyLabels[] = { "TALK BF", "TALK EST", "REC", "PERFIL", "METER", "RELOGIO" };
        for (int i = 0; i < 6; ++i)
        {
            keys[i] = std::make_unique<SurfaceButton> (keyLabels[i], theme::busGreen, 10.0f);
            keys[i]->onClick = [this, i]
            {
                keys[i]->setActive (! keys[i]->isActive());
                if (i == 1) mix.monitor.talkToStudio.store (keys[i]->isActive());
            };
            addAndMakeVisible (*keys[i]);
        }

        static const char* stLabels[] = { "PGM 1", "PGM 2", "PGM 3", "EXT 1" };
        for (int i = 0; i < 4; ++i)
        {
            stSrc[i] = std::make_unique<SurfaceButton> (stLabels[i], theme::busGreen, 11.0f);
            stSrc[i]->onClick = [this, i]
            {
                studioSource = i;
                for (int k = 0; k < 4; ++k) stSrc[k]->setActive (k == i);
                repaint();
            };
            addAndMakeVisible (*stSrc[i]);
        }
        stSrc[0]->setActive (true);

        reset   = std::make_unique<SurfaceButton> ("RESET",    theme::busGreen, 11.0f);
        runStop = std::make_unique<SurfaceButton> ("RUN/STOP", theme::busGreen, 11.0f);
        reset->onClick   = [this] { timerStartMs = juce::Time::getMillisecondCounter(); repaint(); };
        runStop->onClick = [this] { runStop->setActive (! runStop->isActive()); };
        addAndMakeVisible (*reset);
        addAndMakeVisible (*runStop);

        autoBtn = std::make_unique<SurfaceButton> ("AUTOMACAO ...", theme::wide, 11.0f);
        autoBtn->onClick = [this] { if (onOpenAutomation) onOpenAutomation(); };
        addAndMakeVisible (*autoBtn);
    }

    std::function<void()> onOpenAutomation;

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::mono (9.0f));
        g.drawText ("MONITOR \xc2\xb7 CONTROLE", title1, juce::Justification::centredLeft, false);
        g.drawText ("ESTUDIO \xc2\xb7 TIMER",    title2, juce::Justification::centredLeft, false);
        g.drawText ("AUTOMACAO DE CAMERAS",     title3, juce::Justification::centredLeft, false);

        // OLED com as fontes de monitor
        theme::drawOled (g, srcBox);
        auto s = srcBox.reduced (9, 6);
        drawLine (g, s.removeFromTop (16), "MONITOR CR", srcName (mix.monitor.source.load()));
        drawLine (g, s.removeFromTop (16), "ESTUDIO",    srcName (studioSource));

        // CAM no ar
        theme::drawOled (g, camBox);
        g.setColour (autom.camera() > 0 ? theme::onRed : theme::oledDim);
        g.setFont (theme::mono (26.0f, true));
        g.drawText ("CAM " + juce::String (autom.camera()), camBox,
                    juce::Justification::centred, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        title1 = r.removeFromTop (14);
        srcBox = r.removeFromTop (44);
        r.removeFromTop (6);

        auto grid = r.removeFromTop (60);
        layoutGrid (grid, monSrc, 6, 3);
        r.removeFromTop (6);

        auto faders = r.removeFromTop (150);
        const int fw = faders.getWidth() / 3;
        monitorFader->setBounds (faders.removeFromLeft (fw).reduced (2, 0));
        phonesFader ->setBounds (faders.removeFromLeft (fw).reduced (2, 0));
        cueFader    ->setBounds (faders.reduced (2, 0));
        r.removeFromTop (6);

        auto keyGrid = r.removeFromTop (60);
        layoutGrid (keyGrid, keys, 6, 3);
        r.removeFromTop (8);

        title2 = r.removeFromTop (14);
        auto stGrid = r.removeFromTop (28);
        layoutGrid (stGrid, stSrc, 4, 4);
        r.removeFromTop (5);

        auto tRow = r.removeFromTop (28);
        const int halfW = (tRow.getWidth() - 5) / 2;
        reset  ->setBounds (tRow.removeFromLeft (halfW));
        runStop->setBounds (tRow.removeFromRight (halfW));
        r.removeFromTop (8);

        title3 = r.removeFromTop (14);
        camBox = r.removeFromTop (52);
        r.removeFromTop (6);
        autoBtn->setBounds (r.removeFromTop (28));
    }

private:
    template <typename Array>
    void layoutGrid (juce::Rectangle<int> area, Array& items, int count, int cols)
    {
        const int rows = (count + cols - 1) / cols;
        const int cellH = (area.getHeight() - (rows - 1) * 5) / rows;
        for (int rIdx = 0; rIdx < rows; ++rIdx)
        {
            auto rowArea = area.removeFromTop (cellH);
            area.removeFromTop (5);
            const int cellW = (rowArea.getWidth() - (cols - 1) * 5) / cols;
            for (int c = 0; c < cols; ++c)
            {
                const int i = rIdx * cols + c;
                if (i >= count) break;
                items[i]->setBounds (rowArea.removeFromLeft (cellW));
                rowArea.removeFromLeft (5);
            }
        }
    }

    void drawLine (juce::Graphics& g, juce::Rectangle<int> row,
                   const char* label, const juce::String& value)
    {
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (10.0f));
        g.drawText (label, row, juce::Justification::centredLeft, false);
        g.setColour (theme::oled);
        g.drawText (value, row, juce::Justification::centredRight, false);
    }

    static juce::String srcName (int i)
    {
        static const char* n[] = { "PGM 1", "PGM 2", "PGM 3", "PGM 4", "EXT 1", "EXT 2" };
        return (i >= 0 && i < 6) ? n[i] : "--";
    }

    mesa::MixerEngine& mix;
    mesa::AutomationEngine& autom;
    std::unique_ptr<SurfaceButton> monSrc[6], keys[6], stSrc[4], reset, runStop, autoBtn;
    std::unique_ptr<MiniFader> monitorFader, phonesFader, cueFader;
    juce::Rectangle<int> title1, title2, title3, srcBox, camBox;
    int studioSource = 0;
    juce::uint32 timerStartMs = juce::Time::getMillisecondCounter();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterPanel)
};
