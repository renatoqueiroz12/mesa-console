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

    /** Marca de telefone no canto: diz que aquele barramento e o de
        comunicacao com telefones e externas. Desenhada em vetor, e nao como
        caractere, porque simbolo em fonte depende de codificacao e ja
        apareceu quebrado na tela. */
    void setMarcaTelefone (bool v) { if (marcaTel != v) { marcaTel = v; repaint(); } }

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

        if (marcaTel) desenhaTelefone (g);

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
    /** Fone de ouvido classico: dois cantos arredondados e o arco ligando. */
    void desenhaTelefone (juce::Graphics& g) const
    {
        auto r = getLocalBounds().toFloat().removeFromRight (14.0f)
                     .withSizeKeepingCentre (9.0f, 9.0f);

        juce::Path p;
        const float e = r.getWidth() * 0.30f;      // grossura do corpo
        p.addRoundedRectangle (r.getX(), r.getCentreY() - e * 0.5f, e, e * 1.6f, e * 0.35f);
        p.addRoundedRectangle (r.getRight() - e, r.getCentreY() - e * 0.5f, e, e * 1.6f, e * 0.35f);
        p.addCentredArc (r.getCentreX(), r.getCentreY(), r.getWidth() * 0.42f,
                         r.getHeight() * 0.42f, 0.0f,
                         -juce::MathConstants<float>::halfPi,
                         juce::MathConstants<float>::halfPi, true);

        g.setColour (active ? juce::Colours::white.withAlpha (0.85f)
                            : theme::btnText.withAlpha (0.70f));
        g.strokePath (p, juce::PathStrokeType (1.2f));
    }

    juce::String txt, sub;
    juce::Colour acc;
    float fh;
    bool active = false, down = false, marcaTel = false;
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

        // Margem maior que a METADE do cap: no fim do curso ele precisa caber
        // inteiro dentro da caixa, senao some um pedaco.
        //
        // O medidor ao lado usa a MESMA margem (theme::kFaderMargem): sem isso
        // o curso do fader era recuado e a barra ocupava a coluna inteira,
        // entao o mesmo dB caia em alturas diferentes nos dois - a regua da
        // esquerda nao batia com a barra da direita.
        const float top = theme::kFaderMargem;
        const float bottom = r.getHeight() - theme::kFaderMargem;
        const float travel = bottom - top;

        // eixo do rasgo: a escala se apoia nele
        const float cx = r.getCentreX() + 11.0f;

        // ESCALA NUMERADA, CADA NUMERO NA POSICAO DE VERDADE.
        //
        // Antes os rotulos eram distribuidos em espacamento igual, um a cada
        // setimo do curso, enquanto o cap se movia pela escala real. Nunca
        // corresponderam: o "-40" impresso caia a 6/7 do curso e o fader punha
        // -40 a 1/4 dele. Por isso o cap parado em 0,0 nao ficava na marca do
        // zero - e nao havia como ler a posicao pelo desenho.
        //
        // Agora cada rotulo e desenhado onde o proprio faderDbToPos o poe, e a
        // faixa e a mesma dos medidores: o 0 daqui fica na altura do 0 da barra
        // ao lado.
        static const float dbs[] = { 10.0f, 0.0f, -10.0f, -20.0f, -30.0f, -40.0f };
        static const char* nums[] = { "+10", "0", "-10", "-20", "-30", "-40", "-\xe2\x88\x9e" };
        constexpr int kRotulos = 7;

        g.setFont (theme::mono (9.0f, true));
        g.setColour (theme::escalaTexto);
        for (int i = 0; i < kRotulos; ++i)
        {
            // o ultimo e o -infinito: fundo do curso
            const float pos = (i < 6) ? theme::faderDbToPos (dbs[i]) : 0.0f;
            const float y = bottom - travel * pos;
            g.drawText (juce::String::fromUTF8 (nums[i]),
                        juce::Rectangle<float> (2.0f, y - 6.0f, 24.0f, 12.0f),
                        juce::Justification::centredLeft, false);
            // Traco encostado no rasgo, do lado do numero: e assim que se le
            // posicao de fader sem tirar o olho do cap.
            g.setColour (theme::escalaTraco);
            g.fillRect (cx - 21.0f, y - 0.7f, 12.0f, 1.4f);

            // tracos curtos ate o proximo rotulo, tambem na escala de verdade
            if (i < 5)
            {
                const float yProx = bottom - travel * theme::faderDbToPos (dbs[i + 1]);
                for (int k = 1; k <= 2; ++k)
                {
                    const float ym = y + (yProx - y) * (float (k) / 3.0f);
                    g.setColour (theme::escalaTraco.withAlpha (0.5f));
                    g.fillRect (cx - 16.0f, ym - 0.5f, 7.0f, 1.0f);
                }
            }
            g.setColour (theme::escalaTexto);
        }

        // rasgo
        g.setColour (theme::slot);
        g.fillRoundedRectangle (cx - 3.0f, top, 6.0f, travel, 2.0f);

        // cap
        const float y = top + travel * (1.0f - pos);
        // Cap retangular e alto, como o da Axia: alvo generoso para o dedo e
        // silhueta que se acha de relance. Sem contorno colorido — a cor do
        // proprio cap ja destaca, e borda ambar competia com a tally.
        // Retangulo em pe, na proporcao do cap da Axia: mais alto que largo.
        // O nosso estava quase quadrado e nao lia como fader.
        juce::Rectangle<float> cap (cx - 20.0f, y - 31.0f, 40.0f, 62.0f);
        g.setGradientFill (juce::ColourGradient (theme::capTop, 0.0f, cap.getY(),
                                                 theme::capBot, 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 2.5f);
        g.setColour (juce::Colours::black.withAlpha (0.65f));
        g.drawRoundedRectangle (cap, 2.5f, 1.0f);

        // risco central: a linha que o operador usa para ler a posicao
        // risco central: a linha por onde se le a posicao
        g.setColour (theme::capRisco);
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 1.5f, cap.getWidth() - 6.0f, 3.0f);

        // sulcos de pega, distribuidos na altura
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        for (int k = -2; k <= 2; ++k)
            if (k != 0)
                g.fillRect (cap.getX() + 7.0f, cap.getCentreY() + float (k) * 9.0f - 0.75f,
                            cap.getWidth() - 14.0f, 1.5f);
    }

    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }

    /** Duplo clique volta a UNIDADE.

        Convencao de mesa e de DAW, e sem ajuste nenhum: o destino e sempre 0
        dB. Serve para desfazer uma mexida e para chegar na referencia sem
        procurar a marca com o mouse - que e justamente o que se quer quando a
        pressa aperta.

        Nao passa pelo drag: o primeiro clique do duplo ja moveu o fader para
        onde o cursor estava, e voltar ao zero por cima disso e o
        comportamento certo. */
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        pos = theme::faderDbToPos (0.0f);
        repaint();
        if (onDbChange) onDbChange (0.0f);
    }

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
          preview ("CUE", theme::prev),
          recBtn ("REC", theme::onRed, 10.0f),
          bigOn ("ON", theme::onRed, 15.0f),
          bigOff ("OFF", theme::offLaranja, 15.0f)
    {
        soft.onClick = [this, onSoft] { if (onSoft) onSoft (index); };
        addAndMakeVisible (soft);

        for (int b = 0; b < 4; ++b)
        {
            bus[b] = std::make_unique<SurfaceButton> ("PGM " + juce::String (b + 1), theme::busGreen);
            // PGM 4 e o barramento de comunicacao de fabrica, como na Axia
            bus[b]->setMarcaTelefone (b == mesa::kBusComunicacao);
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

        recBtn.onClick = [this] { if (onRec) onRec (index); };
        addAndMakeVisible (recBtn);

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
    /** Aperto no REC daquele canal. */
    std::function<void (int)> onRec;
    void setGravando (bool v) { recBtn.setActive (v); }

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

    /** Ligada pela superficie: a camera deste canal e a que esta no ar. */
    void setOnAir (bool v) { onAir = v; }

    void paint (juce::Graphics& g) override
    {
        theme::drawPanel (g, getLocalBounds(), theme::surfaceHi, theme::surfaceLo);

        // ---- OLED do canal: nome, TRIM e camera
        theme::drawOled (g, oledArea);
        auto o = oledArea.reduced (7, 6);
        g.setColour (theme::oled);
        g.setFont (theme::mono (13.0f));
        // Canal sem fonte se anuncia como INATIVO.
        //
        // "--" nao diz nada: parece nome em branco, e o operador tenta abrir o
        // fader sem entender por que nao sai som. Dizer que esta inativo e a
        // diferenca entre um defeito aparente e um estado conhecido.
        // Inativo por duas razoes: fader vazio, ou input carregado sem fonte
        // escolhida. O segundo caso enganava — mostrava o nome e nao saia som.
        const bool semFonte = ch.name.empty() || ch.params.semFonte.load();

        if (semFonte) g.setColour (theme::oledDim);
        g.drawText (semFonte ? juce::String ("INATIVO") : juce::String (ch.name),
                    o.removeFromTop (16), juce::Justification::centredLeft, true);
        if (semFonte) g.setColour (theme::oled);

        auto subRow = o.removeFromTop (13);
        g.setFont (theme::mono (10.0f));
        g.setColour (theme::oledDim);
        // MOSTRA O GANHO QUE ESTA VALENDO, nao so o que o operador digitou.
        //
        // Existe um auto-trim que soma por cima. Com ele ligado, o numero da
        // tira nao era o ganho aplicado - a tela dizia 0.0 e a entrada podia
        // estar com 12 dB a mais. Numero que nao corresponde ao audio e pior
        // que numero nenhum: manda procurar problema no lugar errado.
        const float trimOp  = ch.params.trimDb.load();
        const float trimAut = ch.params.autoTrim.enabled.load() ? ch.autoTrimDb() : 0.0f;
        const bool  temAuto = std::abs (trimAut) > 0.05f;

        g.drawText (temAuto ? "IN TRIM +AUTO" : "IN TRIM",
                    subRow, juce::Justification::centredLeft, false);
        g.setColour (theme::oled);
        g.drawText (juce::String (trimOp + trimAut, 1),
                    subRow, juce::Justification::centredRight, false);

        const int cam = ch.params.trigger.camera.load();
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (10.0f));
        g.drawText (cam > 0 ? "CAM " + juce::String (cam) + juce::String::fromUTF8 (" \xc2\xb7 AUTO")
                            : juce::String ("SEM AUTOMACAO"),
                    o.removeFromTop (13), juce::Justification::centredLeft, false);

        // ---- medidores VERTICAIS, ladeando o fader
        //
        // Antes eram duas barrinhas horizontais acima do fader, curtas demais
        // para julgar nivel. Na vertical acompanham o curso do fader e ficam no
        // campo de visao de quem opera, que e onde a mao ja esta.
        // IN e DEPOIS do trim de entrada.
        //
        // Media antes, e por isso subir o Input trim gain nao mexia no medidor
        // IN - so aparecia la no OUT, que e o contrario de como se usa o trim.
        // Trim existe para trazer a fonte ao nivel certo, e o nivel certo se
        // confere no medidor da entrada. Quem mede ANTES do trim continua
        // sendo o detector de presenca e o calibrador, que perguntam outra
        // coisa: "esta chegando sinal no conector?".
        // A barra IN ganha a marca do LIMIAR do auto fader: sem ela, ajustar o
        // numero era adivinhar onde ele cai na escala. Com marca, da para ver
        // a barra passar do tracinho e o fader assumir no mesmo instante.
        drawVerticalMeter (g, meterInArea,
                           ch.tapDb  (mesa::TapPoint::PostAutoTrim),
                           ch.tapDbR (mesa::TapPoint::PostAutoTrim), true, false,
                           ch.params.autoMix.enabled.load()
                               ? ch.params.autoMix.floorDb.load() : 1.0f);
        drawVerticalMeter (g, meterOutArea,
                           ch.tapDb  (mesa::TapPoint::PostFader),
                           ch.tapDbR (mesa::TapPoint::PostFader), false,
                           ch.params.autoMix.enabled.load());
        if (! escalaArea.isEmpty()) desenhaEscalaDbfs (g, escalaArea);   // some em tira estreita

        // ---- leitura do fader
        g.setColour (theme::faderWell);
        g.fillRoundedRectangle (dbReadout.toFloat(), 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (dbReadout.toFloat(), 3.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f));
        g.drawText (theme::fmtDb (ch.params.faderDb.load()),
                    dbReadout, juce::Justification::centred, false);

        // ---- TALLY: como a luz de uma mesa de corte.
        //
        // Aqui morava um par de lampadas — presenca de sinal e estado do
        // trigger. A de sinal era redundante: os medidores logo acima dizem a
        // mesma coisa com mais precisao. O que faltava era a informacao que o
        // operador procura de relance: QUEM esta no ar.
        //
        // Vermelho: a camera deste canal esta no ar agora.
        // Ambar: o trigger armou e disputa o corte, mas ainda nao e dele.
        // Apagado: parado.
        const bool armed = trigState == mesa::TriggerState::Active
                        || trigState == mesa::TriggerState::Candidate;

        auto bar = trigRow.toFloat();
        juce::Colour fill = theme::tally().idle;
        juce::Colour ink  = theme::textDim;
        juce::String txt;

        if (onAir)
        {
            fill = theme::tally().onAir;
            ink  = fill.contrasting (0.9f);
            txt  = cam > 0 ? "NO AR  " + juce::String (cam) : juce::String ("NO AR");
        }
        else if (armed)
        {
            fill = theme::tally().armed;
            ink  = fill.contrasting (0.9f);
            txt  = trigState == mesa::TriggerState::Active ? "PRONTO" : "OUVINDO";
        }
        else if (trigState == mesa::TriggerState::Cooldown)
        {
            fill = theme::tally().wait;
            txt  = "ESPERA";
        }
        else if (ch.params.trigger.enabled.load())
        {
            txt = cam > 0 ? "CAM " + juce::String (cam) : juce::String ("SEM CAM");
        }

        g.setColour (fill);
        g.fillRoundedRectangle (bar, 3.0f);
        if (onAir)
        {
            g.setColour (theme::tally().onAir.withAlpha (0.35f));
            g.drawRoundedRectangle (bar.expanded (1.5f), 4.0f, 2.0f);   // brilho
        }
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawRoundedRectangle (bar, 3.0f, 1.0f);

        g.setColour (ink);
        g.setFont (theme::mono (9.5f, onAir));
        g.drawText (txt, trigRow, juce::Justification::centred, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 7);

        oledArea = r.removeFromTop (66);
        r.removeFromTop (6);

        soft.setBounds (r.removeFromTop (30));
        r.removeFromTop (6);

        auto busTop = r.removeFromTop (34);
        auto busBottom = r.removeFromTop (34).withTrimmedTop (5);
        const int halfW = (busTop.getWidth() - 5) / 2;
        bus[0]->setBounds (busTop.removeFromLeft (halfW));
        bus[1]->setBounds (busTop.removeFromRight (halfW));
        bus[2]->setBounds (busBottom.removeFromLeft (halfW));
        bus[3]->setBounds (busBottom.removeFromRight (halfW));
        r.removeFromTop (6);

        // tres botoes na mesma linha: CUE, AUTO e REC
        auto prevRow = r.removeFromTop (32);
        const int larg = (prevRow.getWidth() - 8) / 3;
        preview.setBounds (prevRow.removeFromLeft (larg));
        prevRow.removeFromLeft (4);
        autoBtn.setBounds (prevRow.removeFromLeft (larg));
        prevRow.removeFromLeft (4);
        recBtn.setBounds (prevRow);
        r.removeFromTop (6);


        // RODAPE DA TIRA, como na Axia: leitura, ON, OFF e tally atravessam a
        // largura toda, abaixo do fader. Botao de largura cheia e mais facil
        // de acertar sem olhar — e ON/OFF e o que a mao mais usa no ar.
        //
        // Sobrou altura para isso porque a faixa inferior da mesa saiu: o que
        // era rodape da JANELA virou rodape da TIRA, onde ele pertence.
        trigRow = r.removeFromBottom (22);
        r.removeFromBottom (4);
        bigOff.setBounds (r.removeFromBottom (60));
        r.removeFromBottom (4);
        bigOn .setBounds (r.removeFromBottom (60));
        r.removeFromBottom (4);
        dbReadout = r.removeFromBottom (24);
        r.removeFromBottom (6);

        // par de medidores colado ao fader, com a ESCALA EM dBFS entre eles.
        //
        // Sem numeros, as barras diziam "mais" e "menos" e nada mais: nao dava
        // para ver que o alvo do auto fader e -18, nem conferir se o trim pos
        // a entrada onde devia. A escala fica no meio porque serve as duas -
        // IN de um lado, OUT do outro, e o mesmo numero vale para as duas.
        // MEDIDAS EM PIXEL INTEIRO.
        //
        // Eram 16 px por medidor: o corpo ficava com 14 e as duas colunas com
        // 6,5 cada. Meio pixel nao existe na tela - o desenho arredonda cada
        // coluna para um lado e elas saem com espessura e brilho diferentes,
        // como se uma estivesse medindo outra coisa.
        //
        // 17 px dao corpo de 15: 7 + 1 de folga + 7. Inteiro e simetrico, e as
        // duas colunas de cada medidor passam a ser identicas.
        // O BLOCO DE MEDIDORES SE ADAPTA A LARGURA DA TIRA.
        //
        // Eram 56 px fixos. Com a quantidade de faders configuravel, uma tira
        // de 16 por layer numa tela de 1920 fica com uns 90 px - e 56 fixos
        // esmagavam o fader, que e justamente o que a mao usa. Em tira estreita
        // sai primeiro a regua numerica do meio (a do fader continua, e diz a
        // mesma coisa); se ainda faltar espaco, as barras afinam.
        const int larguraTira = getWidth();
        if (larguraTira >= 120)
        {
            auto meters = r.removeFromRight (56);
            meterInArea  = meters.removeFromLeft (17);
            meters.removeFromLeft (1);
            escalaArea   = meters.removeFromLeft (20);
            meters.removeFromLeft (1);
            meterOutArea = meters.removeFromLeft (17);
        }
        else if (larguraTira >= 90)
        {
            auto meters = r.removeFromRight (36);
            meterInArea  = meters.removeFromLeft (17);
            meters.removeFromLeft (2);
            escalaArea   = {};
            meterOutArea = meters.removeFromLeft (17);
        }
        else
        {
            auto meters = r.removeFromRight (24);
            meterInArea  = meters.removeFromLeft (11);
            meters.removeFromLeft (2);
            escalaArea   = {};
            meterOutArea = meters.removeFromLeft (11);
        }
        r.removeFromRight (4);

        fader.setBounds (r);
    }

private:
    /** Medidor vertical na escala de OPERACAO: verde ate 0, ambar ate +10,
        vermelho acima. Le em dBFS e converte — o operador nunca ve dBFS. */
    /** Escala ao lado das barras, na MESMA lingua do fader.

        Os numeros sao os da escala de operacao - os mesmos +10, 0, -10 que
        estao serigrafados ao lado do fader. Antes as barras vinham sem regua
        nenhuma e, quando ganharam uma, ela falava dBFS: duas reguas com zeros
        diferentes na mesma tela, e o operador traduzindo de cabeca.

        A equivalencia e 0 de operacao = -18 dBFS (EBU R68). Os campos de
        ajuste - alvo do auto fader, limiar, trigger - continuam em dBFS,
        porque e a unidade em que se pensa nivel absoluto; a regua e para o
        olho, e o olho procura o zero. */
    void desenhaEscalaDbfs (juce::Graphics& g, juce::Rectangle<int> area)
    {
        // a regua acompanha a mesma margem das barras e do fader
        auto r = area.toFloat().withTrimmedTop (theme::kFaderMargem)
                               .withTrimmedBottom (theme::kFaderMargem);

        // As marcas sao as MESMAS da escala de operacao do fader, so que
        // escritas em dBFS. Duas reguas com zeros diferentes lado a lado
        // obrigam o operador a traduzir de cabeca, e foi assim que a tela
        // ficou: o fader marcando 0 e a barra marcando -20 na mesma altura.
        // Agora o 0 de operacao e o -18 dBFS sao a mesma linha.
        static const float marcas[] = { 0.0f, -8.0f, -18.0f, -28.0f, -38.0f, -48.0f, -58.0f };

        g.setFont (theme::mono (8.0f));
        for (float dbfs : marcas)
        {
            const float vu = theme::dbfsToVu (dbfs);
            if (vu > theme::kMeterTopVu || vu < theme::kMeterBotVu) continue;

            const float y = theme::vuToY (vu, r);
            const bool  refOperacao = std::abs (dbfs - theme::kMeterRefDb) < 0.01f;

            g.setColour (juce::Colours::white.withAlpha (refOperacao ? 0.30f : 0.12f));
            g.fillRect (r.getX(), y - 0.5f, r.getWidth(), 1.0f);

            g.setColour (refOperacao ? theme::oled : theme::oledDim);
            g.drawText (juce::String (int (vu)),
                        juce::Rectangle<float> (r.getX(), y - 5.0f, r.getWidth(), 10.0f),
                        juce::Justification::centred, false);
        }
    }

    /** Medidor vertical ESTEREO: duas colunas coladas, esquerda e direita.
        Em canal mono os dois valores sao iguais e as colunas andam juntas. */
    void drawVerticalMeter (juce::Graphics& g, juce::Rectangle<int> area,
                            float dbfsL, float dbfsR, bool comThreshold, bool comAlvoAuto,
                            float marcaLimiarDb = 1.0f)
    {
        auto r = area.toFloat();
        g.setColour (juce::Colour (0xff0a1014));
        g.fillRect (r);

        // MESMA MARGEM DO FADER, para as alturas coincidirem.
        //
        // A barra ocupava a coluna toda enquanto o fader reservava espaco para
        // o cap caber nas pontas. O 0 da regua da esquerda e o 0 da barra da
        // direita ficavam em linhas diferentes, e nao dava para comparar os
        // dois de relance - que e exatamente para o que servem lado a lado.
        auto corpo = r.reduced (1.0f, 0.0f)
                      .withTrimmedTop (theme::kFaderMargem)
                      .withTrimmedBottom (theme::kFaderMargem);

        // Duas colunas, com um fio de folga no meio.
        //
        // Barra unica escondia o que mais importa em estereo: um lado mudo.
        // Playout com um canal caido ou cabo solto numa entrada de linha
        // passavam despercebidos porque a mistura ainda mostrava sinal.
        // larguras arredondadas para inteiro, e a folga do meio tirada das
        // duas na mesma medida - senao uma coluna fica mais larga que a outra
        const float largura = std::floor ((corpo.getWidth() - 1.0f) * 0.5f);
        auto colL = corpo.withWidth (largura);
        auto colR = corpo.withX (corpo.getRight() - largura).withWidth (largura);

        // Cor por SEGMENTO, nao pela barra inteira: verde ate 0, ambar de 0 a
        // +10, vermelho acima. Pintar tudo da cor do pico esconderia onde o
        // sinal esta — o operador leria "esta vermelho" em vez de "passou de
        // 0 ha pouco".
        auto pintaColuna = [&] (juce::Rectangle<float> col, float dbfs)
        {
            const float vu = theme::dbfsToVu (dbfs);
            auto faixa = [&] (float de, float ate, juce::Colour c)
            {
                const float topo = juce::jmin (vu, ate);
                if (topo <= de) return;
                const float y1 = theme::vuToY (de, corpo);
                const float y2 = theme::vuToY (topo, corpo);
                g.setColour (c);
                g.fillRect (col.getX(), y2, col.getWidth(), y1 - y2);
            };
            faixa (theme::kMeterBotVu, 0.0f,  theme::meterVerde);
            faixa (0.0f, 10.0f,               theme::meterAmbar);
            faixa (10.0f, theme::kMeterTopVu, theme::meterVermelho);
        };
        pintaColuna (colL, dbfsL);
        pintaColuna (colR, dbfsR);

        // serigrafia de segmentos, como painel de LED
        g.setColour (juce::Colour (0xff0a1014));
        for (float y = corpo.getY(); y < corpo.getBottom(); y += 4.0f)
            g.fillRect (corpo.getX(), y, corpo.getWidth(), 1.0f);

        // marca do 0
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        const float y0 = theme::vuToY (0.0f, corpo);
        g.fillRect (corpo.getX(), y0 - 0.5f, corpo.getWidth(), 1.0f);

        if (comThreshold)
        {
            g.setColour (theme::trig);
            const float y = theme::vuToY (theme::dbfsToVu (ch.params.trigger.thresholdDb.load()), corpo);
            g.fillRect (r.getX() - 1.0f, y - 1.0f, r.getWidth() + 2.0f, 2.0f);
        }
        if (comAlvoAuto)
        {
            g.setColour (theme::wide);
            const float y = theme::vuToY (theme::dbfsToVu (ch.params.autoMix.targetDb.load()), corpo);
            g.fillRect (r.getX() - 1.0f, y - 1.0f, r.getWidth() + 2.0f, 2.0f);
        }
        if (marcaLimiarDb < 0.5f)
        {
            // tracejado, para nao se confundir com o alvo, que e linha cheia
            g.setColour (theme::wide.withAlpha (0.75f));
            const float y = theme::vuToY (theme::dbfsToVu (marcaLimiarDb), corpo);
            for (float x = r.getX(); x < r.getRight(); x += 4.0f)
                g.fillRect (x, y - 1.0f, 2.0f, 2.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRect (r, 1.0f);
    }

    void drawMeterRow (juce::Graphics& g, juce::Rectangle<int> row,
                       const char* label, float db, bool withThreshold,
                       bool withAutoTarget = false)
    {
        g.setColour (theme::oledDim);
        g.setFont (theme::mono (9.0f, true));
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


    mesa::Channel& ch;
    int index;
    SurfaceButton soft, autoBtn, preview, recBtn, bigOn, bigOff;
    std::unique_ptr<SurfaceButton> bus[4];
    FaderComponent fader;
    juce::Rectangle<int> oledArea, meterInArea, meterOutArea, escalaArea, dbReadout, trigRow;
    mesa::TriggerState trigState = mesa::TriggerState::Idle;
    bool onAir = false;
    float lastFaderDb = -1000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
