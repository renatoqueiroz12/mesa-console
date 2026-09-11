#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Theme.h"
#include "../Core/Settings.h"
#include "../Core/MixerEngine.h"
#include "../Core/SourceCatalog.h"
#include "../Core/Version.h"
#include <map>
#include <algorithm>
#include "../Core/AutomationEngine.h"
#include "NdiEngine.h"
#include "SecondaryDevices.h"
#include "LivewireReceiver.h"
#include "LivewireSender.h"
#include "LwrpClient.h"
#include "LivewireBrowser.h"
#include "LivewireScanner.h"
#include "VmixClient.h"
#include "../Core/Defaults.h"

/** Uma pagina de configuracao: linhas de rotulo + controle, empilhadas.
    Ela mesma cresce conforme as linhas, e o Viewport rola quando nao cabe. */
class CfgPage : public juce::Component
{
public:
    void addTitle (const juce::String& t)
    {
        auto* l = new juce::Label ({}, t);
        l->setFont (theme::mono (12.0f, true));
        l->setColour (juce::Label::textColourId, theme::oled);
        items.add ({ nullptr, l, 26 });
        addAndMakeVisible (l);
    }

    void addNote (const juce::String& t)
    {
        auto* l = new juce::Label ({}, t);
        l->setFont (theme::sans (11.0f));
        l->setColour (juce::Label::textColourId, theme::textDim);
        l->setJustificationType (juce::Justification::topLeft);
        items.add ({ nullptr, l, 34 });
        addAndMakeVisible (l);
    }

    /** Devolve o proprio controle para quem chamou ligar o callback. */
    template <typename T>
    T* addRow (const juce::String& label, T* control, int height = 26)
    {
        auto* l = new juce::Label ({}, label);
        l->setFont (theme::mono (11.0f));
        l->setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (l);
        semRoda (control);
        addAndMakeVisible (control);
        items.add ({ l, control, height });
        return control;
    }

    /** Tira a roda do mouse dos controles.

        A roda existe para ROLAR A PAGINA. Com ela ativa nos controles, passar
        o mouse por cima enquanto se rola a tela muda nivel, threshold e trim
        sem ninguem clicar em nada — e o operador so descobre depois, no ar.
        Ajuste de valor exige agarrar o controle, que e o gesto deliberado. */
    static void semRoda (juce::Component* c)
    {
        if (auto* sl = dynamic_cast<juce::Slider*> (c))   sl->setScrollWheelEnabled (false);
        if (auto* cb = dynamic_cast<juce::ComboBox*> (c)) cb->setScrollWheelEnabled (false);
    }

    void addWide (juce::Component* c, int height)
    {
        semRoda (c);
        addAndMakeVisible (c);
        items.add ({ nullptr, c, height });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        for (auto& it : items)
        {
            auto row = r.removeFromTop (it.height);
            r.removeFromTop (6);
            if (it.label != nullptr)
            {
                it.label->setBounds (row.removeFromLeft (190));
                it.control->setBounds (row.removeFromLeft (juce::jmin (row.getWidth(), 320)));
            }
            else
            {
                it.control->setBounds (row);
            }
        }
    }

    int preferredHeight() const
    {
        int h = 20;
        for (auto& it : items) h += it.height + 6;
        return h;
    }

private:
    struct Item { juce::Label* label; juce::Component* control; int height; };
    juce::Array<Item> items;
    juce::OwnedArray<juce::Component> owned;

public:
    /** Guarda o ponteiro para destruir junto com a pagina. */
    template <typename T> T* own (T* c) { owned.add (c); return c; }
};

/** Rola a pagina quando ela nao cabe na aba. */
class CfgScroller : public juce::Component
{
public:
    /** Onde a rolagem esta, para sobreviver a uma reconstrucao da aba. */
    int posicaoDaRolagem() const { return viewport.getViewPositionY(); }
    void vaiPara (int y) { viewport.setViewPosition (0, y); }

    explicit CfgScroller (CfgPage* p) : page (p)
    {
        viewport.setViewedComponent (page, true);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }
    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        page->setSize (getWidth() - 12, juce::jmax (getHeight(), page->preferredHeight()));
    }
private:
    juce::Viewport viewport;
    CfgPage* page;
};

/** Painel de configuracoes: as 9 abas do mockup, ligadas a mesa::Settings.
    O que e de instalacao mora aqui; o que e de show mora na cena. */
class ConfigComponent : public juce::Component,
                        private juce::Timer
{
public:
    ConfigComponent (mesa::Settings& s, mesa::MixerEngine& m,
                     mesa::AutomationEngine& a, juce::AudioDeviceManager& dm,
                     juce::File file, SecondaryDevices* sec = nullptr)
        : settings (s), mix (m), autom (a), deviceManager (dm), settingsFile (std::move (file)),
          secondaries (sec), tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        addAndMakeVisible (tabs);
        tabs.setOutline (0);
        tabs.setColour (juce::TabbedComponent::backgroundColourId, theme::surface);

        buildAllTabs();

        saveButton.setButtonText ("SALVAR E APLICAR");
        saveButton.onClick = [this] { save(); };
        addAndMakeVisible (saveButton);

        // Versao no rodape desta janela.
        //
        // Ela ja aparece na barra de status da mesa, mas essa barra some
        // quando a janela nao esta em tela cheia — e e justamente nas
        // configuracoes que a pergunta "que versao esta rodando?" aparece.
        versaoLabel.setText (juce::String ("v") + mesa::kVersion + "  ("
                             + mesa::kBuildName + ")", juce::dontSendNotification);
        versaoLabel.setFont (theme::mono (10.0f));
        versaoLabel.setColour (juce::Label::textColourId, theme::oledDim);
        addAndMakeVisible (versaoLabel);

        statusLabel.setFont (theme::mono (11.0f));
        statusLabel.setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (statusLabel);

        // enquanto o operador esta escolhendo fonte, a descoberta faz sentido
        NdiEngine::instance().startDiscovery();

        // Procura sozinha, como faz a superficie da Axia. Obrigar o operador a
        // apertar um botao para so entao poder escolher a fonte e trabalho que
        // a mesa sabe fazer — e que ele esquece de fazer.
        procuraLivewire();

        setSize (760, 560);
    }

    /** Avisa a superficie quando fecha, para religar as fontes de rede. */
    std::function<void()> onClosed;
    ~ConfigComponent() override
    {
        // A varredura precisa morrer ANTES da janela: thread viva mexendo em
        // objeto destruido e o defeito que derrubou a mesa.
        stopTimer();
        varredura.cancela();

        // NAO para a descoberta aqui.
        //
        // Parar e religar o NDI a cada abrir e fechar desta janela era o
        // gatilho das quedas: o log mostrou o crash no MESMO segundo de
        // "configuracoes fechadas", com a pilha dentro da DLL do NDI. Montar e
        // desmontar biblioteca de terceiro em plena operacao e risco sem
        // ganho — quem desliga tudo e o encerramento da mesa.
        if (onClosed) onClosed();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::surface); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (40).reduced (12, 6);
        saveButton.setBounds (bottom.removeFromRight (180));
        bottom.removeFromRight (10);
        versaoLabel.setBounds (bottom.removeFromLeft (200));
        bottom.removeFromLeft (8);
        statusLabel.setBounds (bottom);
        tabs.setBounds (r);
    }

private:
    // ------------------------------------------------------------------ helpers
    /** Destino de monitoracao: a lista INTEIRA, como na aba Outputs.

        Antes eram so os pares da placa mestra. Quem tem o monitor noutra placa
        ou manda fone por Livewire nao tinha como escolher — a mesa sabia
        fazer, o campo e que nao deixava. Explicar em nota que o caminho era
        outra aba foi remendo: campo que existe tem de oferecer o que existe.

        Mestra vai pelo caminho direto, sem fila, que e o de menor atraso.
        Secundaria e rede entram como OutputDef, com alguns milissegundos a
        mais — e o preco de sair da placa principal. */
    void destinoDeMonitoracao (CfgPage& p, const juce::String& rotulo, int bus,
                               int parAtual, std::function<void (int)> aoMudarPar)
    {
        const auto lista = availableOutputs (Transport::Asio);
        const auto lista2 = availableOutputs (Transport::Windows);

        auto* box = new juce::ComboBox();
        std::vector<Avail> tudo;
        tudo.push_back ({ "(nao roteado)", int (mesa::InputKind::Device), -1, {} });
        for (const auto& a : lista)  if (a.index >= 0) tudo.push_back (a);
        for (const auto& a : lista2) if (a.index >= 0) tudo.push_back (a);

        int sel = 1;
        for (size_t k = 0; k < tudo.size(); ++k)
        {
            box->addItem (tudo[k].label, int (k) + 1);
            const bool isSec = ! tudo[k].stream.empty() && tudo[k].stream[0] == '\x01';
            if (! isSec && tudo[k].index == parAtual && parAtual >= 0) sel = int (k) + 1;
        }

        // ja existe um OutputDef para este barramento? entao e ele o escolhido
        for (const auto& o : settings.outputs.outputs)
            if (o.busSource == bus && ! o.deviceName.empty())
                for (size_t k = 0; k < tudo.size(); ++k)
                    if (tudo[k].stream.find (o.deviceName) != std::string::npos)
                        sel = int (k) + 1;

        box->setSelectedId (sel, juce::dontSendNotification);

        box->onChange = [this, box, tudo, bus, aoMudarPar]
        {
            const int i = box->getSelectedId() - 1;
            if (i < 0 || i >= int (tudo.size())) return;
            const auto& a = tudo[size_t (i)];

            // limpa qualquer OutputDef anterior deste barramento
            auto& v = settings.outputs.outputs;
            v.erase (std::remove_if (v.begin(), v.end(),
                                     [bus] (const mesa::OutputDef& o)
                                     { return o.busSource == bus && o.name.rfind ("MON ", 0) == 0; }),
                     v.end());

            const bool isSec = ! a.stream.empty() && a.stream[0] == '\x01';
            if (! isSec)
            {
                aoMudarPar (a.index);        // caminho direto da mestra
                return;
            }

            aoMudarPar (-1);                 // sai da mestra
            const auto resto = a.stream.substr (1);
            const auto sep   = resto.find ('\x01');

            mesa::OutputDef o;
            o.name       = "MON " + std::to_string (bus);
            o.kind       = a.kind;
            o.pair       = a.index;
            o.busSource  = bus;
            o.deviceType = resto.substr (0, sep);
            o.deviceName = resto.substr (sep + 1);
            v.push_back (o);
        };

        p.addRow (rotulo, box);
    }

    juce::ComboBox* pairBox (CfgPage& p, const juce::String& label, int current,
                             std::function<void (int)> onChange, bool inputs = false)
    {
        auto* box = new juce::ComboBox();
        box->addItem ("nao roteado", 1);

        // Pares da placa de verdade, com os nomes que ela informa.
        //
        // Antes eram quatro pares fixos no codigo, numerados a mao. Numa placa
        // de 10 saidas o operador nao enxergava metade delas, e os rotulos nao
        // batiam com o que a aba Outputs mostrava — dois lugares falando da
        // mesma coisa em linguagens diferentes.
        int n = inputs ? 8 : 4;
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            const auto nomes = inputs ? dev->getInputChannelNames()
                                      : dev->getOutputChannelNames();
            n = juce::jmax (1, nomes.size() / 2);
            for (int i = 0; i + 1 < nomes.size(); i += 2)
                box->addItem (nomes[i] + " / " + nomes[i + 1], i / 2 + 2);
        }
        else
            for (int i = 0; i < n; ++i)
                box->addItem (juce::String (i * 2 + 1) + "/" + juce::String (i * 2 + 2), i + 2);
        box->setSelectedId (current < 0 ? 1 : current + 2, juce::dontSendNotification);
        box->onChange = [box, onChange] { onChange (box->getSelectedId() - 2); };
        return p.addRow (label, box);
    }

    juce::Slider* dbSlider (CfgPage& p, const juce::String& label, float current,
                            float lo, float hi, std::function<void (float)> onChange)
    {
        auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s->setRange (lo, hi, 0.5);
        s->setValue (current, juce::dontSendNotification);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
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

    juce::TextEditor* textBox (CfgPage& p, const juce::String& label, const std::string& current,
                               std::function<void (const juce::String&)> onChange)
    {
        auto* e = new juce::TextEditor();
        e->setText (current, juce::dontSendNotification);
        e->setFont (theme::mono (12.0f));
        e->onTextChange = [e, onChange] { onChange (e->getText()); };
        return p.addRow (label, e);
    }

    void addTab (const juce::String& name, CfgPage* page)
    {
        tabs.addTab (name, theme::surfaceLo, new CfgScroller (page), true);
    }

    /** Redesenha as abas na hora. Sem isso, criar ou remover um canal parece
        nao fazer nada — o dado muda, a tela nao. */
    /** Reconstroi as abas SEM perder onde a pessoa estava lendo.

        A varredura de rede e a descoberta NDI atualizam a lista sozinhas, e
        cada atualizacao refazia a aba inteira — a rolagem voltava ao topo no
        meio de uma leitura, sem que ninguem tivesse tocado em nada. Guardamos
        a posicao e devolvemos depois de montar. */
    void rebuildTabs()
    {
        const int keep = tabs.getCurrentTabIndex();
        const int ondeEstava = rolagemAtual();

        tabs.clearTabs();
        buildAllTabs();
        tabs.setCurrentTabIndex (juce::jlimit (0, tabs.getNumTabs() - 1, keep));

        if (ondeEstava > 0)
            juce::MessageManager::callAsync ([this, ondeEstava]
            {
                // depois do layout: antes dele a altura ainda e zero e a
                // posicao seria descartada
                if (auto* sc = dynamic_cast<CfgScroller*> (tabs.getCurrentContentComponent()))
                    sc->vaiPara (ondeEstava);
            });
    }

    int rolagemAtual() const
    {
        if (auto* sc = dynamic_cast<const CfgScroller*> (tabs.getCurrentContentComponent()))
            return sc->posicaoDaRolagem();
        return 0;
    }

    void buildAllTabs()
    {
        addTab ("Inputs",      buildCatalog());
        addTab ("Outputs",     buildOutputsTab());
        addTab ("Paginas",     buildPages());
        addTab ("Rec",         buildRec());
        addTab ("Monitoracao", buildMonitor());
        addTab ("Rede",        buildNetwork());
        addTab ("Automacao",   buildAutomation());
        addTab ("DSP",         buildDsp());
        addTab ("Usuarios",    buildUsers());
        addTab ("Sistema",     buildSystem());
    }

    /** TODAS as fontes de audio que a maquina oferece agora, numa lista so:
        entradas da placa aberta e streams NDI vistos na rede. E assim que a
        Axia mostra — o operador escolhe a fonte, nao o meio de transporte. */
    /** Transporte. Livewire e Dante NAO sao caminhos distintos para nos: os dois
        aparecem como dispositivo ASIO ou WDM criado pelo driver deles. Ficam aqui
        para o operador se orientar; o filtro e pelo nome do dispositivo. */
    enum class Transport { Asio = 0, Windows, Livewire, Dante, Ndi };

    /** Id da opcao "sem entrada". Alto de proposito: os ids normais sao a
        posicao na lista, e o seguinte ja e usado pela opcao "(offline)". */
    static constexpr int kSemEntrada = 9999;

    static bool deviceMatches (Transport t, const juce::String& typeName, const juce::String& devName)
    {
        const auto d = devName.toLowerCase();
        const bool aoip = d.contains ("dante") || d.contains ("livewire") || d.contains ("axia");
        switch (t)
        {
            case Transport::Livewire: return d.contains ("livewire") || d.contains ("axia");
            case Transport::Dante:    return d.contains ("dante");
            // Windows mostra TUDO, inclusive os dispositivos do driver da Axia.
            //
            // Antes eles eram filtrados daqui porque tinham categoria propria.
            // Quando o tipo Livewire virou recepcao nativa por numero de canal,
            // esses dispositivos ficaram sem lugar nenhum na interface — e sao
            // justamente eles que resolvem o caso do PC que gera e consome
            // Livewire na mesma maquina, onde a recepcao nativa nao alcanca.
            case Transport::Asio:     return typeName == "ASIO" && ! aoip;
            case Transport::Windows:  return typeName != "ASIO";
            default:                  return false;
        }
    }

    struct Avail { juce::String label; int kind; int index; std::string stream; };

    /** Fontes do transporte escolhido. NDI vem da descoberta; os demais vem do
        dispositivo aberto no momento (o Windows abre um ASIO por vez). */
    std::vector<Avail> availableSources (Transport t) const
    {
        std::vector<Avail> v;
        v.push_back ({ "(sem fonte)", int (mesa::InputKind::Device), -1, {} });

        if (t == Transport::Ndi)
        {
            for (const auto& n : NdiEngine::instance().sources())
                v.push_back ({ juce::String (n.name), int (mesa::InputKind::Network), 0, n.name });
            return v;
        }

        // placa MESTRA: entradas diretas, no relogio do callback
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            const juce::String typeName = deviceManager.getCurrentAudioDeviceType();
            const juce::String devName  = dev->getName();
            if (deviceMatches (t, typeName, devName))
            {
                const auto names = dev->getInputChannelNames();
                for (int i = 0; i < names.size(); ++i)
                    v.push_back ({ "[mestra] " + devName + " - " + names[i],
                                   int (mesa::InputKind::Device), i, {} });
            }
        }

        // placas SECUNDARIAS: entram por fila, com correcao de relogio.
        // Nunca ponha microfone aqui — a latencia inviabiliza o retorno no fone.
        {
            const auto master = deviceManager.getCurrentAudioDevice();
            const juce::String masterName = master != nullptr ? master->getName() : juce::String();

            auto scan = [&] (juce::AudioIODeviceType* dt, const char* typeTag)
            {
                if (dt == nullptr) return;
                dt->scanForDevices();
                for (const auto& dn : dt->getDeviceNames (true))
                {
                    if (dn == masterName) continue;                      // essa e a mestra
                    if (! deviceMatches (t, typeTag, dn)) continue;

                    // Quantos canais o dispositivo TEM, e nao oito por suposicao.
                    // Um Livewire In e estereo: as outras seis entradas nao
                    // existem, e escolher uma delas daria silencio sem aviso.
                    int quantos = 8;
                    if (auto* d = dt->createDevice ({}, dn))
                    {
                        quantos = juce::jmax (1, d->getInputChannelNames().size());
                        delete d;
                    }
                    for (int c = 0; c < quantos; ++c)
                        v.push_back ({ "[secundaria " + juce::String (typeTag) + "] " + dn
                                           + (dn.toLowerCase().contains ("livewire")
                                                  ? " (driver Axia)" : juce::String())
                                           + " - entrada " + juce::String (c + 1),
                                       int (mesa::InputKind::Network), c,
                                       (juce::String::charToString (1) + juce::String (typeTag)
                                        + juce::String::charToString (1) + dn).toStdString() });
                }
            };

           #if JUCE_ASIO
            // dois drivers ASIO DIFERENTES convivem: o que nao existe e abrir
            // o mesmo driver duas vezes
            if (t == Transport::Asio || t == Transport::Livewire || t == Transport::Dante)
            {
                std::unique_ptr<juce::AudioIODeviceType> at (
                    juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
                scan (at.get(), "ASIO");
            }
           #endif

            if (t == Transport::Windows || t == Transport::Livewire || t == Transport::Dante)
            {
                std::unique_ptr<juce::AudioIODeviceType> wt (
                    juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));

                // Entrada PADRAO do Windows, resolvida na hora de abrir.
                //
                // Util quando o dispositivo troca de nome — cabo virtual
                // reinstalado, placa USB em outra porta — ou quando a mesa vai
                // para outra maquina: em vez de apontar para um nome que pode
                // sumir, aponta para "o que o Windows estiver usando".
                if (t == Transport::Windows)
                    for (int c = 0; c < 8; ++c)
                        v.push_back ({ "[secundaria] entrada PADRAO do Windows - canal "
                                           + juce::String (c + 1),
                                       int (mesa::InputKind::Network), c,
                                       (juce::String::charToString (1) + "Windows Audio"
                                        + juce::String::charToString (1) + "*PADRAO*").toStdString() });

                scan (wt.get(), "Windows Audio");
            }
        }
        return v;
    }

    /** Saidas do transporte escolhido, no mesmo formato dos inputs: pares da
        placa mestra e pares das secundarias. */
    std::vector<Avail> availableOutputs (Transport t) const
    {
        std::vector<Avail> v;
        v.push_back ({ "(nao roteado)", int (mesa::InputKind::Device), -1, {} });

        if (t == Transport::Ndi)
        {
            v.push_back ({ "emissor NDI desta mesa (ainda nao transmite)",
                           int (mesa::InputKind::Network), 0, "MESA" });
            return v;
        }

        const auto master = deviceManager.getCurrentAudioDevice();
        const juce::String masterName = master != nullptr ? master->getName() : juce::String();

        // pares da MESTRA: saem direto no callback, sem fila
        if (master != nullptr && deviceMatches (t, deviceManager.getCurrentAudioDeviceType(), masterName))
        {
            const auto names = master->getOutputChannelNames();
            for (int i = 0; i + 1 < names.size(); i += 2)
                v.push_back ({ "[mestra] " + masterName + " - " + names[i] + " / " + names[i + 1],
                               int (mesa::InputKind::Device), i / 2, {} });
        }

        // pares das SECUNDARIAS: passam por fila, com conversao de taxa
        auto scan = [&] (juce::AudioIODeviceType* dt, const char* typeTag)
        {
            if (dt == nullptr) return;
            dt->scanForDevices();
            for (const auto& dn : dt->getDeviceNames (false))
            {
                if (dn == masterName) continue;
                if (! deviceMatches (t, typeTag, dn)) continue;
                for (int pr = 0; pr < 4; ++pr)
                    v.push_back ({ "[secundaria " + juce::String (typeTag) + "] " + dn
                                       + " - saidas " + juce::String (pr * 2 + 1)
                                       + "/" + juce::String (pr * 2 + 2),
                                   int (mesa::InputKind::Network), pr,
                                   (juce::String::charToString (1) + juce::String (typeTag)
                                    + juce::String::charToString (1) + dn).toStdString() });
            }
        };

       #if JUCE_ASIO
        if (t == Transport::Asio || t == Transport::Livewire || t == Transport::Dante)
        {
            std::unique_ptr<juce::AudioIODeviceType> at (
                juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
            scan (at.get(), "ASIO");
        }
       #endif
        if (t == Transport::Windows || t == Transport::Livewire || t == Transport::Dante)
        {
            std::unique_ptr<juce::AudioIODeviceType> wt (
                juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
            scan (wt.get(), "Windows Audio");
        }
        return v;
    }

    /** Controles de dispositivo, no topo dos Inputs: e aqui que se escolhe o
        driver. Antes moravam numa aba separada, e ficava confuso ter uma aba
        que decidia o que a outra podia mostrar. */
    void addDeviceControls (CfgPage& p)
    {
        p.addTitle ("Placa mestra");

        auto* typeBox = new juce::ComboBox();
        const auto& types = deviceManager.getAvailableDeviceTypes();
        for (int i = 0; i < types.size(); ++i) typeBox->addItem (types[i]->getTypeName(), i + 1);
        typeBox->setText (deviceManager.getCurrentAudioDeviceType(), juce::dontSendNotification);
        typeBox->onChange = [this, typeBox]
        {
            deviceManager.setCurrentAudioDeviceType (typeBox->getText(), true);
            rebuildTabs();
        };
        p.addRow ("Driver", typeBox);

        auto* devBox = new juce::ComboBox();
        if (auto* t = deviceManager.getCurrentDeviceTypeObject())
        {
            t->scanForDevices();
            const auto names = t->getDeviceNames();
            for (int i = 0; i < names.size(); ++i) devBox->addItem (names[i], i + 1);
        }
        if (auto* dev = deviceManager.getCurrentAudioDevice())
            devBox->setText (dev->getName(), juce::dontSendNotification);
        devBox->onChange = [this, devBox]
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.inputDeviceName = setup.outputDeviceName = devBox->getText();
            deviceManager.setAudioDeviceSetup (setup, true);
            rebuildTabs();
        };
        p.addRow ("Placa", devBox);

        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            auto* rateBox = new juce::ComboBox();
            for (auto r : dev->getAvailableSampleRates())
                rateBox->addItem (juce::String (int (r)) + " Hz", int (r));
            rateBox->setSelectedId (int (dev->getCurrentSampleRate()), juce::dontSendNotification);
            rateBox->onChange = [this, rateBox]
            {
                auto setup = deviceManager.getAudioDeviceSetup();
                setup.sampleRate = rateBox->getSelectedId();
                deviceManager.setAudioDeviceSetup (setup, true);
            };
            p.addRow ("Taxa de amostragem", rateBox);

            auto* bufBox = new juce::ComboBox();
            for (auto b : dev->getAvailableBufferSizes()) bufBox->addItem (juce::String (b), b);
            bufBox->setSelectedId (dev->getCurrentBufferSizeSamples(), juce::dontSendNotification);
            bufBox->onChange = [this, bufBox]
            {
                auto setup = deviceManager.getAudioDeviceSetup();
                setup.bufferSize = bufBox->getSelectedId();
                deviceManager.setAudioDeviceSetup (setup, true);
            };
            p.addRow ("Buffer (amostras)", bufBox);

            p.addRow ("Latencia informada",
                      makeReadOnly (std::to_string (int (1000.0 * (dev->getInputLatencyInSamples()
                                     + dev->getOutputLatencyInSamples()) / dev->getCurrentSampleRate()))
                                    + " ms"));

            const int nIn  = dev->getInputChannelNames().size();
            const int nOut = dev->getOutputChannelNames().size();
            p.addRow ("Canais", makeReadOnly (std::to_string (nIn) + " entradas, "
                                              + std::to_string (nOut) + " saidas"));

            // sem isso, uma lista de fontes vazia parece defeito quando na
            // verdade e a placa que nao oferece entrada nenhuma
            if (nIn == 0)
                p.addNote ("ATENCAO: esta placa nao expoe nenhuma entrada, entao a lista de "
                           "fontes ASIO vem vazia. Escolha outra placa acima, ou use uma "
                           "fonte de rede (NDI) ou uma placa secundaria pelo tipo Windows.");
        }
        else
        {
            p.addNote ("Nenhuma placa aberta. Escolha driver e placa acima.");
        }
    }

    /** LISTA de inputs, como a tela de Source Profiles do QOR: nome, uso e
        fonte, uma linha cada. Abrir tudo de uma vez virava um paredao de
        controles em que ninguem achava nada. */
    CfgPage* buildCatalog()
    {
        if (editandoInput >= 0 && editandoInput < int (settings.catalog.sources.size()))
            return buildInputDetail (size_t (editandoInput));

        auto* p = new CfgPage();
        addDeviceControls (*p);

        p->addTitle ("No Livewire");
        {
            // Escolha da placa: numa maquina com mais de uma, o Windows decide
            // sozinho por onde pedir o multicast e costuma decidir errado. O
            // sintoma e cruel: abre sem erro e nunca recebe pacote.
            auto* placa = new juce::ComboBox();
            placa->addItem ("automatica (deduzida da varredura)", 1);

            int selP = 1, idP = 2;
            for (const auto& ip : juce::IPAddress::getAllAddresses (false))
            {
                if (ip.toString() == "127.0.0.1") continue;
                placa->addItem (ip.toString(), idP);
                if (ip.toString().toStdString() == settings.livewirePlaca) selP = idP;
                ++idP;
            }
            placa->setSelectedId (selP, juce::dontSendNotification);
            placa->onChange = [this, placa]
            {
                settings.livewirePlaca = placa->getSelectedId() == 1
                                             ? std::string()
                                             : placa->getText().toStdString();
            };
            p->addRow ("Placa de rede", placa);
            p->addNote ("A mesma que o painel da Axia chama de Livewire Network Card. "
                        "Em automatica, a mesa usa a placa que enxerga os equipamentos "
                        "encontrados na varredura — e o que evita o pior sintoma desta "
                        "funcao: multicast pedido pela placa errada, que produz silencio "
                        "sem nenhuma mensagem de erro.");
            if (! settings.livewirePlaca.empty())
                p->addRow ("Em uso", makeReadOnly (settings.livewirePlaca));
        }
        {
            auto* nodeBox = new juce::TextEditor();
            nodeBox->setText (settings.livewireNode, juce::dontSendNotification);
            nodeBox->setFont (theme::mono (12.0f));
            nodeBox->onTextChange = [this, nodeBox]
            { settings.livewireNode = nodeBox->getText().toStdString(); };
            p->addRow ("Endereco do no", nodeBox);

            // Varredura: acha os equipamentos sozinha, sem IP cadastrado.
            // Endereco muda; nome nao. Cadastro de IP se quebra sozinho meses
            // depois, e sempre no pior momento.
            auto* varrer = new juce::TextButton (varredura.emAndamento()
                                                     ? "VARRENDO..." : "PROCURAR NA REDE");
            varrer->onClick = [this]
            {
                if (varredura.emAndamento()) { varredura.cancela(); stopTimer(); rebuildTabs(); return; }
                procuraLivewire();     // um caminho so: o mesmo da varredura automatica
                rebuildTabs();
            };
            p->addWide (varrer, 32);

            if (varredura.emAndamento())
                p->addRow ("Progresso",
                           makeReadOnly (std::to_string (varredura.progresso()) + " de "
                                         + std::to_string (varredura.totalEnderecos())
                                         + " enderecos"));

            for (const auto& a : varredura.resultado())
                p->addRow (a.equipamento.isEmpty() ? a.ip : a.equipamento,
                           makeReadOnly ((a.ip + "   " + juce::String (int (a.fontes.size()))
                                          + " fontes").toStdString()));

            p->addNote ("Procura quem responde ao LWRP na sub-rede desta placa. Nao "
                        "depende de IP cadastrado nem do multicast de anuncios — se o "
                        "switch bloquear multicast, isto continua funcionando.");

            auto* scan = new juce::TextButton ("CONSULTAR SO ESTE ENDERECO");
            scan->onClick = [this] { scanLivewire(); };
            p->addRow ("", scan, 30);

            // Escuta dos anuncios: e assim que a lista do QOR se enche com
            // fontes de OUTROS equipamentos, inclusive o driver da Axia. O
            // LWRP acima so conhece as fontes do proprio no.
            auto* ouvir = new juce::TextButton (navegador != nullptr && navegador->ativo()
                                                    ? "PARAR DE OUVIR A REDE"
                                                    : "OUVIR ANUNCIOS DA REDE");
            ouvir->onClick = [this]
            {
                if (navegador == nullptr) navegador = std::make_unique<LivewireBrowser>();
                if (navegador->ativo()) navegador->stop();
                else navegador->start (juce::String (settings.livewirePlaca));
                rebuildTabs();
            };
            p->addRow ("", ouvir, 30);

            if (navegador != nullptr && navegador->ativo())
            {
                juce::String est;
                est << navegador->pacotes() << " anuncios recebidos";
                if (navegador->erro().isNotEmpty()) est << "  |  " << navegador->erro();
                p->addRow ("Anuncios", makeReadOnly (est.toStdString()));

                const auto achadas = navegador->fontes();
                if (achadas.empty())
                {
                    p->addNote ("Nenhuma fonte reconhecida ainda. Se o contador acima esta "
                                "em zero, o multicast de anuncios nao chega — placa de rede "
                                "ou IGMP. Se esta subindo mas nada aparece, o formato do "
                                "anuncio difere do esperado: mande as amostras abaixo.");
                    for (const auto& linha : navegador->amostrasCruas())
                        p->addNote (linha);
                }
                else
                {
                    for (const auto& f : achadas)
                        p->addRow (juce::String (f.canal),
                                   makeReadOnly ((f.nome + "   (" + f.origem + ")").toStdString()));
                    p->addNote ("Estas vieram dos anuncios da rede. Para usar, escolha o "
                                "canal no campo do input.");
                }
            }

            p->addRow ("Estado", makeReadOnly (livewireStatus.toStdString()));
            p->addNote ("Consulta o no pelo LWRP, na porta 93, e traz a lista de fontes "
                        "com nome e canal — a mesma que aparece na superficie da Axia. "
                        "Depois de listar, escolha a fonte no campo de cada input.");
        }


        p->addTitle ("Inputs");

        auto* add = new juce::TextButton ("ADICIONAR INPUT");
        add->onClick = [this]
        {
            mesa::SourceDef d;
            d.name = "INPUT " + std::to_string (settings.catalog.sources.size() + 1);
            settings.catalog.add (d);
            editandoInput = int (settings.catalog.sources.size()) - 1;
            rebuildTabs();
        };
        p->addRow ("Novo", add, 30);

        static const char* usos[] = { "MIC Operador", "MIC Produtor",
                                      "MIC Convidado (controle)", "MIC Convidado (estudio)",
                                      "MIC Externo", "Linha", "Telefone", "Codec",
                                      "Player de PC", "Feed de Estudio" };

        for (size_t i = 0; i < settings.catalog.sources.size(); ++i)
        {
            const auto& src = settings.catalog.sources[i];

            juce::String fonte;
            if (src.livewireChannel > 0)      fonte = "Livewire " + juce::String (src.livewireChannel);
            else if (! src.streamName.empty()) fonte = "NDI " + juce::String (src.streamName);
            else if (! src.deviceName.empty()) fonte = juce::String (src.deviceName)
                                                     + " entrada " + juce::String (src.deviceChannel + 1);
            else if (src.index >= 0)           fonte = "entrada " + juce::String (src.index + 1);
            else                               fonte = "sem fonte";

            auto* linha = new juce::TextButton (juce::String (src.name)
                                                + "      " + usos[juce::jlimit (0, 9, src.type)]
                                                + "      " + fonte);
            linha->setColour (juce::TextButton::buttonColourId, theme::surfaceLo);
            linha->onClick = [this, i] { editandoInput = int (i); rebuildTabs(); };
            p->addWide (linha, 28);
        }



        {
            p->addTitle ("Latencia das placas secundarias");

            auto* modeBox = new juce::ComboBox();
            modeBox->addItem ("Minima (menos margem)", 1);
            modeBox->addItem ("Equilibrada", 2);
            modeBox->addItem ("Segura (mais margem)", 3);
            modeBox->setSelectedId (settings.secondaryLatencyMode + 1, juce::dontSendNotification);
            modeBox->onChange = [this, modeBox]
            {
                settings.secondaryLatencyMode = modeBox->getSelectedId() - 1;
                if (secondaries != nullptr)
                {
                    secondaries->setLatencyMode (settings.secondaryLatencyMode);
                    secondaries->closeAll();     // reabre com a nova profundidade
                }
                statusLabel.setText ("feche as configuracoes para aplicar",
                                     juce::dontSendNotification);
            };
            p->addRow ("Modo", modeBox);
            p->addNote ("Minima corta a fila ao osso. Se a coluna de falhas abaixo subir "
                        "durante a operacao, esta rasa demais para esta maquina — suba um "
                        "nivel. Falha zero por meia hora e o sinal de que aguenta.");
        }

        if (secondaries != nullptr && secondaries->count() > 0)
        {
            p->addTitle ("Placas secundarias em uso");
            p->addNote ("A secundaria sempre acrescenta latencia: o buffer do driver mais "
                        "a fila que absorve a diferenca de relogio. ASIO usa fila rasa; "
                        "WASAPI precisa de folga. Microfone deve ficar na mestra.");
            for (int i = 0; i < secondaries->count(); ++i)
            {
                if (auto* d = secondaries->at (i))
                {
                    juce::String txt = juce::String (int (d->sampleRate())) + " Hz  |  +"
                                     + juce::String (d->latencyMs(), 1) + " ms de fila";
                    txt += "  |  falhas: " + juce::String (d->glitches());
                    if (d->dropouts() > 0) txt += "  |  quedas: " + juce::String (d->dropouts());
                    if (d->isLost())       txt += "  |  PERDIDA";
                    p->addRow (d->deviceName(), makeReadOnly (txt.toStdString()));
                }
            }
        }

        if (secondaries != nullptr && secondaries->count() > 0)
        {
            p->addTitle ("Placas secundarias em uso");
            for (int i = 0; i < secondaries->count(); ++i)
                if (auto* d = secondaries->at (i))
                {
                    juce::String txt = juce::String (int (d->sampleRate())) + " Hz  |  +"
                                     + juce::String (d->latencyMs(), 1) + " ms de fila"
                                     + (d->recebendo()
                                            ? "  |  recebendo"
                                            : "  |  SEM AUDIO: abriu mas nao entrega nada")
                                     + "  |  buracos " + juce::String (d->buracos())
                                     + "  |  faltas " + juce::String (d->faltas())
                                     + "  |  descartes " + juce::String (d->descartes());
                    if (d->isLost()) txt += "  |  PERDIDA";
                    p->addRow (d->deviceName(), makeReadOnly (txt.toStdString()));
                }
        }
        return p;
    }

    /** Detalhe de UM input. */
    CfgPage* buildInputDetail (size_t indice)
    {
        auto& src = settings.catalog.sources[indice];
        auto* p = new CfgPage();

        auto* voltar = new juce::TextButton ("< VOLTAR A LISTA");
        voltar->onClick = [this] { editandoInput = -1; rebuildTabs(); };
        p->addWide (voltar, 30);
        p->addTitle (juce::String (src.name));

        // Nomes que dizem o que a coisa E. "Operador" nao deixa claro que se
        // trata de microfone — e o tipo e justamente o que decide o mute
        // automatico do monitor e o mix-minus.
        static const char* uses[] = { "MIC Operador", "MIC Produtor",
                                      "MIC Convidado (controle)", "MIC Convidado (estudio)",
                                      "MIC Externo", "Linha", "Telefone", "Codec",
                                      "Player de PC", "Feed de Estudio" };
        const size_t si = indice;
        {

            auto* nameBox = new juce::TextEditor();
            nameBox->setText (src.name, juce::dontSendNotification);
            nameBox->setFont (theme::mono (12.0f));
            nameBox->onTextChange = [nameBox, &src] { src.name = nameBox->getText().toStdString(); };
            p->addRow ("Nome", nameBox);

            const Transport t = inputTransport (si, src);

            auto* tBox = new juce::ComboBox();
            tBox->addItem ("ASIO", 1);     tBox->addItem ("Windows", 2);
            tBox->addItem ("Livewire", 3); tBox->addItem ("Dante", 4);
            tBox->addItem ("NDI", 5);
            tBox->setSelectedId (int (t) + 1, juce::dontSendNotification);
            tBox->onChange = [this, tBox, si] { inputT[si] = tBox->getSelectedId() - 1; rebuildTabs(); };
            p->addRow ("Tipo", tBox);

            // Livewire nativo: nao ha dispositivo para listar — o canal vira
            // endereco por conta, e o operador digita o numero que ve no QOR.
            if (t == Transport::Livewire)
            {
                // lista vinda do no, como na superficie da Axia
                auto* lwList = new juce::ComboBox();
                lwList->addItem ("(nenhuma)", 1);
                int sel = 1, id = 2;
                for (const auto& lw : livewireSources)
                {
                    lwList->addItem (juce::String (lw.livewireChannel) + "  " + lw.name, id);
                    if (lw.livewireChannel == src.livewireChannel) sel = id;
                    ++id;
                }
                if (sel == 1 && src.livewireChannel > 0)
                {
                    // Mostra o nome guardado em vez de "(fora da lista)": a
                    // fonte nao sumiu, so ainda nao varremos a rede nesta
                    // sessao. Dizer que sumiu assusta sem motivo.
                    const juce::String rotulo = src.livewireNome.empty()
                        ? juce::String (src.livewireChannel) + "  (procurando na rede...)"
                        : juce::String (src.livewireChannel) + "  " + src.livewireNome;
                    lwList->addItem (rotulo, id);
                    sel = id;
                }
                lwList->setSelectedId (sel, juce::dontSendNotification);

                auto listCopy = livewireSources;
                lwList->onChange = [lwList, &src, listCopy]
                {
                    const int i = lwList->getSelectedId() - 2;

                    // escolher "(nenhuma)" LIMPA de verdade: antes caia no
                    // return abaixo e o canal antigo ficava, entao o input
                    // continuava ativo com uma fonte que a tela dizia nao ter
                    if (lwList->getSelectedId() == 1)
                    {
                        src.livewireChannel = 0;
                        src.livewireNome.clear();
                        return;
                    }

                    if (i < 0 || i >= int (listCopy.size())) return;
                    src.livewireChannel = listCopy[size_t (i)].livewireChannel;
                    src.livewireNome    = listCopy[size_t (i)].name.toStdString();
                    src.kind  = int (mesa::InputKind::Network);
                    src.index = -1;
                    src.kind = int (mesa::InputKind::Network);
                    src.streamName.clear();
                    src.deviceName.clear();
                    src.index = -1;
                };
                p->addRow ("Fonte Livewire", lwList);

                auto* lwBox = new juce::TextEditor();
                lwBox->setText (juce::String (src.livewireChannel), juce::dontSendNotification);
                lwBox->setInputRestrictions (5, "0123456789");
                lwBox->setFont (theme::mono (12.0f));
                lwBox->onTextChange = [lwBox, &src]
                {
                    src.livewireChannel = lwBox->getText().getIntValue();
                    if (src.livewireChannel > 0)
                    {
                        src.kind = int (mesa::InputKind::Network);
                        src.streamName.clear();
                        src.deviceName.clear();
                        src.index = -1;
                    }
                };
                p->addRow ("ou canal direto", lwBox);

                auto* sideBox = new juce::ComboBox();
                sideBox->addItem ("Esquerdo", 1);
                sideBox->addItem ("Direito", 2);
                sideBox->addItem ("Estereo (soma L+R)", 3);
                sideBox->setSelectedId (src.livewireSide + 1, juce::dontSendNotification);
                sideBox->onChange = [sideBox, &src] { src.livewireSide = sideBox->getSelectedId() - 1; };
                p->addRow ("Canal do estereo", sideBox);
                p->addNote ("O canal desta mesa e MONO com pan, como em console de radio: "
                            "cada fader carrega um sinal. Para microfone, telefone e codec "
                            "isso e o certo. Para playout com musica, use a soma — pegar so "
                            "um lado perderia metade do conteudo.");

                p->addRow ("Endereco", makeReadOnly (src.livewireChannel > 0
                    ? (LivewireReceiver::addressForChannel (src.livewireChannel) + ":5004").toStdString()
                    : std::string ("-")));

                p->addNote ("O numero e o mesmo que aparece na lista de fontes do QOR. "
                            "A mesa entra no grupo multicast e recebe direto, sem driver "
                            "e sem PTP: a diferenca de relogio e absorvida pela mesma "
                            "correcao usada nas placas secundarias.");
            }
            else
            {
            const auto avail = availableSources (t);
            auto* srcBox = new juce::ComboBox();
            int sel = 1;
            for (size_t k = 0; k < avail.size(); ++k)
            {
                srcBox->addItem (avail[k].label, int (k) + 1);
                const auto& a = avail[k];
                const bool isSecondary = ! a.stream.empty() && a.stream[0] == '\x01';
                const bool match = k > 0 && a.kind == src.kind
                    && (isSecondary
                            ? (a.stream.find (src.deviceName) != std::string::npos
                                   && a.index == src.deviceChannel)
                            : (src.kind == int (mesa::InputKind::Network)
                                   ? a.stream == src.streamName
                                   : a.index == src.index));
                if (match) sel = int (k) + 1;
            }
            if (sel == 1 && ! (src.index == -1 && src.streamName.empty()))
            {
                srcBox->addItem (src.kind == int (mesa::InputKind::Network)
                                     ? juce::String (src.streamName) + "  (offline)"
                                     : "entrada " + juce::String (src.index + 1) + "  (outro dispositivo)",
                                 int (avail.size()) + 1);
                sel = int (avail.size()) + 1;
            }
            srcBox->addItem ("(sem entrada — canal inativo)", kSemEntrada);
            if (src.index == -1 && src.streamName.empty() && src.deviceName.empty()
                && src.livewireChannel == 0)
                sel = kSemEntrada;

            srcBox->setSelectedId (sel, juce::dontSendNotification);
            if (avail.size() <= 1)
                srcBox->setTextWhenNoChoicesAvailable ("nada disponivel neste tipo");
            auto availCopy = avail;
            srcBox->onChange = [srcBox, &src, availCopy]
            {
                // "(sem entrada)" e escolha legitima, nao falta de escolha: o
                // canal fica inativo de proposito e a tira anuncia isso. Antes
                // so dava para apagar o nome, o que parecia defeito.
                //
                // Id proprio e alto: os ids normais sao a posicao na lista, e
                // o seguinte ja pertence a opcao "(offline)".
                if (srcBox->getSelectedId() == kSemEntrada)
                {
                    src.kind  = int (mesa::InputKind::Device);
                    src.index = -1;
                    src.deviceName.clear();
                    src.streamName.clear();
                    src.livewireChannel = 0;
                    src.livewireNome.clear();
                    return;
                }

                const int i = srcBox->getSelectedId() - 1;
                if (i < 0 || i >= int (availCopy.size())) return;
                const auto& a = availCopy[size_t (i)];
                src.kind = a.kind;

                // Escolher dispositivo APAGA o canal Livewire.
                //
                // Sem isto, um input que ja tivera canal Livewire voltava como
                // Livewire ao reabrir a tela — a deducao do tipo olha o canal
                // antes do dispositivo — e o dispositivo escolhido sumia. Duas
                // fontes gravadas ao mesmo tempo, uma delas fantasma.
                src.livewireChannel = 0;
                src.livewireNome.clear();

                // marcador \x01 no campo stream distingue placa secundaria de NDI
                if (! a.stream.empty() && a.stream[0] == '\x01')
                {
                    const auto rest = a.stream.substr (1);
                    const auto sep  = rest.find ('\x01');
                    src.deviceType    = rest.substr (0, sep);
                    src.deviceName    = rest.substr (sep + 1);
                    src.deviceChannel = a.index;
                    src.streamName.clear();
                    src.index = -1;               // o hub atribui o slot
                }
                else
                {
                    src.streamName = a.stream;
                    src.deviceName.clear();
                    src.index = a.index;
                }
            };
            p->addRow ("Fonte", srcBox);
            if (avail.size() <= 1)
                p->addNote (t == Transport::Ndi
                    ? "Nenhum emissor NDI visto na rede agora. Confira a aba Rede."
                    : t == Transport::Asio
                      ? "Nenhuma entrada ASIO. A placa mestra tem entradas? Para cabo "
                        "virtual e placas do Windows, troque o Tipo para Windows."
                      : "Nenhuma entrada disponivel neste tipo. Tente outro tipo.");

            }

            auto* useBox = new juce::ComboBox();
            for (int k = 0; k < 10; ++k) useBox->addItem (uses[k], k + 1);
            useBox->setSelectedId (src.type + 1, juce::dontSendNotification);
            useBox->onChange = [useBox, &src] { src.type = useBox->getSelectedId() - 1; };
            p->addRow ("Uso", useBox);

            dbSlider (*p, "Trim", src.trimDb, -25.0f, 25.0f, [&src] (float v) { src.trimDb = v; });

            auto* camBox = new juce::ComboBox();
            camBox->addItem ("sem camera", 1);
            for (int k = 1; k <= 8; ++k) camBox->addItem ("CAM " + juce::String (k), k + 1);
            camBox->setSelectedId (src.camera + 1, juce::dontSendNotification);
            camBox->onChange = [camBox, &src] { src.camera = camBox->getSelectedId() - 1; };
            p->addRow ("Camera", camBox);

            toggle (*p, "Transcrever a fala deste input", src.transcrever,
                    [&src] (bool v) { src.transcrever = v; });

            dbSlider (*p, "Fala: nivel minimo", src.falaThresholdDb, -70.0f, -20.0f,
                      [&src] (float v) { src.falaThresholdDb = v; });
            {
                auto* maxMs = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                maxMs->setRange (3000.0, 30000.0, 500.0);
                maxMs->setValue (src.falaMaxTrechoMs, juce::dontSendNotification);
                maxMs->onValueChange = [maxMs, &src] { src.falaMaxTrechoMs = float (maxMs->getValue()); };
                p->addRow ("Fala: trecho maximo (ms)", maxMs);
                p->addNote ("Fala continua — musica, locucao sem pausa — nunca entrega o "
                            "silencio que fecharia o trecho. Este teto garante que o "
                            "arquivo saia mesmo assim.");
            }

            p->addNote ("Medido na ENTRADA do canal, antes de trim e fader: o texto do que "
                        "foi dito nao deve depender de onde o operador deixou o fader. "
                        "Se nao gravar nada, veja o medidor IN da tira — o nivel precisa "
                        "passar deste valor com folga.");

            p->addTitle ("GPIO deste input");

            auto* gpoP = new juce::TextEditor();
            gpoP->setText (juce::String (src.gpoPorta), juce::dontSendNotification);
            gpoP->setInputRestrictions (2, "0123456789");
            gpoP->onTextChange = [gpoP, &src] { src.gpoPorta = gpoP->getText().getIntValue(); };
            p->addRow ("Saida: porta", gpoP);

            auto* gpoN = new juce::TextEditor();
            gpoN->setText (juce::String (src.gpoPino), juce::dontSendNotification);
            gpoN->setInputRestrictions (1, "12345");
            gpoN->onTextChange = [gpoN, &src] { src.gpoPino = gpoN->getText().getIntValue(); };
            p->addRow ("Saida: pino", gpoN);
            p->addNote ("Acompanha o ON/OFF do canal: fecha o contato quando abre o fader. "
                        "E o que acende a luz de ar e aciona rele. Zero desliga.");

            auto* gpiP = new juce::TextEditor();
            gpiP->setText (juce::String (src.gpiPorta), juce::dontSendNotification);
            gpiP->setInputRestrictions (2, "0123456789");
            gpiP->onTextChange = [gpiP, &src] { src.gpiPorta = gpiP->getText().getIntValue(); };
            p->addRow ("Entrada: porta", gpiP);

            auto* gpiN = new juce::TextEditor();
            gpiN->setText (juce::String (src.gpiPino), juce::dontSendNotification);
            gpiN->setInputRestrictions (1, "12345");
            gpiN->onTextChange = [gpiN, &src] { src.gpiPino = gpiN->getText().getIntValue(); };
            p->addRow ("Entrada: pino", gpiN);
            p->addNote ("Contato externo liga e desliga este canal — botao de mesa, pedal, "
                        "comando de estudio. Zero desliga.");

            p->addTitle ("Trigger");
            dbSlider (*p, "Threshold", src.thresholdDb, -70.0f, -10.0f,
                      [&src] (float v) { src.thresholdDb = v; });
            toggle (*p, "Trigger ligado", src.triggerEnabled,
                    [&src] (bool v) { src.triggerEnabled = v; });

            auto* del = new juce::TextButton ("REMOVER INPUT");
            const std::string nameCopy = src.name;
            del->onClick = [this, nameCopy]
            { settings.catalog.remove (nameCopy); editandoInput = -1; rebuildTabs(); };
            p->addRow ("", del, 28);
        }



        return p;
    }


    /** Outputs: destinos da mesa. Cada um diz o que sai e por onde. */
    CfgPage* buildOutputsTab()
    {
        auto* p = new CfgPage();
        p->addTitle ("Outputs");
        p->addNote ("Saidas da placa mestra saem direto no callback. Saidas de placa "
                    "secundaria passam por fila, com conversao de taxa — mesma mecanica "
                    "dos inputs. Envio por NDI ainda nao transmite.");

        auto* add = new juce::TextButton ("ADICIONAR OUTPUT");
        add->onClick = [this]
        {
            mesa::OutputDef o;
            o.name = "OUTPUT " + std::to_string (settings.outputs.outputs.size() + 1);
            settings.outputs.add (o);
            rebuildTabs();
        };
        p->addRow ("Novo", add, 30);

        static const char* busNames[] = { "PGM 1", "PGM 2", "PGM 3", "PGM 4",
                                          "CUE", "Monitor CR", "Fone", "Estudio" };

        for (size_t oi = 0; oi < settings.outputs.outputs.size(); ++oi)
        {
            auto& out = settings.outputs.outputs[oi];
            p->addTitle (juce::String (out.name));

            auto* nameBox = new juce::TextEditor();
            nameBox->setText (out.name, juce::dontSendNotification);
            nameBox->setFont (theme::mono (12.0f));
            nameBox->onTextChange = [nameBox, &out] { out.name = nameBox->getText().toStdString(); };
            p->addRow ("Nome", nameBox);

            auto* busBox = new juce::ComboBox();
            for (int k = 0; k < 8; ++k) busBox->addItem (busNames[k], k + 1);
            busBox->setSelectedId (out.busSource + 1, juce::dontSendNotification);
            busBox->onChange = [busBox, &out] { out.busSource = busBox->getSelectedId() - 1; };
            p->addRow ("O que sai", busBox);

            const Transport t = outputTransport (oi, out);

            auto* tBox = new juce::ComboBox();
            tBox->addItem ("ASIO", 1);     tBox->addItem ("Windows", 2);
            tBox->addItem ("Livewire", 3); tBox->addItem ("Dante", 4);
            tBox->addItem ("NDI", 5);
            tBox->setSelectedId (int (t) + 1, juce::dontSendNotification);
            tBox->onChange = [this, tBox, oi] { outputT[oi] = tBox->getSelectedId() - 1; rebuildTabs(); };
            p->addRow ("Tipo", tBox);

            // Livewire nativo na saida: a mesa TRANSMITE naquele canal e o no
            // Axia recebe apontando o Primary source para o numero.
            if (t == Transport::Livewire)
            {
                auto* lwBox = new juce::TextEditor();
                lwBox->setText (juce::String (out.livewireChannel), juce::dontSendNotification);
                lwBox->setInputRestrictions (5, "0123456789");
                lwBox->setFont (theme::mono (12.0f));
                lwBox->onTextChange = [lwBox, &out]
                {
                    out.livewireChannel = lwBox->getText().getIntValue();
                    if (out.livewireChannel > 0)
                    {
                        out.kind = int (mesa::InputKind::Network);
                        out.deviceName.clear();
                        out.streamName.clear();
                        out.pair = -1;
                    }
                };
                p->addRow ("Canal a transmitir", lwBox);
                p->addRow ("Endereco", makeReadOnly (out.livewireChannel > 0
                    ? (LivewireSender::addressForChannel (out.livewireChannel) + ":5004").toStdString()
                    : std::string ("-")));
                p->addNote ("A mesa vira fonte Livewire nesse canal. Ela NAO aparece na "
                            "lista de nomes do QOR — aquela lista vem do protocolo de "
                            "anuncio, que e proprietario. No no, aponte o Primary source "
                            "para este numero.");
                p->addNote ("Transmitimos no relogio da maquina, sem travar no PTP da "
                            "rede. A correcao adaptativa absorve a diferenca, mas isso "
                            "so se prova em teste longo com o no real.");
            }
            else
            {
            const auto avail = availableOutputs (t);
            auto* dstBox = new juce::ComboBox();
            int sel = 1;
            for (size_t k = 0; k < avail.size(); ++k)
            {
                dstBox->addItem (avail[k].label, int (k) + 1);
                const auto& a = avail[k];
                const bool isSec = ! a.stream.empty() && a.stream[0] == '\x01';
                if (k > 0 && a.kind == out.kind && a.index == out.pair
                    && (isSec ? a.stream.find (out.deviceName) != std::string::npos
                              : a.stream == out.streamName))
                    sel = int (k) + 1;
            }
            dstBox->setSelectedId (sel, juce::dontSendNotification);
            auto availCopy = avail;
            dstBox->onChange = [dstBox, &out, availCopy]
            {
                const int i = dstBox->getSelectedId() - 1;
                if (i < 0 || i >= int (availCopy.size())) return;
                const auto& a = availCopy[size_t (i)];
                out.kind = a.kind;
                out.pair = a.index;

                if (! a.stream.empty() && a.stream[0] == '\x01')
                {
                    const auto rest = a.stream.substr (1);
                    const auto sep  = rest.find ('\x01');
                    out.deviceType = rest.substr (0, sep);
                    out.deviceName = rest.substr (sep + 1);
                    out.streamName.clear();
                }
                else
                {
                    out.streamName = a.stream;
                    out.deviceName.clear();
                }
            };
            p->addRow ("Para onde", dstBox);

            }

            auto* del = new juce::TextButton ("REMOVER OUTPUT");
            const std::string nameCopy = out.name;
            del->onClick = [this, nameCopy] { settings.outputs.remove (nameCopy); rebuildTabs(); };
            p->addRow ("", del, 28);
        }

        if (settings.outputs.outputs.empty())
            p->addNote ("Nenhum output ainda. Aperte ADICIONAR OUTPUT.");

        dbSlider (*p, "Ganho do master", settings.routing.masterGainDb, -20.0f, 20.0f,
                  [this] (float v) { settings.routing.masterGainDb = v; mix.masterGainDb.store (v); });
        return p;
    }

    /** Guarda o tipo escolhido por linha; sem escolha, deduz do que esta gravado. */
    /** Deduz o transporte a partir do que esta GRAVADO.

        Cuidado aqui: placa secundaria e tratada internamente como "rede",
        porque entra pela mesma fila assincrona. Como NDI era o primeiro caso
        de rede testado, um cabo virtual do Windows reaparecia como NDI ao
        reabrir a tela — o dado estava certo, o campo mostrado e que mentia.
        Quem decide e o campo preenchido, nao o tipo interno. */
    Transport inputTransport (size_t i, const mesa::SourceDef& d) const
    {
        auto it = inputT.find (i);
        if (it != inputT.end()) return Transport (it->second);

        if (d.livewireChannel > 0)   return Transport::Livewire;
        if (! d.streamName.empty())  return Transport::Ndi;
        if (! d.deviceName.empty())  return transporteDoDispositivo (d.deviceType, d.deviceName);
        return Transport::Asio;
    }

    /** Placa secundaria: o tipo vem do DRIVER, nunca do nome.

        Nome nao decide transporte. "Livewire In 03" e um dispositivo WDM do
        driver da Axia — Windows, portanto —, e nao recepcao Livewire nativa.
        Deduzir pelo nome fazia a tela abrir o formulario de canal multicast
        para um input que na verdade e placa: o resumo da lista mostrava o
        dispositivo certo e o detalhe aparecia vazio, como se a fonte tivesse
        sumido.

        Recepcao nativa se reconhece por ter numero de canal preenchido, e isso
        e decidido antes de chegar aqui. */
    static Transport transporteDoDispositivo (const std::string& tipo, const std::string&)
    {
        return juce::String (tipo) == "ASIO" ? Transport::Asio : Transport::Windows;
    }
    Transport outputTransport (size_t i, const mesa::OutputDef& o) const
    {
        auto it = outputT.find (i);
        if (it != outputT.end()) return Transport (it->second);

        if (o.livewireChannel > 0)   return Transport::Livewire;
        if (! o.streamName.empty())  return Transport::Ndi;
        if (! o.deviceName.empty())  return transporteDoDispositivo (o.deviceType, o.deviceName);
        return Transport::Asio;
    }

    /** Gravacao de programa. */
    CfgPage* buildRec()
    {
        auto* p = new CfgPage();
        p->addTitle ("Gravacao");

        textBox (*p, "Pasta", settings.recPasta,
                 [this] (const juce::String& v) { settings.recPasta = v.toStdString(); });
        p->addNote ("Vazio grava em uma subpasta 'gravacoes' junto da configuracao.");

        auto* ponto = new juce::ComboBox();
        ponto->addItem ("Entrada crua do canal", 1);
        ponto->addItem ("Canal pos-fader", 2);
        ponto->addItem ("Programa (PGM 1)", 3);
        ponto->setSelectedId (settings.recPonto + 1, juce::dontSendNotification);
        ponto->onChange = [this, ponto] { settings.recPonto = ponto->getSelectedId() - 1; };
        p->addRow ("O que gravar", ponto);
        p->addNote ("Entrada crua nao passa por trim, DSP nem fader — e o que serve para "
                    "diagnosticar defeito de audio, porque nao carrega nada nosso. "
                    "Pos-fader grava o que o canal entrega. Programa grava o PGM 1 e "
                    "independe de canal.");

        auto* bits = new juce::ComboBox();
        bits->addItem ("16 bits", 1);
        bits->addItem ("24 bits", 2);
        bits->addItem ("32 bits", 3);
        bits->setSelectedId (settings.recBits == 16 ? 1 : settings.recBits == 32 ? 3 : 2,
                             juce::dontSendNotification);
        bits->onChange = [this, bits]
        {
            const int id = bits->getSelectedId();
            settings.recBits = id == 1 ? 16 : id == 3 ? 32 : 24;
        };
        p->addRow ("Resolucao", bits);
        p->addNote ("24 bits e o padrao: sobra margem e nao acrescenta ruido proprio. "
                    "16 bits ocupa menos disco e basta para arquivo de programa.");

        toggle (*p, "Gravar na taxa da placa", settings.recTaxaDaPlaca,
                [this] (bool v) { settings.recTaxaDaPlaca = v; });
        p->addNote ("Ligado grava na taxa em que a placa esta rodando, sem conversao — "
                    "menos processamento e nenhuma chance de estragar o material.");

        p->addTitle ("Transporte");
        p->addNote ("Os tres botoes ficam no pe da coluna esquerda da mesa: gravar, "
                    "pausar e parar. Pausar mantem o arquivo aberto e retomar continua "
                    "no mesmo, sem emenda; parar fecha e o proximo comeca outro.");
        p->addNote ("O REC de cada tira grava aquele canal. O botao do transporte grava o "
                    "canal com CUE ligado, ou o primeiro com fonte.");

        return p;
    }

    /** Paginas: mapa de posicao de fader para nome de fonte. */
    CfgPage* buildPages()
    {
        auto* p = new CfgPage();
        p->addTitle ("Paginas de fader");
        p->addNote ("Cada pagina diz que fonte fica em cada posicao. Trocar de pagina "
                    "NAO tira do ar: canal aberto fica pendente ate ser fechado.");

        auto* add = new juce::TextButton ("ADICIONAR PAGINA");
        add->onClick = [this]
        {
            mesa::FaderPage pg;
            pg.name = "PAGINA " + std::to_string (settings.pages.pages.size() + 1);
            pg.slots.assign (size_t (mix.numChannels()), std::string());
            settings.pages.pages.push_back (pg);
            rebuildTabs();
        };
        p->addRow ("Nova", add, 30);

        for (auto& pg : settings.pages.pages)
        {
            p->addTitle (juce::String (pg.name));

            auto* nameBox = new juce::TextEditor();
            nameBox->setText (pg.name, juce::dontSendNotification);
            nameBox->setFont (theme::mono (12.0f));
            nameBox->onTextChange = [nameBox, &pg] { pg.name = nameBox->getText().toStdString(); };
            p->addRow ("Nome", nameBox);

            pg.slots.resize (size_t (mix.numChannels()));

            for (size_t i = 0; i < pg.slots.size(); ++i)
            {
                auto* box = new juce::ComboBox();
                box->addItem ("(vazio)", 1);
                int sel = 1, id = 2;
                for (const auto& src : settings.catalog.sources)
                {
                    box->addItem (src.name, id);
                    if (src.name == pg.slots[i]) sel = id;
                    ++id;
                }
                box->setSelectedId (sel, juce::dontSendNotification);
                auto* slot = &pg.slots[i];
                box->onChange = [box, slot]
                {
                    const auto t = box->getText();
                    *slot = (t == "(vazio)") ? std::string() : t.toStdString();
                };
                p->addRow ("Fader " + juce::String (int (i) + 1), box);
            }
        }
        return p;
    }

    CfgPage* buildMonitor()
    {
        auto* p = new CfgPage();
        p->addTitle ("Saidas de monitoracao");
        destinoDeMonitoracao (*p, "Monitor do controle", 5,
                              settings.routing.monitorOutputPair,
                              [this] (int v) { settings.routing.monitorOutputPair = v; });
        destinoDeMonitoracao (*p, "Fone do operador", 6,
                              settings.routing.phonesOutputPair,
                              [this] (int v) { settings.routing.phonesOutputPair = v; });
        destinoDeMonitoracao (*p, "Monitor do estudio", 7,
                              settings.routing.studioOutputPair,
                              [this] (int v) { settings.routing.studioOutputPair = v; });
        destinoDeMonitoracao (*p, "CUE", 4,
                              settings.routing.cueOutputPair,
                              [this] (int v) { settings.routing.cueOutputPair = v; });

        {
            juce::String jaCriados;
            for (const auto& o : settings.outputs.outputs)
                if (o.busSource >= 4)
                {
                    if (jaCriados.isNotEmpty()) jaCriados << "; ";
                    static const char* nomes[] = { "CUE", "Monitor CR", "Fone", "Estudio" };
                    jaCriados << nomes[juce::jlimit (0, 3, o.busSource - 4)]
                              << " -> " << juce::String (o.name);
                }
            if (jaCriados.isNotEmpty())
                p->addRow ("Ja na aba Outputs", makeReadOnly (jaCriados.toStdString()));
        }

        p->addTitle ("Fontes externas");
        pairBox (*p, "EXT 1 (entradas)", settings.routing.ext1InputPair,
                 [this] (int v) { settings.routing.ext1InputPair = v; }, true);
        pairBox (*p, "EXT 2 (entradas)", settings.routing.ext2InputPair,
                 [this] (int v) { settings.routing.ext2InputPair = v; }, true);

        p->addTitle ("Niveis");
        dbSlider (*p, "Monitor", mix.monitor.monitorDb.load(), -60.0f, 0.0f,
                  [this] (float v) { mix.monitor.monitorDb.store (v); });
        dbSlider (*p, "Fone", mix.monitor.phonesDb.load(), -60.0f, 0.0f,
                  [this] (float v) { mix.monitor.phonesDb.store (v); });
        dbSlider (*p, "CUE", mix.monitor.cueDb.load(), -60.0f, 0.0f,
                  [this] (float v) { mix.monitor.cueDb.store (v); });
        dbSlider (*p, "Estudio", mix.monitor.studioDb.load(), -60.0f, 0.0f,
                  [this] (float v) { mix.monitor.studioDb.store (v); });
        dbSlider (*p, "DIM no talkback", mix.monitor.dimDb.load(), -40.0f, 0.0f,
                  [this] (float v) { mix.monitor.dimDb.store (v); });
        p->addTitle ("CUE: onde entra");
        toggle (*p, "CUE entra no fone", settings.routing.cueToPhones,
                [this] (bool v)
                { settings.routing.cueToPhones = v; mix.monitor.cueToPhones.store (v); });
        toggle (*p, "CUE entra no monitor", settings.routing.cueToMonitor,
                [this] (bool v)
                { settings.routing.cueToMonitor = v; mix.monitor.cueToMonitor.store (v); });
        toggle (*p, "CUE entra no estudio", settings.routing.cueToStudio,
                [this] (bool v)
                { settings.routing.cueToStudio = v; mix.monitor.cueToStudio.store (v); });

        // Escala util ATE o mudo, sem escala enganosa.
        //
        // Ela ia a -120: o meio do curso caia em -60, que na pratica ja cala o
        // programa, e quem procurava "abaixa um pouco" achava silencio. Agora
        // o curso util e -40..0, e o ultimo passo — e so ele — e MUDO, para
        // quem quer ouvir SO o CUE. Os dois casos existem e agora convivem sem
        // um atrapalhar o outro.
        {
            auto* sl = new juce::Slider (juce::Slider::LinearHorizontal,
                                         juce::Slider::TextBoxRight);
            sl->setRange (-41.0, 0.0, 0.5);
            sl->setValue (settings.routing.cueDimDb <= -41.0f ? -41.0
                                                              : settings.routing.cueDimDb,
                          juce::dontSendNotification);
            sl->textFromValueFunction = [] (double v)
            { return v <= -41.0 ? juce::String ("MUDO") : juce::String (v, 1); };
            sl->onValueChange = [this, sl]
            {
                const double v = sl->getValue();
                // o ultimo passo vira silencio de verdade, nao -41 dB
                const float db = v <= -41.0 ? -120.0f : float (v);
                settings.routing.cueDimDb = db;
                mix.monitor.cueDimDb.store (db);
            };
            p->addRow ("DIM do CUE", sl);
        }
        p->addNote ("Quanto o que ja estava tocando abaixa enquanto o CUE toca. No fundo da "
                    "escala o CUE SUBSTITUI — era o unico comportamento antes. Em -12 o "
                    "programa fica audivel por baixo, que e como se confere um corte sem "
                    "perder o ar de vista. Zero deixa os dois no mesmo nivel.");

        p->addNote ("Mic aberto no controle muta o monitor automaticamente. "
                    "Isso vem do TIPO da fonte, nao de um botao.");
        return p;
    }

    CfgPage* buildNetwork()
    {
        auto* p = new CfgPage();

        p->addTitle ("NDI");
        auto& ndi = NdiEngine::instance();
        p->addRow ("Estado", makeReadOnly (ndi.status().toStdString()));

        ndiList = new juce::Label ({}, "procurando...");
        ndiList->setFont (theme::mono (11.0f));
        ndiList->setColour (juce::Label::textColourId, theme::oled);
        ndiList->setJustificationType (juce::Justification::topLeft);
        p->addWide (ndiList, 120);

        auto* rescan = new juce::TextButton ("ATUALIZAR LISTA");
        rescan->onClick = [this] { refreshNdiList(); };
        p->addRow ("Fontes vistas", rescan, 30);
        refreshNdiList();

        p->addNote ("A descoberta so fica ligada com esta janela aberta ou quando existe "
                    "input usando NDI — deixar a busca varrendo a rede o dia todo sem "
                    "necessidade ja derrubou a mesa uma vez.");
        p->addNote ("A descoberta roda sozinha em segundo plano. Se a lista vier vazia, "
                    "confira se o emissor esta na mesma sub-rede e se o mDNS nao esta "
                    "bloqueado pelo firewall.");

        p->addTitle ("Identificacao na rede");
        textBox (*p, "Nome desta maquina", settings.network.machineName,
                 [this] (const juce::String& v) { settings.network.machineName = v.toStdString(); });
        textBox (*p, "Servidor de descoberta", settings.network.discoveryServer,
                 [this] (const juce::String& v) { settings.network.discoveryServer = v.toStdString(); });
        toggle (*p, "Preferir multicast", settings.network.preferMulticast,
                [this] (bool v) { settings.network.preferMulticast = v; });

        p->addTitle ("Transcricao (experimental)");
        toggle (*p, "Gravar trechos de fala", settings.falaEnabled,
                [this] (bool v) { settings.falaEnabled = v; });
        textBox (*p, "Pasta dos trechos", settings.falaPasta,
                 [this] (const juce::String& v) { settings.falaPasta = v.toStdString(); });
        p->addNote ("A mesa corta a fala em trechos e grava em WAV 16 kHz. Quem transcreve "
                    "e um programa SEPARADO, que le a pasta e devolve o texto pela porta "
                    "8890. Isso e proposital: reconhecimento de fala e pesado e trava com "
                    "frequencia — dentro da mesa, uma travada dessas tiraria a emissora do "
                    "ar. Marque quais inputs transcrever na aba Inputs. Exige reabrir.");

        toggle (*p, "Transcricao em TEMPO REAL", settings.falaTempoReal,
                [this] (bool v) { settings.falaTempoReal = v; });

        {
            auto* chave = new juce::TextEditor();
            chave->setText (settings.falaChave, juce::dontSendNotification);
            chave->setFont (theme::mono (11.0f));
            chave->setPasswordCharacter (juce::juce_wchar ('*'));
            chave->onTextChange = [this, chave]
            { settings.falaChave = chave->getText().trim().toStdString(); };
            p->addRow ("Chave da API", chave);
            textBox (*p, "Transcritor (script)", settings.falaScript,
                     [this] (const juce::String& v) { settings.falaScript = v.toStdString(); });
            textBox (*p, "Python", settings.falaPython,
                     [this] (const juce::String& v) { settings.falaPython = v.toStdString(); });
            p->addNote ("A mesa inicia o transcritor sozinha ao abrir e o encerra ao fechar. "
                        "Vazio procura transcritor_tempo_real.py ao lado do executavel e usa "
                        "o 'python' do sistema. Ele roda como processo separado de proposito: "
                        "rede com TLS dentro do processo de audio ja derrubou esta mesa duas "
                        "vezes, e assim uma falha da API nao tira a emissora do ar.");
            p->addNote ("Guardada aqui para nao precisar passar na linha de comando toda "
                        "vez. O transcritor le com --chave-da-mesa. Fica escondida na tela, "
                        "mas o settings.json e texto puro — trate a maquina como confiavel.");
        }
        p->addNote ("Manda audio continuo para o transcritor externo, em vez de gravar "
                    "trecho e esperar ele fechar. O texto chega em menos de um segundo, "
                    "contra os cerca de 17 do caminho por arquivo. Precisa do "
                    "transcritor_tempo_real.py rodando. Exige reabrir.");

        {
            auto* abrirT = new juce::TextButton ("ABRIR AS TRANSCRICOES");
            abrirT->onClick = [this]
            { settingsFile.getParentDirectory().getChildFile ("transcricoes").revealToUser(); };
            p->addRow ("", abrirT, 28);
            p->addNote ("Um arquivo de texto por dia, com hora em cada linha. E o material "
                        "para responder, com dados, com que frequencia um VT seria acionado "
                        "por voz e quais expressoes o locutor usa de verdade.");
        }

        p->addTitle ("GPIO");
        toggle (*p, "GPIO habilitado", settings.gpioEnabled,
                [this] (bool v) { settings.gpioEnabled = v; });
        textBox (*p, "No do GPIO", settings.gpioNode,
                 [this] (const juce::String& v) { settings.gpioNode = v.toStdString(); });
        p->addNote ("Vazio usa o mesmo no do Livewire. O QOR desta instalacao ja tem "
                    "8 entradas e 8 saidas com contato seco — nao ha placa de rele para "
                    "comprar nem driver para instalar. O mapeamento de porta e pino fica "
                    "em cada input, na aba Inputs.");
        p->addNote ("Trocar isto exige reabrir a mesa.");

        p->addTitle ("Receber comandos");
        p->addNote ("Comandos de texto, uma linha cada: CH1 ON / CH1 PLAY, CH1 OFF, "
                    "CH1 PAUSE, CH2 FADER -6, CH3 CUE ON, CH2 MUTE OFF, CH1 TRIM 3. "
                    "O alvo pode ser o nome do canal: PLAYOUT A PAUSE.");
        p->addNote ("PAUSE guarda onde o fader estava e o PLAY seguinte retoma naquele "
                    "ponto — pausar nao apaga o ajuste do operador. OFF e fim: depois "
                    "dele o proximo ON entra no nivel padrao abaixo.");
        toggle (*p, "Habilitado", settings.remoteEnabled,
                [this] (bool v) { settings.remoteEnabled = v; });

        auto* uPort = new juce::TextEditor();
        uPort->setText (juce::String (settings.remoteUdpPort), juce::dontSendNotification);
        uPort->setInputRestrictions (5, "0123456789");
        uPort->onTextChange = [this, uPort] { settings.remoteUdpPort = uPort->getText().getIntValue(); };
        p->addRow ("Porta UDP", uPort);

        auto* tPort = new juce::TextEditor();
        tPort->setText (juce::String (settings.remoteTcpPort), juce::dontSendNotification);
        tPort->setInputRestrictions (5, "0123456789");
        tPort->onTextChange = [this, tPort] { settings.remoteTcpPort = tPort->getText().getIntValue(); };
        p->addRow ("Porta TCP", tPort);

        toggle (*p, "ON externo poe o fader em nivel", settings.remoteOnSetsFader,
                [this] (bool v) { settings.remoteOnSetsFader = v; });
        dbSlider (*p, "Nivel do ON externo", settings.remoteOnFaderDb, -20.0f, 10.0f,
                  [this] (float v) { settings.remoteOnFaderDb = v; });
        p->addNote ("Com isso ligado, um ON vindo da cartucheira sobe o fader ate esse "
                    "nivel se ele estiver abaixo — o playout entra no ar em nivel, sem "
                    "depender de onde o fader ficou. Se estiver acima, nao mexe.");
        p->addNote ("Comando recebido NAO redispara a logica de saida do canal: se ele "
                    "mandasse DECK1_PLAY de volta para quem acabou de pedir o ON, o laco "
                    "nao teria fim. Trocar a porta exige reabrir a mesa.");

        p->addTitle ("Destinos de comando");
        for (size_t i = 0; i < settings.targets.size(); ++i)
        {
            auto& t = settings.targets[i];
            const juce::String pfx = juce::String (t.name) + "  ";

            textBox (*p, pfx + "host", t.host,
                     [&t] (const juce::String& v) { t.host = v.toStdString(); });

            auto* portBox = new juce::TextEditor();
            portBox->setText (juce::String (t.port), juce::dontSendNotification);
            portBox->setInputRestrictions (5, "0123456789");
            portBox->onTextChange = [portBox, &t] { t.port = portBox->getText().getIntValue(); };
            p->addRow (pfx + "porta", portBox);

            auto* protoBox = new juce::ComboBox();
            protoBox->addItem ("TCP", 1); protoBox->addItem ("UDP", 2); protoBox->addItem ("HTTP", 3);
            protoBox->setSelectedId (t.protocol == "UDP" ? 2 : t.protocol == "HTTP" ? 3 : 1,
                                     juce::dontSendNotification);
            protoBox->onChange = [protoBox, &t]
            {
                t.protocol = protoBox->getSelectedId() == 2 ? "UDP"
                           : protoBox->getSelectedId() == 3 ? "HTTP" : "TCP";
            };
            p->addRow (pfx + "protocolo", protoBox);

            toggle (*p, pfx + "fim de linha", t.appendNewline,
                    [&t] (bool v) { t.appendNewline = v; });
        }

        p->addNote ("A API TCP do vMix espera fim de linha. A cartucheira, no UDP, nao.");
        return p;
    }

    CfgPage* buildAutomation()
    {
        auto* p = new CfgPage();
        p->addTitle ("vMix");
        {
            auto* portBox = new juce::TextEditor();
            portBox->setText (juce::String (settings.vmixApiPort), juce::dontSendNotification);
            portBox->setInputRestrictions (5, "0123456789");
            portBox->onTextChange = [this, portBox]
            { settings.vmixApiPort = portBox->getText().getIntValue(); };
            p->addRow ("Porta da API (listagem)", portBox);

            auto* fetch = new juce::TextButton ("BUSCAR ENTRADAS DO VMIX");
            fetch->onClick = [this] { fetchVmix(); };
            p->addRow ("", fetch, 30);
            p->addRow ("Estado", makeReadOnly (vmixStatus.toStdString()));
            p->addNote ("A porta 8088 e a do controlador web, usada so para LISTAR. "
                        "Os comandos continuam saindo pela 8099, configurada em Destinos.");
        }

        p->addTitle ("Automacao de cameras");
        toggle (*p, "Automacao geral", mix.automation.enabled.load(),
                [this] (bool v) { mix.automation.enabled.store (v); });
        toggle (*p, "Modo de teste", mix.automation.testMode.load(),
                [this] (bool v) { mix.automation.testMode.store (v); });
        toggle (*p, "Regra de dominancia", mix.automation.dominance.load(),
                [this] (bool v) { mix.automation.dominance.store (v); });
        dbSlider (*p, "Dominancia (dB)", mix.automation.dominanceDb.load(), 0.0f, 20.0f,
                  [this] (float v) { mix.automation.dominanceDb.store (v); });

        toggle (*p, "Fala interrompe o plano geral",
                mix.automation.geralInterrompivel.load(),
                [this] (bool v) { mix.automation.geralInterrompivel.store (v); });
        p->addNote ("Ligado: com a camera padrao no ar, quem comeca a falar assume na "
                    "hora, sem esperar o plano minimo. Desligado: o plano geral cumpre "
                    "o tempo minimo antes de ser trocado — corta menos, mas alguem pode "
                    "falar um instante com a geral no ar.");

        toggle (*p, "Conversa cruzada", mix.automation.multiTalkEnabled.load(),
                [this] (bool v) { mix.automation.multiTalkEnabled.store (v); });

        auto* multi = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        multi->setRange (0.0, 10000.0, 100.0);
        multi->setValue (mix.automation.multiTalkMs.load(), juce::dontSendNotification);
        multi->onValueChange = [this, multi]
        { mix.automation.multiTalkMs.store (float (multi->getValue())); };
        p->addRow ("Tempo para reconhecer (ms)", multi);

        auto* multiCam = new juce::ComboBox();
        multiCam->addItem ("usar a camera padrao", 1);
        int msel = 1, mid = 2;
        for (const auto& vi : vmixInputs)
        {
            multiCam->addItem (VmixClient::label (vi), mid);
            if (vi.number == mix.automation.multiTalkCamera.load()) msel = mid;
            ++mid;
        }
        multiCam->setSelectedId (msel, juce::dontSendNotification);
        auto mlist = vmixInputs;
        multiCam->onChange = [this, multiCam, mlist]
        {
            const int i = multiCam->getSelectedId() - 2;
            mix.automation.multiTalkCamera.store (i >= 0 && i < int (mlist.size())
                                                      ? mlist[size_t (i)].number : 0);
        };
        p->addRow ("Plano da conversa cruzada", multiCam);
        p->addNote ("Quando DUAS ou mais pessoas falam ao mesmo tempo por esse tempo, "
                    "a mesa vai para o plano aberto em vez de ficar escolhendo entre "
                    "elas. Sobreposicao curta e normal na fala — alguem concorda, ri, "
                    "completa a frase — por isso ha permanencia propria. "
                    "Terminada a conversa, quem continuar falando reassume a camera. "
                    "Desligada, a mesa segue escolhendo entre quem fala mais alto.");

        auto* minShot = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        minShot->setRange (200.0, 15000.0, 50.0);
        minShot->setValue (mix.automation.minShotMs.load(), juce::dontSendNotification);
        minShot->onValueChange = [this, minShot] { mix.automation.minShotMs.store (float (minShot->getValue())); };
        p->addRow ("Plano minimo (ms)", minShot);
        p->addNote ("Tempo minimo entre trocas de CAMERA. Serve contra pingue-pongue "
                    "quando duas pessoas se alternam rapido. NAO atrasa a volta ao "
                    "plano padrao — quem manda nisso e o hold de cada canal.");

        auto* wide = new juce::ComboBox();
        wide->addItem ("nenhuma (fica onde esta)", 1);
        int wsel = 1, wid = 2;
        for (const auto& vi : vmixInputs)
        {
            wide->addItem (VmixClient::label (vi), wid);
            if (vi.number == mix.automation.wideCamera.load()) wsel = wid;
            ++wid;
        }
        if (wsel == 1 && mix.automation.wideCamera.load() > 0)
        {
            wide->addItem (juce::String (mix.automation.wideCamera.load()) + " - (fora da lista)", wid);
            wsel = wid;
        }
        wide->setSelectedId (wsel, juce::dontSendNotification);
        auto wlist = vmixInputs;
        wide->onChange = [this, wide, wlist]
        {
            const int i = wide->getSelectedId() - 2;
            mix.automation.wideCamera.store (i >= 0 && i < int (wlist.size())
                                                 ? wlist[size_t (i)].number : 0);
        };
        p->addRow ("Camera padrao (BG)", wide);
        p->addNote ("Para onde a mesa volta quando ninguem esta falando e vence o tempo "
                    "de permanencia. Em nenhuma, ela simplesmente fica no ultimo plano.");

        auto* manHold = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        manHold->setRange (0.0, 60000.0, 500.0);
        manHold->setValue (mix.automation.manualHoldMs.load(), juce::dontSendNotification);
        manHold->onValueChange = [this, manHold]
        { mix.automation.manualHoldMs.store (float (manHold->getValue())); };
        p->addRow ("Permanencia do corte manual (ms)", manHold);
        p->addNote ("Quanto um disparo de teste ou comando externo segura o plano antes "
                    "de a mesa poder voltar ao padrao.");

        auto* resetAuto = new juce::TextButton ("VOLTAR AOS AJUSTES DE FABRICA");
        resetAuto->onClick = [this] { mesa::resetAutomation (mix); rebuildTabs(); };
        p->addRow ("Ajustes", resetAuto, 30);

        p->addNote ("Cooldown e plano minimo sao GLOBAIS. Cooldown por canal nao impede "
                    "pingue-pongue entre dois microfones.");
        p->addNote ("Para VT: a cartucheira pode mandar AUTOMACAO OFF quando a materia "
                    "entra e AUTOMACAO ON quando acaba, ou AUTOMACAO HOLD 30 para suspender "
                    "por 30 segundos e voltar sozinha. Sem isso, a mesa corta para quem "
                    "tossir no estudio no meio da materia.");
        return p;
    }

    CfgPage* buildDsp()
    {
        auto* p = new CfgPage();
        p->addTitle ("Rack de DSP");
        toggle (*p, "Escanear VST3 em outro processo", settings.dsp.scanOutOfProcess,
                [this] (bool v) { settings.dsp.scanOutOfProcess = v; });
        textBox (*p, "Pasta dos VST3", settings.dsp.vst3Path,
                 [this] (const juce::String& v) { settings.dsp.vst3Path = v.toStdString(); });
        p->addNote ("Escanear em processo separado e o que impede um plugin travado "
                    "de derrubar o console no ar. O host VST3 ainda nao esta ligado.");
        return p;
    }

    CfgPage* buildUsers()
    {
        auto* p = new CfgPage();
        p->addTitle ("Acesso");
        toggle (*p, "Exigir login", settings.access.requireLogin,
                [this] (bool v) { settings.access.requireLogin = v; });
        p->addRow ("Perfil em vigor", makeReadOnly (settings.access.active));
        p->addRow ("Usuario logado", makeReadOnly (settings.access.activeUser.empty()
                                                   ? std::string ("(ninguem)")
                                                   : settings.access.activeUser));

        p->addTitle ("Usuarios cadastrados");
        for (auto& u : settings.access.users)
        {
            const juce::String pfx = juce::String (u.name) + "  ";
            p->addRow (pfx + "perfil", makeReadOnly (u.profile));
            toggle (*p, pfx + "habilitado", u.enabled, [&u] (bool v) { u.enabled = v; });
        }

        p->addTitle ("Perfis");
        for (auto& pr : settings.access.profiles)
        {
            juce::String areas;
            if (pr.source)  areas += "fonte ";
            if (pr.gain)    areas += "ganho ";
            if (pr.buses)   areas += "buses ";
            if (pr.dsp)     areas += "dsp ";
            if (pr.trigger) areas += "trigger";
            p->addRow (juce::String (pr.name), makeReadOnly (areas.toStdString()));
        }

        p->addNote ("Cadastro e troca de PIN entram na proxima fatia. Aqui hoje da "
                    "para habilitar, desabilitar e conferir o que cada perfil abre.");
        return p;
    }

    CfgPage* buildSystem()
    {
        auto* p = new CfgPage();
        p->addTitle ("Versao");
        p->addRow ("Mesa", makeReadOnly (std::string (mesa::kVersion)
                                         + "  (" + mesa::kBuildName + ")"));
        p->addRow ("Compilada em", makeReadOnly (std::string (mesa::kBuildDate)));

        p->addTitle ("Gravacoes de diagnostico");
        p->addNote ("O gravador fica no SOFT de cada canal, aba Validacao. Ele grava a "
                    "entrada crua, sem nenhum processamento nosso — e a forma de "
                    "descobrir se um defeito de audio nasce antes ou depois da mesa.");

        // Aviso de conflito bem no alto: dois donos no mesmo par foi o defeito
        // mais caro desta semana, e a mesa nao dava sinal nenhum.
        {
            const auto aviso = mesa::conflitosDeSaida (settings);
            if (! aviso.empty())
            {
                auto* l = new juce::Label ({}, "CONFLITO DE SAIDA: " + aviso);
                l->setFont (theme::mono (12.0f, true));
                l->setColour (juce::Label::textColourId, theme::onRed);
                p->addWide (l, 34);
                p->addNote ("Dois caminhos no mesmo par nao somam: um sobrescreve o outro, e "
                            "o que sai depende da ordem interna. O sintoma e som mais baixo "
                            "ou diferente, sem nenhum erro aparecer. Aponte cada um para um "
                            "par proprio.");
            }
        }

        p->addTitle ("Nivel dos barramentos");
        p->addNote ("Zero e o correto: o caminho da mesa entrega unidade — fader em 0 sai "
                    "no mesmo nivel de qualquer outra fonte na mesma saida. Este ajuste "
                    "existe para casar com equipamento externo que espere outro nivel de "
                    "referencia, nao para corrigir a mesa.");

        for (int b = 0; b < mesa::kNumBuses; ++b)
        {
            auto* sl = new juce::Slider (juce::Slider::LinearHorizontal,
                                         juce::Slider::TextBoxRight);
            sl->setRange (-20.0, 20.0, 0.1);
            sl->setValue (settings.routing.busGainDb[b], juce::dontSendNotification);
            sl->onValueChange = [this, sl, b]
            { settings.routing.busGainDb[b] = float (sl->getValue()); };
            p->addRow ("PGM " + juce::String (b + 1), sl);
        }

        p->addTitle ("Cores da tally");
        p->addNote ("Vermelho no ar nao e universal — cada emissora tem sua convencao. "
                    "O texto se ajusta sozinho para contrastar com a cor escolhida.");
        {
            struct Item { const char* nome; unsigned* valor; };
            static const char* nomes[] = { "No ar", "Armado", "Espera", "Parado" };
            unsigned* alvos[] = { &settings.tallyOnAir, &settings.tallyArmed,
                                  &settings.tallyWait,  &settings.tallyIdle };

            for (int i = 0; i < 4; ++i)
            {
                auto* botao = new juce::TextButton ("ESCOLHER COR");
                unsigned* alvo = alvos[i];
                botao->setColour (juce::TextButton::buttonColourId, juce::Colour (*alvo));
                botao->setColour (juce::TextButton::textColourOffId,
                                  juce::Colour (*alvo).contrasting (0.9f));
                botao->onClick = [this, alvo, botao]
                {
                    auto* sel = new juce::ColourSelector (juce::ColourSelector::showColourAtTop
                                                        | juce::ColourSelector::showSliders
                                                        | juce::ColourSelector::showColourspace);
                    sel->setCurrentColour (juce::Colour (*alvo));
                    sel->setSize (300, 300);
                    // aplica ao vivo: a superficie por tras muda enquanto escolhe
                    appliers.add (new ColourApplier (alvo, botao, [this] { applyTally(); }));
                    sel->addChangeListener (appliers.getLast());
                    juce::CallOutBox::launchAsynchronously (std::unique_ptr<juce::Component> (sel),
                                                            botao->getScreenBounds(), nullptr);
                };
                p->addRow (nomes[i], botao, 28);
            }

            auto* padrao = new juce::TextButton ("VOLTAR AS CORES PADRAO");
            padrao->onClick = [this]
            {
                settings.tallyOnAir = 0xffff3b30;
                settings.tallyArmed = 0xffffb020;
                settings.tallyWait  = 0xff2b3440;
                settings.tallyIdle  = 0xff20242a;
                applyTally();
                rebuildTabs();
            };
            p->addRow ("", padrao, 28);
        }

        p->addTitle ("Registro tecnico");
        p->addNote ("O que a mesa esta fazendo — comandos, rede, avisos. Ficava na tela "
                    "principal, mas defeito se investiga sentado, e a tela do operador vale "
                    "mais mostrando o que esta sendo dito no ar.");
        {
            auto* abrirLog = new juce::TextButton ("ABRIR O REGISTRO");
            abrirLog->onClick = [this]
            { settingsFile.getSiblingFile ("mesa.log").revealToUser(); };
            p->addRow ("", abrirLog, 28);
        }

        p->addTitle ("Ajustes de fabrica");
        p->addNote ("Ponto de partida para fala de estudio: threshold -35 dBFS, "
                    "permanencia 300 ms, histerese 6 dB, hold 2500 ms, silencio antes "
                    "do BG 3 s, plano minimo 1500 ms, nivelador em -18 dBFS a 6 dB/s. "
                    "Nao e o ideal para a sua sala — e o NORTE de onde ajustar uma "
                    "coisa de cada vez.");

        auto* resetAll = new juce::TextButton ("RESETAR TODOS OS CANAIS E A AUTOMACAO");
        resetAll->onClick = [this]
        {
            mesa::resetAll (mix);
            statusLabel.setText ("ajustes de fabrica aplicados a todos os canais",
                                 juce::dontSendNotification);
            rebuildTabs();
        };
        p->addWide (resetAll, 32);
        p->addNote ("Fonte, roteamento, camera e comandos NAO sao tocados: o reset "
                    "devolve os TEMPOS e NIVEIS, nao a instalacao.");

        p->addTitle ("Instalacao");
        p->addRow ("Configuracao guardada em",
                   makeReadOnly (settingsFile.getParentDirectory().getFullPathName().toStdString()));
        p->addNote ("Fica FORA da pasta de compilacao de proposito: build/ e descartavel "
                    "e some a cada reconfiguracao, levando junto nomes de canal, "
                    "thresholds e cameras. Aqui sobrevive a versao nova e a build limpo.");

        auto* abrir = new juce::TextButton ("ABRIR A PASTA");
        abrir->onClick = [this] { settingsFile.getParentDirectory().revealToUser(); };
        p->addRow ("", abrir, 28);
        p->addRow ("Faders por camada", makeReadOnly (std::to_string (settings.surface.fadersPerLayer)));
        p->addRow ("Camadas", makeReadOnly (std::to_string (settings.surface.layers)));
        p->addRow ("Canais", makeReadOnly (std::to_string (mix.numChannels())));

        auto* meter = new juce::ComboBox();
        meter->addItem ("PPM", 1); meter->addItem ("VU", 2);
        meter->setSelectedId (settings.surface.meterMode == "VU" ? 2 : 1, juce::dontSendNotification);
        meter->onChange = [this, meter]
        { settings.surface.meterMode = meter->getSelectedId() == 2 ? "VU" : "PPM"; };
        p->addRow ("Modo do medidor", meter);

        p->addNote ("Cena e o que muda por programa. Isto aqui e a instalacao: "
                    "dispositivo, rede e destinos nao mudam quando troca o show.");
        return p;
    }

    void fetchVmix()
    {
        juce::String host = "127.0.0.1";
        for (const auto& t : settings.targets)
            if (juce::String (t.name).containsIgnoreCase ("vmix")) host = t.host;

        const auto r = VmixClient::query (host, settings.vmixApiPort);
        if (r.ok)
        {
            vmixInputs = r.inputs;
            vmixStatus = "vMix " + r.version + "  |  "
                       + juce::String (int (r.inputs.size())) + " entradas";
        }
        else { vmixInputs.clear(); vmixStatus = "falhou: " + r.error; }
        rebuildTabs();
    }

    /** Varredura automatica, com a placa deduzida do resultado. */
    void procuraLivewire()
    {
        if (varredura.emAndamento()) return;
        varredura.varre (juce::String (settings.livewirePlaca));
        ultimoAchado = 0;
        startTimerHz (4);        // acompanha o andamento por aqui
    }

    /** Acompanha a varredura.

        Antes o fim da varredura chamava de volta a janela a partir da thread
        dela. Se o operador fechasse as configuracoes no meio — e a varredura
        levava minutos —, o aviso chegava a um objeto ja destruido e a mesa
        caia. Perguntar de tempos em tempos e menos elegante e nao tem essa
        classe de defeito. */
    void timerCallback() override
    {
        const int agora = varredura.quantosAchados();
        const bool acabou = ! varredura.emAndamento();

        if (agora != ultimoAchado || acabou)
        {
            ultimoAchado = agora;
            livewireSources = varredura.todasAsFontes();

            if (settings.livewirePlaca.empty())
            {
                const auto p = varredura.placaDeduzida();
                if (p.isNotEmpty())
                {
                    settings.livewirePlaca = p.toStdString();
                    if (onLivewirePlaca) onLivewirePlaca (p);
                }
            }
            rebuildTabs();
        }

        if (acabou) stopTimer();
    }

    void scanLivewire()
    {
        const juce::String host (settings.livewireNode);
        if (host.isEmpty()) { livewireStatus = "informe o endereco do no"; rebuildTabs(); return; }

        const auto r = LwrpClient::query (host);
        if (! r.ok)
        {
            livewireStatus = "falhou: " + r.error;
            livewireSources.clear();
        }
        else
        {
            livewireSources = r.sources;
            livewireStatus = r.device + " " + r.version + "  |  "
                           + juce::String (int (r.sources.size())) + " fontes, "
                           + juce::String (int (r.destinations.size())) + " destinos";
        }
        rebuildTabs();
    }

    void refreshNdiList()
    {
        if (ndiList == nullptr) return;
        const auto list = NdiEngine::instance().sources();
        if (list.empty()) { ndiList->setText ("nenhuma fonte NDI encontrada", juce::dontSendNotification); return; }

        juce::String txt;
        for (const auto& s : list) txt << juce::String (s.name) << "\n";
        ndiList->setText (txt, juce::dontSendNotification);
    }

    juce::Label* makeReadOnly (const std::string& v)
    {
        auto* l = new juce::Label ({}, v);
        l->setFont (theme::mono (11.0f));
        l->setColour (juce::Label::textColourId, theme::oled);
        return l;
    }

    /** Leva as cores das configuracoes para a superficie. */
    void applyTally()
    {
        theme::tally().onAir = juce::Colour (settings.tallyOnAir);
        theme::tally().armed = juce::Colour (settings.tallyArmed);
        theme::tally().wait  = juce::Colour (settings.tallyWait);
        theme::tally().idle  = juce::Colour (settings.tallyIdle);
    }

    /** Aplica a cor enquanto o seletor esta aberto, para o operador ver o
        efeito na mesa atras da janela em vez de escolher no escuro. */
    struct ColourApplier : juce::ChangeListener
    {
        ColourApplier (unsigned* t, juce::TextButton* b, std::function<void()> aplicar)
            : alvo (t), botao (b), onChange (std::move (aplicar)) {}
        void changeListenerCallback (juce::ChangeBroadcaster* src) override
        {
            if (auto* sel = dynamic_cast<juce::ColourSelector*> (src))
            {
                const auto c = sel->getCurrentColour();
                *alvo = c.getARGB();
                botao->setColour (juce::TextButton::buttonColourId, c);
                botao->setColour (juce::TextButton::textColourOffId, c.contrasting (0.9f));
                if (onChange) onChange();      // reflete na mesa atras da janela
            }
        }
        unsigned* alvo; juce::TextButton* botao; std::function<void()> onChange;
    };

    void save()
    {
        settings.routing.masterGainDb = mix.masterGainDb.load();
        mesa::applyRouting (settings, mix);
        mesa::applyOutputs (settings.outputs, mix);
        applyTally();
        if (auto xml = deviceManager.createStateXml())
            settings.deviceState = xml->toString().toStdString();
        const auto json = mesa::settingsToJson (settings);
        settingsFile.replaceWithText (json);
        statusLabel.setText ("salvo em " + settingsFile.getFileName()
                             + " \x7c aplicado ao motor", juce::dontSendNotification);
    }

    mesa::Settings& settings;
    mesa::MixerEngine& mix;
    mesa::AutomationEngine& autom;
    juce::AudioDeviceManager& deviceManager;
    juce::File settingsFile;
    SecondaryDevices* secondaries = nullptr;
    juce::TabbedComponent tabs;
    juce::TextButton saveButton;
    juce::Label statusLabel, versaoLabel;
    juce::Label* ndiList = nullptr;
    mutable std::map<size_t, int> inputT, outputT;
    std::vector<LwrpClient::Source> livewireSources;
    std::unique_ptr<LivewireBrowser> navegador;
    LivewireScanner varredura;
    int ultimoAchado = 0;

public:
    /** Avisa a superficie que a placa mudou, para religar os receptores. */
    std::function<void (const juce::String&)> onLivewirePlaca;

private:
    int editandoInput = -1;
    juce::String livewireStatus { "nao consultado" };
    std::vector<VmixClient::Input> vmixInputs;
    juce::String vmixStatus { "nao consultado" };
    juce::OwnedArray<ColourApplier> appliers;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConfigComponent)
};
