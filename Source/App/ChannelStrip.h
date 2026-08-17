#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "../Core/Channel.h"
#include "../Core/TriggerEngine.h"

/** Botao da superficie: retangular, bisel, texto mono. Acende quando ativo.
    Serve para SOFT, PGM 1..4, PREV e para os grandes ON/OFF. */
class SurfaceButton : public juce::Component
{
public:
    SurfaceButton (juce::String label, juce::Colour activeColour, float fontHeight = 12.0f)
        : txt (std::move (label)), acc (activeColour), fh (fontHeight) {}

    std::function<void()> onClick;

    void setActive (bool a) { if (active != a) { active = a; repaint(); } }
    bool isActive() const noexcept { return active; }
    void setText (juce::String t) { txt = std::move (t); repaint(); }
    /** Segunda linha, menor: usada pelos botoes de layer ("LAYER 1-8"). */
    void setSub (juce::String t) { sub = std::move (t); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        if (active)
        {
            g.setGradientFill (juce::ColourGradient (acc.brighter (0.10f), 0.0f, r.getY(),
                                                     acc.darker (0.45f),  0.0f, r.getBottom(), false));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (acc.withAlpha (0.30f));
            g.drawRoundedRectangle (r.expanded (1.0f), 4.0f, 2.0f);   // brilho externo
        }
        else
        {
            auto top = down ? theme::btnTopDown : theme::btnTop;
            auto bot = down ? theme::btnBotDown : theme::btnBot;
            g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(), bot, 0.0f, r.getBottom(), false));
            g.fillRoundedRectangle (r, 3.0f);
        }
        g.setColour (theme::edge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawLine (r.getX() + 3.0f, r.getY() + 1.0f, r.getRight() - 3.0f, r.getY() + 1.0f, 1.0f);

        g.setColour (active ? juce::Colours::white.withAlpha (0.95f) : theme::btnText);
        g.setFont (theme::mono (fh));
        if (sub.isEmpty())
        {
            g.drawText (txt, getLocalBounds(), juce::Justification::centred, false);
        }
        else
        {
            auto b = getLocalBounds();
            g.drawText (txt, b.removeFromTop (b.getHeight() - 26), juce::Justification::centred, false);
            g.setFont (theme::mono (8.5f));
            g.setColour (active ? juce::Colours::white.withAlpha (0.65f) : theme::textDim);
            g.drawFittedText (sub, b, juce::Justification::centred, 2);
        }
    }

    void mouseDown (const juce::MouseEvent&) override { down = true;  repaint(); }
    void mouseUp   (const juce::MouseEvent& e) override
    {
        down = false; repaint();
        if (getLocalBounds().contains (e.getPosition()) && onClick) onClick();
    }

private:
    juce::String txt, sub;
    juce::Colour acc;
    float fh;
    bool active = false, down = false;
};

/** Fader vertical desenhado como o do mockup: rasgo central, escala numerada
    a esquerda, cap com risco branco. Arrasto direto, sem caixa de texto. */
class FaderComponent : public juce::Component
{
public:
    std::function<void (float)> onDbChange;   // recebe dB

    void setDb (float db) { pos = theme::faderDbToPos (db); repaint(); }
    float db() const noexcept { return theme::faderPosToDb (pos); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::faderWell);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        const float top = 16.0f, bottom = r.getHeight() - 16.0f;
        const float travel = bottom - top;

        // escala numerada
        static const char* nums[] = { "+10", "0", "-10", "-20", "-30", "-40", "-50", "-\xe2\x88\x9e" };
        g.setFont (theme::mono (8.0f));
        g.setColour (juce::Colour (0xff7d8794));
        for (int i = 0; i < 8; ++i)
        {
            const float y = top + travel * (float (i) / 7.0f);
            g.drawText (juce::String::fromUTF8 (nums[i]),
                        juce::Rectangle<float> (4.0f, y - 5.0f, 24.0f, 10.0f),
                        juce::Justification::centredLeft, false);
            g.setColour (juce::Colour (0xff4c5560));
            g.fillRect (r.getRight() - 9.0f, y - 0.5f, 6.0f, 1.0f);
            g.setColour (juce::Colour (0xff7d8794));
        }

        // rasgo
        const float cx = r.getCentreX() + 8.0f;
        g.setColour (theme::slot);
        g.fillRoundedRectangle (cx - 3.0f, top, 6.0f, travel, 2.0f);

        // cap
        const float y = top + travel * (1.0f - pos);
        juce::Rectangle<float> cap (cx - 17.0f, y - 11.0f, 34.0f, 22.0f);
        g.setGradientFill (juce::ColourGradient (theme::capTop, 0.0f, cap.getY(),
                                                 theme::capBot, 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 6.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }

private:
    void drag (const juce::MouseEvent& e)
    {
        const float top = 16.0f, travel = float (getHeight()) - 32.0f;
        if (travel <= 0.0f) return;
        pos = juce::jlimit (0.0f, 1.0f, 1.0f - (float (e.y) - top) / travel);
        repaint();
        if (onDbChange) onDbChange (db());
    }
    float pos = 0.0f;
};

/** Tira de canal completa: OLED, SOFT, buses, PREV, medidores de IN e TRIM,
    fader, leitura em dB, ON/OFF e a linha de lampadas do trigger.
    Le o Channel do core a cada refresh; escreve so por atomics. */
class ChannelStrip : public juce::Component
{
public:
    /** onSoft recebe o indice global do canal — quem abre o menu e a MainComponent. */
    ChannelStrip (mesa::Channel& c, int globalIndex, std::function<void (int)> onSoft)
        : ch (c), index (globalIndex),
          soft ("SOFT ...", theme::oled),
          autoBtn ("AUTO", theme::wide, 10.0f),
          preview ("PREV", theme::prev),
          bigOn ("ON", theme::onRed, 15.0f),
          bigOff ("OFF", juce::Colour (0xff414852), 15.0f)
    {
        soft.onClick = [this, onSoft] { if (onSoft) onSoft (index); };
        addAndMakeVisible (soft);

        for (int b = 0; b < 4; ++b)
        {
            bus[b] = std::make_unique<SurfaceButton> ("PGM " + juce::String (b + 1), theme::busGreen);
            bus[b]->onClick = [this, b]
            {
                const unsigned mask = ch.params.busMask.load();
                const bool nowOn = ! ((mask >> b) & 1u);
                ch.params.busMask.store (nowOn ? (mask | (1u << b)) : (mask & ~(1u << b)));
                bus[b]->setActive (nowOn);
            };
            addAndMakeVisible (*bus[b]);
        }

        autoBtn.onClick = [this]
        {
            const bool v = ! ch.params.autoMix.enabled.load();
            ch.params.autoMix.enabled.store (v);
            autoBtn.setActive (v);
        };
        addAndMakeVisible (autoBtn);

        preview.onClick = [this]
        {
            const bool v = ! ch.params.cue.load();
            ch.params.cue.store (v);
            preview.setActive (v);
        };
        addAndMakeVisible (preview);

        fader.onDbChange = [this] (float db)
        {
            // Operador encostou no fader: assume o controle. Dois donos do
            // mesmo fader brigariam, e quem manda e quem esta na sala.
            if (ch.params.autoMix.enabled.load())
            {
                ch.params.autoMix.enabled.store (false);
                autoBtn.setActive (false);
            }
            ch.params.faderDb.store (db);
        };
        fader.setDb (ch.params.faderDb.load());
        addAndMakeVisible (fader);

        // ON/OFF: a borda dispara a logica do canal. Apertar de novo reenvia —
        // relancar o cartucho e acao explicita do operador, nao repeticao de estado.
        bigOn .onClick = [this] { if (onPressOnOff) onPressOnOff (index, true);  };
        bigOff.onClick = [this] { if (onPressOnOff) onPressOnOff (index, false); };
        addAndMakeVisible (bigOn);
        addAndMakeVisible (bigOff);

        refresh();
    }

    /** Ligada pela MainComponent: o envio de comando mora la, junto do engine. */
    std::function<void (int, bool)> onPressOnOff;

    /** Chamada pelo timer da tela. Nao toca em nada do audio. */
    void refresh()
    {
        const unsigned mask = ch.params.busMask.load();
        for (int b = 0; b < 4; ++b) bus[b]->setActive ((mask >> b) & 1u);
        preview.setActive (ch.params.cue.load());
        autoBtn.setActive (ch.params.autoMix.enabled.load());

        const bool on = ch.params.on.load();
        bigOn .setActive (on);
        bigOff.setActive (! on);

        const float f = ch.params.faderDb.load();
        if (! juce::approximatelyEqual (f, lastFaderDb)) { fader.setDb (f); lastFaderDb = f; }

        repaint();
    }

    void setTriggerState (mesa::TriggerState s) { trigState = s; }

    void paint (juce::Graphics& g) override
    {
        theme::drawPanel (g, getLocalBounds(), theme::surfaceHi, theme::surfaceLo);

        // ---- OLED do canal: nome, TRIM e camera
        theme::drawOled (g, oledArea);
        auto o = oledArea.reduced (7, 6);
        g.setColour (theme::oled);
        g.setFont (theme::mono (13.0f));
        g.drawText (ch.name.empty() ? juce::String ("--") : juce::String (ch.name),
                    o.removeFromTop (16), juce::Justification::centredLeft, true);

        auto subRow = o.removeFromTop (13);
        g.setFont (theme::mono (10.0f));
        g.setColour (theme::oledDim);
        g.drawText ("TRIM", subRow, juce::Justification::centredLeft, false);
        g.setColour (theme::oled);
        g.drawText (juce::String (ch.params.trimDb.load(), 1),
                    subRow, juce::Justification::centredRight, false);

        const int cam = ch.params.trigger.camera.load();
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (10.0f));
        g.drawText (cam > 0 ? "CAM " + juce::String (cam) + juce::String::fromUTF8 (" \xc2\xb7 AUTO")
                            : juce::String ("SEM AUTOMACAO"),
                    o.removeFromTop (13), juce::Justification::centredLeft, false);

        // ---- medidores de entrada: IN (o que o trigger enxerga) e pos-trim
        theme::drawOled (g, meterArea, 3.0f);
        auto m = meterArea.reduced (6, 6);
        drawMeterRow (g, m.removeFromTop (9), "IN",
                      ch.tapDb (mesa::TapPoint::Input), true);
        m.removeFromTop (4);
        // Segundo medidor: o que o canal REALMENTE entrega ao sistema, ja com
        // o fader. E o par natural do de entrada: um mostra o que chega, outro
        // o que sai.
        drawMeterRow (g, m.removeFromTop (9), "OUT",
                      ch.tapDb (mesa::TapPoint::PostFader), false,
                      ch.params.autoMix.enabled.load());

        // ---- leitura do fader
        g.setColour (theme::faderWell);
        g.fillRoundedRectangle (dbReadout.toFloat(), 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (dbReadout.toFloat(), 3.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f));
        g.drawText (theme::fmtDb (ch.params.faderDb.load()),
                    dbReadout, juce::Justification::centred, false);

        // ---- lampadas: sinal presente e estado do trigger
        auto lampRow = trigRow;
        auto sig = lampRow.removeFromLeft (10).withSizeKeepingCentre (8, 8);
        g.setColour (ch.presence.hasSignal() ? theme::busGreen : juce::Colour (0xff2b3038));
        g.fillEllipse (sig.toFloat());

        lampRow.removeFromLeft (5);
        auto tl = lampRow.removeFromLeft (10).withSizeKeepingCentre (8, 8);
        g.setColour (trigColour());
        g.fillEllipse (tl.toFloat());

        lampRow.removeFromLeft (5);
        g.setColour (theme::textDim);
        g.setFont (theme::mono (9.0f));
        g.drawText (trigState == mesa::TriggerState::Idle
                        ? juce::String ("-") : juce::String (mesa::triggerStateName (trigState)),
                    lampRow,
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 7);

        oledArea = r.removeFromTop (58);
        r.removeFromTop (6);

        soft.setBounds (r.removeFromTop (26));
        r.removeFromTop (6);

        auto busTop = r.removeFromTop (30);
        auto busBottom = r.removeFromTop (30).withTrimmedTop (5);
        const int halfW = (busTop.getWidth() - 5) / 2;
        bus[0]->setBounds (busTop.removeFromLeft (halfW));
        bus[1]->setBounds (busTop.removeFromRight (halfW));
        bus[2]->setBounds (busBottom.removeFromLeft (halfW));
        bus[3]->setBounds (busBottom.removeFromRight (halfW));
        r.removeFromTop (6);

        auto prevRow = r.removeFromTop (28);
        autoBtn.setBounds (prevRow.removeFromRight (56));
        prevRow.removeFromRight (4);
        preview.setBounds (prevRow);
        r.removeFromTop (6);

        meterArea = r.removeFromTop (40);
        r.removeFromTop (6);

        // de baixo para cima: trigger, ON/OFF, leitura. O resto e do fader.
        trigRow = r.removeFromBottom (16);
        r.removeFromBottom (5);
        auto onoff = r.removeFromBottom (72);
        bigOn .setBounds (onoff.removeFromTop (35));
        bigOff.setBounds (onoff.removeFromBottom (35));
        r.removeFromBottom (5);
        dbReadout = r.removeFromBottom (18);
        r.removeFromBottom (4);

        fader.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 74), r.getHeight()));
    }

private:
    void drawMeterRow (juce::Graphics& g, juce::Rectangle<int> row,
                       const char* label, float db, bool withThreshold,
                       bool withAutoTarget = false)
    {
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (8.0f));
        g.drawText (label, row.removeFromLeft (28), juce::Justification::centredLeft, false);
        row.removeFromLeft (4);

        auto bar = row.toFloat();
        theme::drawBar (g, bar, theme::dbToNorm (db));

        if (withAutoTarget)
        {
            // Onde o nivelador esta mirando. O alvo e RMS e a barra mostra pico,
            // entao a marca fica ABAIXO do que a barra bate quando esta certo —
            // sem ela o operador acha que o alvo esta sendo ignorado.
            const float t = ch.params.autoMix.targetDb.load();
            const float x = bar.getX() + bar.getWidth() * theme::dbToNorm (t);
            g.setColour (theme::wide);
            g.fillRect (x - 1.0f, bar.getY() - 1.0f, 2.0f, bar.getHeight() + 2.0f);
        }

        if (withThreshold)
        {
            const float thr = ch.params.trigger.thresholdDb.load();
            const float x = bar.getX() + bar.getWidth() * theme::dbToNorm (thr);
            g.setColour (theme::trig.withAlpha (0.9f));
            g.fillRect (x - 1.0f, bar.getY() - 1.0f, 2.0f, bar.getHeight() + 2.0f);
        }
    }

    juce::Colour trigColour() const
    {
        switch (trigState)
        {
            case mesa::TriggerState::Active:    return theme::trig;
            case mesa::TriggerState::Candidate: return theme::prev;
            case mesa::TriggerState::Cooldown:  return theme::wide.withAlpha (0.55f);
            default:                            return juce::Colour (0xff2b3038);
        }
    }

    mesa::Channel& ch;
    int index;
    SurfaceButton soft, autoBtn, preview, bigOn, bigOff;
    std::unique_ptr<SurfaceButton> bus[4];
    FaderComponent fader;
    juce::Rectangle<int> oledArea, meterArea, dbReadout, trigRow;
    mesa::TriggerState trigState = mesa::TriggerState::Idle;
    float lastFaderDb = -1000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
