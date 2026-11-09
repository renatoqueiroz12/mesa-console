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

        // Duas linhas centradas na caixa: antes elas ficavam no topo e sobrava
        // uma faixa morta embaixo, que so ocupava altura util da mesa.
        auto centra = [] (juce::Rectangle<int> caixa)
        {
            return caixa.withSizeKeepingCentre (caixa.getWidth() - 22, 50);
        };

        // REGUA COM NUMEROS, na mesma lingua do fader e das tiras.
        //
        // A barra ja estava na escala certa - 0 vale -18 dBFS desde que a
        // referencia virou EBU - mas sem numero nenhum ela dizia so "mais" e
        // "menos". Nao dava para ver onde estava o 0 nem quanto faltava para o
        // teto, que e o que se olha numa barra de programa.
        auto l = centra (leftBox);
        drawRow (g, l.removeFromTop (16), "PGM 1", mix.busMeter[0].peakDb(),
                 mix.busMeterR[0].peakDb());
        l.removeFromTop (4);
        drawRow (g, l.removeFromTop (16), "PGM 2", mix.busMeter[1].peakDb(),
                 mix.busMeterR[1].peakDb());
        l.removeFromTop (1);
        drawEscala (g, l.removeFromTop (11));

        auto r = centra (rightBox);
        drawRow (g, r.removeFromTop (16), "PGM 3", mix.busMeter[2].peakDb(),
                 mix.busMeterR[2].peakDb());
        r.removeFromTop (4);
        drawRow (g, r.removeFromTop (16), "PGM 4", mix.busMeter[3].peakDb(),
                 mix.busMeterR[3].peakDb());
        r.removeFromTop (1);
        drawEscala (g, r.removeFromTop (11));

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
    /** A regua, alinhada com a barra das duas linhas acima.

        Os recortes repetem os do drawRow de proposito: rotulo a esquerda,
        valor a direita, barra no meio. Numero que nao cai em cima da marca que
        ele nomeia nao serve para nada. */
    void drawEscala (juce::Graphics& g, juce::Rectangle<int> row)
    {
        row.removeFromLeft (56 + 9);
        row.removeFromRight (50 + 9);

        auto r = row.toFloat();
        static const float marcas[] = { -40.0f, -30.0f, -20.0f, -10.0f, 0.0f, 10.0f };

        g.setFont (theme::mono (8.0f));
        for (float vu : marcas)
        {
            if (vu > theme::kMeterTopVu || vu < theme::kMeterBotVu) continue;

            const float x = r.getX() + r.getWidth()
                          * ((vu - theme::kMeterBotVu)
                             / (theme::kMeterTopVu - theme::kMeterBotVu));
            const bool zero = std::abs (vu) < 0.01f;

            g.setColour (juce::Colours::white.withAlpha (zero ? 0.40f : 0.15f));
            g.fillRect (x - 0.5f, r.getY(), 1.0f, 3.0f);

            g.setColour (zero ? theme::oled : theme::oledDim);
            g.drawText (juce::String (int (vu)),
                        juce::Rectangle<float> (x - 12.0f, r.getY() + 3.0f, 24.0f, 8.0f),
                        juce::Justification::centred, false);
        }
    }

    /** Linha de programa ESTEREO: duas barras empilhadas, L em cima e R
        embaixo. A leitura numerica mostra o MAIOR dos dois - e o numero que
        responde "isto vai estourar?", que e para o que se olha a barra de
        programa. */
    void drawRow (juce::Graphics& g, juce::Rectangle<int> row,
                  const char* label, float dbL, float dbR)
    {
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (12.0f));
        g.drawText (label, row.removeFromLeft (56), juce::Justification::centredLeft, false);
        row.removeFromLeft (9);

        auto valueArea = row.removeFromRight (50);
        row.removeFromRight (9);

        auto barras = row.toFloat().withSizeKeepingCentre (float (row.getWidth()), 15.0f);
        theme::drawBarVu (g, barras.removeFromTop (7.0f), dbL);
        barras.removeFromTop (1.0f);
        theme::drawBarVu (g, barras.removeFromTop (7.0f), dbR);

        g.setColour (theme::oled);
        g.setFont (theme::mono (12.0f));
        g.drawText (theme::fmtDb (juce::jmax (dbL, dbR)), valueArea,
                    juce::Justification::centredRight, false);
    }

    mesa::MixerEngine& mix;
    juce::Rectangle<int> leftBox, rightBox, clockBox;
};
