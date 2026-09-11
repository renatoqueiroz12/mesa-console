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

        // Margem maior que a METADE do cap: no fim do curso ele precisa caber
        // inteiro dentro da caixa, senao some um pedaco.
        const float top = 34.0f, bottom = r.getHeight() - 34.0f;
        const float travel = bottom - top;

        // eixo do rasgo: a escala se apoia nele
        const float cx = r.getCentreX() + 11.0f;

        // escala numerada
        static const char* nums[] = { "+20", "+10", "0", "-10", "-20", "-30", "-40", "-\xe2\x88\x9e" };
        g.setFont (theme::mono (9.0f, true));
        g.setColour (theme::escalaTexto);
        for (int i = 0; i < 8; ++i)
        {
            const float y = top + travel * (float (i) / 7.0f);
            g.drawText (juce::String::fromUTF8 (nums[i]),
                        juce::Rectangle<float> (2.0f, y - 6.0f, 24.0f, 12.0f),
                        juce::Justification::centredLeft, false);
            // Traco encostado no rasgo, do lado do numero: e assim que se le
            // posicao de fader sem tirar o olho do cap.
            g.setColour (theme::escalaTraco);
            g.fillRect (cx - 21.0f, y - 0.7f, 12.0f, 1.4f);

            // dois tracos curtos entre um rotulo e o proximo, como regua
            if (i < 7)
                for (int k = 1; k <= 2; ++k)
                {
                    const float ym = y + (travel / 7.0f) * (float (k) / 3.0f);
                    g.setColour (theme::escalaTraco.withAlpha (0.5f));
                    g.fillRect (cx - 16.0f, ym - 0.5f, 7.0f, 1.0f);
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

        // ---- medidores VERTICAIS, ladeando o fader
        //
        // Antes eram duas barrinhas horizontais acima do fader, curtas demais
        // para julgar nivel. Na vertical acompanham o curso do fader e ficam no
        // campo de visao de quem opera, que e onde a mao ja esta.
        drawVerticalMeter (g, meterInArea,  ch.tapDb (mesa::TapPoint::Input), true, false);
        drawVerticalMeter (g, meterOutArea, ch.tapDb (mesa::TapPoint::PostFader), false,
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

        // par de medidores colado ao fader, ocupando a altura restante
        auto meters = r.removeFromRight (34);
        meterInArea  = meters.removeFromLeft (16);
        meters.removeFromLeft (2);
        meterOutArea = meters.removeFromLeft (16);
        r.removeFromRight (4);

        fader.setBounds (r);
    }

private:
    /** Medidor vertical na escala de OPERACAO: verde ate 0, ambar ate +10,
        vermelho acima. Le em dBFS e converte — o operador nunca ve dBFS. */
    void drawVerticalMeter (juce::Graphics& g, juce::Rectangle<int> area,
                            float dbfs, bool comThreshold, bool comAlvoAuto)
    {
        auto r = area.toFloat();
        g.setColour (juce::Colour (0xff0a1014));
        g.fillRect (r);

        auto corpo = r.reduced (1.0f, 1.0f);
        const float vu = theme::dbfsToVu (dbfs);

        // Cor por SEGMENTO, nao pela barra inteira: verde ate 0, ambar de 0 a
        // +10, vermelho acima. Pintar tudo da cor do pico esconderia onde o
        // sinal esta — o operador leria "esta vermelho" em vez de "passou de
        // 0 ha pouco".
        auto pinta = [&] (float de, float ate, juce::Colour c)
        {
            const float topo = juce::jmin (vu, ate);
            if (topo <= de) return;
            const float y1 = theme::vuToY (de, corpo);
            const float y2 = theme::vuToY (topo, corpo);
            g.setColour (c);
            g.fillRect (corpo.getX(), y2, corpo.getWidth(), y1 - y2);
        };
        pinta (theme::kMeterBotVu, 0.0f,  theme::meterVerde);
        pinta (0.0f, 10.0f,               theme::meterAmbar);
        pinta (10.0f, theme::kMeterTopVu, theme::meterVermelho);

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
    juce::Rectangle<int> oledArea, meterInArea, meterOutArea, dbReadout, trigRow;
    mesa::TriggerState trigState = mesa::TriggerState::Idle;
    bool onAir = false;
    float lastFaderDb = -1000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
