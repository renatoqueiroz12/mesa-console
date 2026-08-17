#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Theme.h"
#include "ConfigWindow.h"          // reaproveita CfgPage e CfgScroller
#include "../Core/Channel.h"
#include "../Core/DspRack.h"
#include "../Core/MixerEngine.h"
#include "../Core/AutomationEngine.h"
#include "../Core/Settings.h"
#include "../Core/SourceCatalog.h"

/** Menu do canal, aberto pela tecla SOFT. Sete secoes, como no mockup:
    fonte, ganho, buses, rack de DSP, trigger, logica e validacao.
    Escreve direto nos atomics do canal — o audio ve na hora, sem parar. */
class ChannelMenu : public juce::Component, private juce::Timer
{
public:
    ChannelMenu (mesa::MixerEngine& m, mesa::AutomationEngine& a,
                 mesa::Settings& s, int channelIndex)
        : mix (m), autom (a), settings (s), index (channelIndex),
          ch (m.channel (channelIndex)), tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        addAndMakeVisible (tabs);
        tabs.setOutline (0);
        tabs.setColour (juce::TabbedComponent::backgroundColourId, theme::surface);

        addTab ("Fonte",     buildSource());
        addTab ("Ganho",     buildGain());
        addTab ("Buses",     buildBuses());
        addTab ("DSP",       buildDsp());
        addTab ("Trigger",   buildTrigger());
        addTab ("Logica",    buildLogic());
        addTab ("Validacao", buildDiagnostics());

        header.setFont (theme::mono (13.0f, true));
        header.setColour (juce::Label::textColourId, theme::oled);
        addAndMakeVisible (header);

        setSize (720, 540);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::surface); }

    void resized() override
    {
        auto r = getLocalBounds();
        header.setBounds (r.removeFromTop (26).reduced (14, 2));
        tabs.setBounds (r);
    }

private:
    void addTab (const juce::String& name, CfgPage* page)
    {
        tabs.addTab (name, theme::surfaceLo, new CfgScroller (page), true);
    }

    void timerCallback() override
    {
        header.setText ("CANAL " + juce::String (index + 1) + "   \x7c   "
                        + juce::String (ch.name)
                        + "   \x7c   IN " + juce::String (ch.tapDb (mesa::TapPoint::Input), 1)
                        + " dBFS   \x7c   OUT "
                        + juce::String (ch.tapDb (mesa::TapPoint::PostFader), 1)
                        + " dBFS   \x7c   fader "
                        + juce::String (ch.params.faderDb.load(), 1) + " dB"
                        + (ch.params.autoMix.enabled.load() ? "  (AUTO)" : ""),
                        juce::dontSendNotification);

        if (diagLabel != nullptr)
            diagLabel->setText (diagnosticsText(), juce::dontSendNotification);
        if (calLabel != nullptr)
            calLabel->setText (calibratorText(), juce::dontSendNotification);
    }

    // ------------------------------------------------------------- helpers
    juce::Slider* slider (CfgPage& p, const juce::String& label, float current,
                          double lo, double hi, double step,
                          std::function<void (float)> onChange)
    {
        auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s->setRange (lo, hi, step);
        s->setValue (current, juce::dontSendNotification);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
        s->onValueChange = [s, onChange] { onChange (float (s->getValue())); };
        return p.addRow (label, s);
    }

    juce::ToggleButton* toggle (CfgPage& p, const juce::String& label, bool current,
                                std::function<void (bool)> onChange)
    {
        auto* t = new juce::ToggleButton();
        t->setToggleState (current, juce::dontSendNotification);
        t->onClick = [t, onChange] { onChange (t->getToggleState()); };
        return p.addRow (label, t);
    }

    // ------------------------------------------------------------- secoes
    CfgPage* buildSource()
    {
        auto* p = new CfgPage();
        p->addTitle ("Fonte carregada no canal");
        p->addNote ("A logica segue a FONTE: tipo, camera e threshold viajam com ela. "
                    "Trocar a fonte muda o comportamento automatico do canal.");

        // carregar do catalogo: e assim que uma fonte de rede entra no fader
        auto* load = new juce::ComboBox();
        load->addItem ("(escolher canal)", 1);
        int id = 2;
        for (const auto& src : settings.catalog.sources) load->addItem (src.name, id++);
        load->onChange = [this, load]
        {
            const int i = load->getSelectedId() - 2;
            if (i < 0 || i >= int (settings.catalog.sources.size())) return;
            mesa::loadSource (settings.catalog.sources[size_t (i)], ch);
        };
        p->addRow ("Carregar canal", load);

        auto* saveDef = new juce::TextButton ("SALVAR AJUSTES NO CANAL");
        saveDef->onClick = [this]
        {
            if (auto* def = settings.catalog.find (ch.name))
                mesa::captureSource (ch, *def);
        };
        p->addRow ("", saveDef, 28);
        p->addNote ("Calibrou o threshold ouvindo? Salve no canal — o ajuste passa a valer "
                    "em qualquer fader que carregar esse canal depois.");

        auto* nameBox = new juce::TextEditor();
        nameBox->setText (ch.name, juce::dontSendNotification);
        nameBox->setFont (theme::mono (12.0f));
        nameBox->onTextChange = [this, nameBox] { ch.name = nameBox->getText().toStdString(); };
        p->addRow ("Nome", nameBox);

        static const char* types[] = { "Operador", "Produtor", "Convidado CR", "Convidado Estudio",
                                       "Mic Externo", "Linha", "Telefone", "Codec",
                                       "Player de PC", "Feed de Estudio" };
        auto* typeBox = new juce::ComboBox();
        for (int t = 0; t < 10; ++t) typeBox->addItem (types[t], t + 1);
        typeBox->setSelectedId (ch.params.sourceType.load() + 1, juce::dontSendNotification);
        typeBox->onChange = [this, typeBox] { ch.params.sourceType.store (typeBox->getSelectedId() - 1); };
        p->addRow ("Tipo", typeBox);

        auto* kindBox = new juce::ComboBox();
        kindBox->addItem ("Placa (ASIO)", 1);
        kindBox->addItem ("Rede (NDI / AES67)", 2);
        kindBox->setSelectedId (ch.params.inputKind.load() + 1, juce::dontSendNotification);
        kindBox->onChange = [this, kindBox] { ch.params.inputKind.store (kindBox->getSelectedId() - 1); };
        p->addRow ("Origem", kindBox);

        auto* inBox = new juce::ComboBox();
        inBox->addItem ("sem fonte", 1);
        for (int k = 0; k < 16; ++k) inBox->addItem ("entrada " + juce::String (k + 1), k + 2);
        inBox->setSelectedId (ch.params.inputIndex.load() + 2, juce::dontSendNotification);
        inBox->onChange = [this, inBox] { ch.params.inputIndex.store (inBox->getSelectedId() - 2); };
        p->addRow ("Entrada", inBox);

        p->addTitle ("Mix-minus");
        p->addNote ("Telefone, codec e feed de estudio recebem backfeed proprio: "
                    "o programa menos a propria voz. Sem isso o interlocutor se ouve.");

        auto* feedBox = new juce::ComboBox();
        feedBox->addItem ("automatico", 1);
        for (int b = 1; b <= 4; ++b) feedBox->addItem ("PGM " + juce::String (b), b + 1);
        feedBox->setSelectedId (ch.params.feedSource.load() + 1, juce::dontSendNotification);
        feedBox->onChange = [this, feedBox] { ch.params.feedSource.store (feedBox->getSelectedId() - 1); };
        p->addRow ("Fonte do backfeed", feedBox);

        auto* outBox = new juce::ComboBox();
        outBox->addItem ("nao roteado", 1);
        for (int k = 0; k < 4; ++k)
            outBox->addItem (juce::String (k * 2 + 1) + "/" + juce::String (k * 2 + 2), k + 2);
        outBox->setSelectedId (ch.params.feedOutputPair.load() + 2, juce::dontSendNotification);
        outBox->onChange = [this, outBox] { ch.params.feedOutputPair.store (outBox->getSelectedId() - 2); };
        p->addRow ("Saida do backfeed", outBox);

        slider (*p, "DIM no talkback", ch.params.feedDimDb.load(), -30.0, 0.0, 0.5,
                [this] (float v) { ch.params.feedDimDb.store (v); });
        return p;
    }

    CfgPage* buildGain()
    {
        auto* p = new CfgPage();
        p->addTitle ("Trim e fader");
        slider (*p, "Trim manual (dB)", ch.params.trimDb.load(), -25.0, 25.0, 0.1,
                [this] (float v) { ch.params.trimDb.store (v); });
        slider (*p, "Fader (dB)", ch.params.faderDb.load(), -60.0, 10.0, 0.1,
                [this] (float v) { ch.params.faderDb.store (v); });
        slider (*p, "Pan", ch.params.panPos.load(), -1.0, 1.0, 0.01,
                [this] (float v) { ch.params.panPos.store (v); });
        toggle (*p, "Mute", ch.params.mute.load(),
                [this] (bool v) { ch.params.mute.store (v); });

        p->addTitle ("Automix (nivelador de fader)");
        p->addNote ("Move o FADER para manter o canal no alvo, como um operador "
                    "acompanhando. Mede antes do fader, para nao realimentar. "
                    "Encostar no fader desliga: quem manda e quem esta na sala.");
        auto& am = ch.params.autoMix;
        auto& atx = ch.params.autoTrim;

        // os dois perseguem nivel; ligados juntos, brigam
        toggle (*p, "Ligado", am.enabled.load(), [&am, &atx] (bool v)
                { am.enabled.store (v); if (v) atx.enabled.store (false); });

        slider (*p, "Alvo (dBFS RMS)", am.targetDb.load(), -40.0, -6.0, 0.5,
                [&am] (float v) { am.targetDb.store (v); });
        p->addNote ("O alvo e RMS; o medidor da tira mostra PICO. Em fala o pico fica "
                    "uns 12 dB acima do RMS: alvo -18 faz a barra bater perto de -6. "
                    "A marca azul na barra OUT mostra onde ele esta mirando.");
        slider (*p, "Fader maximo (dB)", am.maxFaderDb.load(), -10.0, 10.0, 0.5,
                [&am] (float v) { am.maxFaderDb.store (v); });
        slider (*p, "Fader minimo (dB)", am.minFaderDb.load(), -60.0, 0.0, 0.5,
                [&am] (float v) { am.minFaderDb.store (v); });
        slider (*p, "So atua acima de (dBFS)", am.floorDb.load(), -70.0, -20.0, 0.5,
                [&am] (float v) { am.floorDb.store (v); });
        p->addNote ("Abaixo deste nivel ele congela. E o que impede o fader de subir "
                    "atras do ruido de sala na pausa. Suba ate parar de andar em silencio.");
        slider (*p, "Velocidade (dB/s)", am.speedDbPerSec.load(), 0.5, 20.0, 0.5,
                [&am] (float v) { am.speedDbPerSec.store (v); });
        p->addNote ("Velocidade alta reage rapido e bombeia; baixa e discreta e demora "
                    "a acompanhar quem fala muito baixo. 4 a 8 dB/s costuma ser o ponto.");

        p->addTitle ("Auto trim");
        p->addNote ("Corrige o nivel medio da fonte antes do DSP. Nao substitui o trim "
                    "manual: trabalha por cima dele, devagar, e para no silencio.");
        auto& at = ch.params.autoTrim;
        toggle (*p, "Ligado", at.enabled.load(), [&am, &at] (bool v)
                { at.enabled.store (v); if (v) am.enabled.store (false); });
        slider (*p, "Alvo RMS (dBFS)", at.targetDb.load(), -40.0, -6.0, 0.5,
                [&at] (float v) { at.targetDb.store (v); });
        slider (*p, "Ganho maximo (dB)", at.maxGainDb.load(), 0.0, 30.0, 0.5,
                [&at] (float v) { at.maxGainDb.store (v); });
        slider (*p, "Ganho minimo (dB)", at.minGainDb.load(), -30.0, 0.0, 0.5,
                [&at] (float v) { at.minGainDb.store (v); });
        slider (*p, "Piso de atuacao (dBFS)", at.gateDb.load(), -70.0, -20.0, 0.5,
                [&at] (float v) { at.gateDb.store (v); });
        slider (*p, "Velocidade (dB/s)", at.speedDbPerSec.load(), 0.5, 12.0, 0.5,
                [&at] (float v) { at.speedDbPerSec.store (v); });
        return p;
    }

    CfgPage* buildBuses()
    {
        auto* p = new CfgPage();
        p->addTitle ("Envio para os buses");
        for (int b = 0; b < mesa::kNumBuses; ++b)
            toggle (*p, "PGM " + juce::String (b + 1), (ch.params.busMask.load() >> b) & 1u,
                    [this, b] (bool v)
                    {
                        const unsigned mask = ch.params.busMask.load();
                        ch.params.busMask.store (v ? (mask | (1u << b)) : (mask & ~(1u << b)));
                    });

        p->addTitle ("CUE");
        toggle (*p, "CUE (PFL)", ch.params.cue.load(),
                [this] (bool v) { ch.params.cue.store (v); });
        p->addNote ("O CUE e PFL de verdade: escuta antes do fader e independe do ON/OFF. "
                    "Da para conferir a fonte com o canal fechado, sem ir ao ar.");
        return p;
    }

    CfgPage* buildDsp()
    {
        auto* p = new CfgPage();
        auto& rack = ch.rack;

        p->addTitle ("Cadeia");
        p->addNote ("Ordem de fabrica: gate, EQ, compressor, de-esser, limiter. "
                    "A troca de ordem e publicada por store atomico — o audio nunca "
                    "ve uma lista pela metade.");

        p->addTitle ("Gate");
        toggle (*p, "Ligado", rack.isEnabled (mesa::DspType::Gate),
                [&rack] (bool v) { rack.setEnabled (mesa::DspType::Gate, v); });
        auto& gate = rack.noiseGate();
        slider (*p, "Threshold (dBFS)", gate.thresholdDb.load(), -80.0, 0.0, 0.5,
                [&gate] (float v) { gate.thresholdDb.store (v); });
        slider (*p, "Range (dB)", gate.rangeDb.load(), -80.0, 0.0, 1.0,
                [&gate] (float v) { gate.rangeDb.store (v); });
        slider (*p, "Attack (ms)", gate.attackMs.load(), 0.1, 50.0, 0.1,
                [&gate] (float v) { gate.attackMs.store (v); });
        slider (*p, "Hold (ms)", gate.holdMs.load(), 0.0, 1000.0, 5.0,
                [&gate] (float v) { gate.holdMs.store (v); });
        slider (*p, "Release (ms)", gate.releaseMs.load(), 5.0, 2000.0, 5.0,
                [&gate] (float v) { gate.releaseMs.store (v); });

        p->addTitle ("EQ 3 bandas");
        toggle (*p, "Ligado", rack.isEnabled (mesa::DspType::Eq),
                [&rack] (bool v) { rack.setEnabled (mesa::DspType::Eq, v); });
        auto& eq = rack.equaliser();
        slider (*p, "Grave freq (Hz)", eq.lowFreq.load(), 30.0, 500.0, 1.0,
                [&eq] (float v) { eq.lowFreq.store (v); });
        slider (*p, "Grave ganho (dB)", eq.lowGainDb.load(), -18.0, 18.0, 0.5,
                [&eq] (float v) { eq.lowGainDb.store (v); });
        slider (*p, "Medio freq (Hz)", eq.midFreq.load(), 200.0, 6000.0, 10.0,
                [&eq] (float v) { eq.midFreq.store (v); });
        slider (*p, "Medio ganho (dB)", eq.midGainDb.load(), -18.0, 18.0, 0.5,
                [&eq] (float v) { eq.midGainDb.store (v); });
        slider (*p, "Medio Q", eq.midQ.load(), 0.2, 8.0, 0.1,
                [&eq] (float v) { eq.midQ.store (v); });
        slider (*p, "Agudo freq (Hz)", eq.highFreq.load(), 2000.0, 16000.0, 50.0,
                [&eq] (float v) { eq.highFreq.store (v); });
        slider (*p, "Agudo ganho (dB)", eq.highGainDb.load(), -18.0, 18.0, 0.5,
                [&eq] (float v) { eq.highGainDb.store (v); });

        p->addTitle ("Compressor");
        toggle (*p, "Ligado", rack.isEnabled (mesa::DspType::Compressor),
                [&rack] (bool v) { rack.setEnabled (mesa::DspType::Compressor, v); });
        auto& comp = rack.compressor();
        slider (*p, "Threshold (dBFS)", comp.thresholdDb.load(), -60.0, 0.0, 0.5,
                [&comp] (float v) { comp.thresholdDb.store (v); });
        slider (*p, "Ratio", comp.ratio.load(), 1.0, 20.0, 0.1,
                [&comp] (float v) { comp.ratio.store (v); });
        slider (*p, "Attack (ms)", comp.attackMs.load(), 0.1, 100.0, 0.1,
                [&comp] (float v) { comp.attackMs.store (v); });
        slider (*p, "Release (ms)", comp.releaseMs.load(), 10.0, 1000.0, 5.0,
                [&comp] (float v) { comp.releaseMs.store (v); });
        slider (*p, "Makeup (dB)", comp.makeupDb.load(), 0.0, 24.0, 0.5,
                [&comp] (float v) { comp.makeupDb.store (v); });

        p->addTitle ("De-esser");
        toggle (*p, "Ligado", rack.isEnabled (mesa::DspType::DeEsser),
                [&rack] (bool v) { rack.setEnabled (mesa::DspType::DeEsser, v); });
        auto& dees = rack.deEsser();
        slider (*p, "Frequencia (Hz)", dees.freq.load(), 2000.0, 12000.0, 50.0,
                [&dees] (float v) { dees.freq.store (v); });
        slider (*p, "Threshold (dBFS)", dees.thresholdDb.load(), -50.0, 0.0, 0.5,
                [&dees] (float v) { dees.thresholdDb.store (v); });
        slider (*p, "Ratio", dees.ratio.load(), 1.0, 12.0, 0.1,
                [&dees] (float v) { dees.ratio.store (v); });

        p->addTitle ("Limiter");
        toggle (*p, "Ligado", rack.isEnabled (mesa::DspType::Limiter),
                [&rack] (bool v) { rack.setEnabled (mesa::DspType::Limiter, v); });
        auto& lim = rack.limiter();
        slider (*p, "Teto (dBFS)", lim.ceilingDb.load(), -12.0, 0.0, 0.1,
                [&lim] (float v) { lim.ceilingDb.store (v); });
        slider (*p, "Release (ms)", lim.releaseMs.load(), 5.0, 500.0, 5.0,
                [&lim] (float v) { lim.releaseMs.store (v); });
        return p;
    }

    CfgPage* buildTrigger()
    {
        auto* p = new CfgPage();
        auto& tr = ch.params.trigger;

        p->addTitle ("Audio Trigger");
        toggle (*p, "Ligado", tr.enabled.load(), [&tr] (bool v) { tr.enabled.store (v); });

        auto* tapBox = new juce::ComboBox();
        tapBox->addItem ("Entrada", 1);
        tapBox->addItem ("Pos auto trim", 2);
        tapBox->addItem ("Pos DSP", 3);
        tapBox->addItem ("Pos fader", 4);
        tapBox->setSelectedId (tr.source.load() + 1, juce::dontSendNotification);
        tapBox->onChange = [&tr, tapBox] { tr.source.store (tapBox->getSelectedId() - 1); };
        p->addRow ("Ponto de deteccao", tapBox);

        auto* camBox = new juce::ComboBox();
        camBox->addItem ("sem camera", 1);
        for (int k = 1; k <= 8; ++k) camBox->addItem ("CAM " + juce::String (k), k + 1);
        camBox->setSelectedId (tr.camera.load() + 1, juce::dontSendNotification);
        camBox->onChange = [&tr, camBox] { tr.camera.store (camBox->getSelectedId() - 1); };
        p->addRow ("Camera", camBox);

        slider (*p, "Threshold (dBFS)", tr.thresholdDb.load(), -70.0, -5.0, 0.5,
                [&tr] (float v) { tr.thresholdDb.store (v); });
        slider (*p, "Permanencia (ms)", tr.triggerMs.load(), 20.0, 3000.0, 10.0,
                [&tr] (float v) { tr.triggerMs.store (v); });
        slider (*p, "Histerese (dB)", tr.hysteresisDb.load(), 0.0, 20.0, 0.5,
                [&tr] (float v) { tr.hysteresisDb.store (v); });
        slider (*p, "Hold (ms)", tr.holdMs.load(), 100.0, 30000.0, 100.0,
                [&tr] (float v) { tr.holdMs.store (v); });
        slider (*p, "Release (ms)", tr.releaseMs.load(), 50.0, 15000.0, 50.0,
                [&tr] (float v) { tr.releaseMs.store (v); });
        slider (*p, "Cooldown (ms)", tr.cooldownMs.load(), 0.0, 30000.0, 100.0,
                [&tr] (float v) { tr.cooldownMs.store (v); });

        p->addNote ("O trigger le o envelope rapido, nao o medidor de pico. Permanencia "
                    "separa fala de estalo de papel; HOLD e quanto a camera fica depois "
                    "que a pessoa para, e e ele que evita corte na respirada entre frases. "
                    "1000 ms sao 1 segundo: para fala corrida, hold de 2000 a 5000 costuma "
                    "ficar natural.");

        p->addTitle ("Comando");
        auto* cmdBox = new juce::TextEditor();
        cmdBox->setText (tr.command.str(), juce::dontSendNotification);
        cmdBox->setFont (theme::mono (12.0f));
        cmdBox->onTextChange = [&tr, cmdBox] { tr.command.set (cmdBox->getText().toStdString()); };
        p->addRow ("Comando literal", cmdBox);
        p->addNote ("Vazio: a mesa monta um CUT pela camera. Preenchido: manda exatamente "
                    "este texto — serve para overlay, transicao ou outro sistema.");

        auto* targetBox = new juce::ComboBox();
        for (size_t i = 0; i < settings.targets.size(); ++i)
            targetBox->addItem (settings.targets[i].name, int (i) + 1);
        targetBox->setSelectedId (tr.target.load() + 1, juce::dontSendNotification);
        targetBox->onChange = [&tr, targetBox] { tr.target.store (targetBox->getSelectedId() - 1); };
        p->addRow ("Destino", targetBox);

        auto* fire = new juce::TextButton ("DISPARAR TESTE");
        fire->onClick = [this] { autom.testFire (mix, index); };
        p->addRow ("Teste", fire, 30);
        return p;
    }

    CfgPage* buildLogic()
    {
        auto* p = new CfgPage();
        p->addTitle ("Logica do canal (fader-start)");
        p->addNote ("Comandos disparados na BORDA de ON/OFF. E assim que o fader liga "
                    "e para a cartucheira, o playout ou a luz de ar.");

        toggle (*p, "Habilitada", ch.params.logicEnabled.load(),
                [this] (bool v) { ch.params.logicEnabled.store (v); });

        auto* onBox = new juce::TextEditor();
        onBox->setText (ch.params.onCommand.str(), juce::dontSendNotification);
        onBox->setFont (theme::mono (12.0f));
        onBox->onTextChange = [this, onBox] { ch.params.onCommand.set (onBox->getText().toStdString()); };
        p->addRow ("Comando de ON", onBox);

        auto* offBox = new juce::TextEditor();
        offBox->setText (ch.params.offCommand.str(), juce::dontSendNotification);
        offBox->setFont (theme::mono (12.0f));
        offBox->onTextChange = [this, offBox] { ch.params.offCommand.set (offBox->getText().toStdString()); };
        p->addRow ("Comando de OFF", offBox);

        auto* targetBox = new juce::ComboBox();
        for (size_t i = 0; i < settings.targets.size(); ++i)
            targetBox->addItem (settings.targets[i].name, int (i) + 1);
        targetBox->setSelectedId (ch.params.logicTarget.load() + 1, juce::dontSendNotification);
        targetBox->onChange = [this, targetBox] { ch.params.logicTarget.store (targetBox->getSelectedId() - 1); };
        p->addRow ("Destino", targetBox);

        auto* play = new juce::TextButton ("REENVIAR ON");
        play->onClick = [this] { autom.fireChannelLogic (mix, index, true); };
        p->addRow ("Teste", play, 30);

        auto* pause = new juce::TextButton ("REENVIAR OFF");
        pause->onClick = [this] { autom.fireChannelLogic (mix, index, false); };
        p->addRow ("", pause, 30);
        return p;
    }

    CfgPage* buildDiagnostics()
    {
        auto* p = new CfgPage();
        p->addTitle ("Presenca de sinal");
        diagLabel = new juce::Label ({}, diagnosticsText());
        diagLabel->setFont (theme::mono (11.0f));
        diagLabel->setColour (juce::Label::textColourId, theme::oled);
        diagLabel->setJustificationType (juce::Justification::topLeft);
        p->addWide (diagLabel, 90);

        p->addTitle ("Calibrador de threshold");
        p->addNote ("Deixe o canal aberto, peca silencio por alguns segundos e depois "
                    "fala normal. O calibrador mede os dois e sugere o meio.");
        calLabel = new juce::Label ({}, calibratorText());
        calLabel->setFont (theme::mono (11.0f));
        calLabel->setColour (juce::Label::textColourId, theme::oled);
        calLabel->setJustificationType (juce::Justification::topLeft);
        p->addWide (calLabel, 90);

        auto* start = new juce::TextButton ("INICIAR MEDICAO");
        start->onClick = [this] { ch.calibrator.start(); };
        p->addRow ("Calibrar", start, 30);

        auto* apply = new juce::TextButton ("APLICAR SUGESTAO");
        apply->onClick = [this]
        {
            if (ch.calibrator.ready())
                ch.params.trigger.thresholdDb.store (ch.calibrator.suggestedThresholdDb());
        };
        p->addRow ("", apply, 30);
        return p;
    }

    juce::String diagnosticsText() const
    {
        return juce::String ("sinal presente: ") + (ch.presence.hasSignal() ? "sim" : "nao")
             + "\nja teve sinal: " + (ch.presence.everHadSignal() ? "sim" : "nao")
             + "\nsilencio ha: " + juce::String (ch.presence.silenceMs() / 1000.0f, 1) + " s"
             + "\npico retido: " + juce::String (ch.presence.peakHoldDb(), 1) + " dBFS"
             + "\nclips: " + juce::String (ch.presence.clips())
             + (ch.presence.isSilentAlarm() ? "\nALARME: canal emudeceu" : "");
    }

    juce::String calibratorText() const
    {
        if (! ch.calibrator.isRunning() && ch.calibrator.elapsedMs() <= 0.0f)
            return "parado";
        return juce::String ("medindo ha ") + juce::String (ch.calibrator.elapsedMs() / 1000.0f, 1) + " s"
             + "\npiso de ruido: " + juce::String (ch.calibrator.noiseFloorDb(), 1) + " dBFS"
             + "\nnivel de fala: " + juce::String (ch.calibrator.speechLevelDb(), 1) + " dBFS"
             + (ch.calibrator.ready()
                    ? "\nsugestao: " + juce::String (ch.calibrator.suggestedThresholdDb(), 1) + " dBFS"
                    : "\nainda sem material suficiente");
    }

    mesa::MixerEngine& mix;
    mesa::AutomationEngine& autom;
    mesa::Settings& settings;
    int index;
    mesa::Channel& ch;
    juce::TabbedComponent tabs;
    juce::Label header;
    juce::Label* diagLabel = nullptr;
    juce::Label* calLabel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelMenu)
};
