#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

/** Menu lateral das configuracoes, no formato do painel do QOR.

    Por que nao as abas do JUCE na lateral: elas GIRAM o texto 90 graus e
    empurram o que nao cabe para um botao "+". Com dez paginas o resultado foi
    um nome deitado e o resto escondido — pior do que as abas no topo.

    Aqui cada item e uma linha comum, com o nome na horizontal, agrupado por
    secao. E o formato que o QOR usa, e ele funciona pelo motivo obvio: dez
    nomes legiveis, um abaixo do outro, e a pagina inteira ao lado. */
class MenuLateral : public juce::Component
{
public:
    /** A macro de nao-copiavel do JUCE tambem some com o construtor padrao. */
    MenuLateral() = default;

    std::function<void (int)> aoEscolher;

    /** Titulo de secao: nao clicavel, so organiza. */
    void addSecao (const juce::String& titulo)
    {
        itens.push_back ({ titulo, -1, true });
    }

    /** Item clicavel; devolve o indice da pagina. */
    void addItem (const juce::String& nome, int indicePagina)
    {
        itens.push_back ({ nome, indicePagina, false });
    }

    void limpa() { itens.clear(); repaint(); }

    void seleciona (int indicePagina)
    {
        escolhido = indicePagina;
        repaint();
    }

    int escolhidoAgora() const noexcept { return escolhido; }

    int alturaNecessaria() const
    {
        int h = 8;
        for (const auto& i : itens) h += i.secao ? kAltSecao : kAltItem;
        return h;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::surfaceLo);

        int y = 8;
        for (const auto& i : itens)
        {
            const int h = i.secao ? kAltSecao : kAltItem;
            const juce::Rectangle<int> linha (0, y, getWidth(), h);

            if (i.secao)
            {
                g.setColour (theme::textDim);
                g.setFont (theme::mono (11.0f, true));
                g.drawText (i.nome.toUpperCase(), linha.reduced (10, 0).withTrimmedTop (6),
                            juce::Justification::bottomLeft, false);
            }
            else
            {
                const bool ativo = i.pagina == escolhido;
                if (ativo)
                {
                    g.setColour (theme::busGreen.withAlpha (0.22f));
                    g.fillRect (linha);
                    g.setColour (theme::busGreen);
                    g.fillRect (linha.withWidth (3));
                }

                g.setColour (ativo ? theme::text : theme::textDim);
                g.setFont (theme::mono (12.0f, ativo));
                g.drawText (i.nome, linha.reduced (22, 0),
                            juce::Justification::centredLeft, true);
            }
            y += h;
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        int y = 8;
        for (const auto& i : itens)
        {
            const int h = i.secao ? kAltSecao : kAltItem;
            if (! i.secao && e.y >= y && e.y < y + h)
            {
                seleciona (i.pagina);
                if (aoEscolher) aoEscolher (i.pagina);
                return;
            }
            y += h;
        }
    }

private:
    static constexpr int kAltItem  = 30;
    static constexpr int kAltSecao = 30;

    struct Item { juce::String nome; int pagina; bool secao; };
    std::vector<Item> itens;
    int escolhido = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MenuLateral)
};
