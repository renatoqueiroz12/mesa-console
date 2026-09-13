#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

/** Uma linha de tabela com colunas alinhadas.

    Antes cada linha era um botao com os textos colados por espacos: com nomes
    de tamanhos diferentes nada alinhava, e a lista virava um paredao onde o
    olho nao encontra a coluna. Aqui cada coluna tem sua largura, como na lista
    de Source Profiles do QOR.

    A proporcao e dada em fracao da largura para a tabela acompanhar a janela
    em vez de fixar pixels que quebram quando a tela muda. */
class LinhaTabela : public juce::Component
{
public:
    LinhaTabela() = default;

    std::function<void()> aoClicar;

    void defineColunas (juce::StringArray textos, std::vector<float> fracoes,
                        bool ehCabecalho = false)
    {
        colunas = std::move (textos);
        larguras = std::move (fracoes);
        cabecalho = ehCabecalho;
        repaint();
    }

    void setDestacada (bool v) { destacada = v; repaint(); }

    void paint (juce::Graphics& g) override
    {
        if (cabecalho)
        {
            g.setColour (theme::surfaceHi);
            g.fillRect (getLocalBounds());
        }
        else
        {
            // zebra discreta: ajuda o olho a seguir a linha ate o fim, que e o
            // problema real de uma lista longa
            g.setColour (destacada ? theme::busGreen.withAlpha (0.18f)
                                   : (par ? theme::surfaceLo : theme::surface));
            g.fillRect (getLocalBounds());
        }

        g.setColour (theme::edge);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, float (getWidth()));

        int x = 0;
        for (int i = 0; i < colunas.size(); ++i)
        {
            const float fr = i < int (larguras.size()) ? larguras[size_t (i)] : 0.25f;
            const int w = int (float (getWidth()) * fr);

            g.setColour (cabecalho ? theme::textDim
                                   : (i == 0 ? theme::text : theme::textDim));
            g.setFont (theme::mono (cabecalho ? 10.0f : 12.0f, cabecalho || i == 0));
            g.drawText (colunas[i], juce::Rectangle<int> (x + 10, 0, w - 14, getHeight()),
                        juce::Justification::centredLeft, true);
            x += w;
        }
    }

    void mouseUp (const juce::MouseEvent&) override { if (aoClicar) aoClicar(); }
    void mouseEnter (const juce::MouseEvent&) override { if (! cabecalho) { destacada = true; repaint(); } }
    void mouseExit  (const juce::MouseEvent&) override { if (! cabecalho) { destacada = false; repaint(); } }

    bool par = false;

private:
    juce::StringArray colunas;
    std::vector<float> larguras;
    bool cabecalho = false, destacada = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinhaTabela)
};
