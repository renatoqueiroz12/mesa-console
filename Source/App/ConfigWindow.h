#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Theme.h"
#include "../Core/Settings.h"
#include "../Core/MixerEngine.h"
#include "../Core/SourceCatalog.h"
#include "../Core/Version.h"
#include <map>
#include "../Core/AutomationEngine.h"
#include "NdiEngine.h"
#include "SecondaryDevices.h"

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
        addAndMakeVisible (control);
        items.add ({ l, control, height });
        return control;
    }

    void addWide (juce::Component* c, int height)
    {
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
class ConfigComponent : public juce::Component
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

        statusLabel.setFont (theme::mono (11.0f));
        statusLabel.setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (statusLabel);

        // enquanto o operador esta escolhendo fonte, a descoberta faz sentido
        NdiEngine::instance().startDiscovery();

        setSize (760, 560);
    }

    /** Avisa a superficie quando fecha, para religar as fontes de rede. */
    std::function<void()> onClosed;
    ~ConfigComponent() override
    {
        NdiEngine::instance().stopDiscovery();
        if (onClosed) onClosed();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::surface); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (40).reduced (12, 6);
        saveButton.setBounds (bottom.removeFromRight (180));
        bottom.removeFromRight (10);
        statusLabel.setBounds (bottom);
        tabs.setBounds (r);
    }

private:
    // ------------------------------------------------------------------ helpers
    juce::ComboBox* pairBox (CfgPage& p, const juce::String& label, int current,
                             std::function<void (int)> onChange, bool inputs = false)
    {
        auto* box = new juce::ComboBox();
        box->addItem ("nao roteado", 1);
        const int n = inputs ? 8 : 4;
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
    void rebuildTabs()
    {
        const int keep = tabs.getCurrentTabIndex();
        tabs.clearTabs();
        buildAllTabs();
        tabs.setCurrentTabIndex (juce::jlimit (0, tabs.getNumTabs() - 1, keep));
    }

    void buildAllTabs()
    {
        addTab ("Inputs",      buildCatalog());
        addTab ("Outputs",     buildOutputsTab());
        addTab ("Paginas",     buildPages());
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

    static bool deviceMatches (Transport t, const juce::String& typeName, const juce::String& devName)
    {
        const auto d = devName.toLowerCase();
        const bool aoip = d.contains ("dante") || d.contains ("livewire") || d.contains ("axia");
        switch (t)
        {
            case Transport::Livewire: return d.contains ("livewire") || d.contains ("axia");
            case Transport::Dante:    return d.contains ("dante");
            case Transport::Asio:     return typeName == "ASIO" && ! aoip;
            case Transport::Windows:  return typeName != "ASIO" && ! aoip;
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
                    for (int c = 0; c < 8; ++c)
                        v.push_back ({ "[secundaria " + juce::String (typeTag) + "] "
                                           + dn + " - entrada " + juce::String (c + 1),
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

    /** Inputs: cada canal tem nome, TIPO de transporte e fonte. Escolhido o tipo,
        a lista traz so o que aquele transporte oferece agora. */
    CfgPage* buildCatalog()
    {
        auto* p = new CfgPage();
        addDeviceControls (*p);

        p->addTitle ("Inputs");
        p->addNote ("Livewire e Dante chegam como dispositivo criado pelo driver deles; "
                    "estao separados aqui so para facilitar achar. NDI vem da rede.");

        auto* add = new juce::TextButton ("ADICIONAR INPUT");
        add->onClick = [this]
        {
            mesa::SourceDef d;
            d.name = "INPUT " + std::to_string (settings.catalog.sources.size() + 1);
            settings.catalog.add (d);
            rebuildTabs();
        };
        p->addRow ("Novo", add, 30);

        static const char* uses[] = { "Operador", "Produtor", "Convidado CR", "Convidado Estudio",
                                      "Mic Externo", "Linha", "Telefone", "Codec",
                                      "Player de PC", "Feed de Estudio" };

        for (size_t si = 0; si < settings.catalog.sources.size(); ++si)
        {
            auto& src = settings.catalog.sources[si];
            p->addTitle (juce::String (src.name));

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
            srcBox->setSelectedId (sel, juce::dontSendNotification);
            if (avail.size() <= 1)
                srcBox->setTextWhenNoChoicesAvailable ("nada disponivel neste tipo");
            auto availCopy = avail;
            srcBox->onChange = [srcBox, &src, availCopy]
            {
                const int i = srcBox->getSelectedId() - 1;
                if (i < 0 || i >= int (availCopy.size())) return;
                const auto& a = availCopy[size_t (i)];
                src.kind = a.kind;

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
                    : "Nenhuma entrada disponivel neste tipo. Veja a placa mestra acima, "
                      "ou tente outro tipo.");

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

            dbSlider (*p, "Threshold", src.thresholdDb, -70.0f, -10.0f,
                      [&src] (float v) { src.thresholdDb = v; });
            toggle (*p, "Trigger ligado", src.triggerEnabled,
                    [&src] (bool v) { src.triggerEnabled = v; });

            auto* del = new juce::TextButton ("REMOVER INPUT");
            const std::string nameCopy = src.name;
            del->onClick = [this, nameCopy] { settings.catalog.remove (nameCopy); rebuildTabs(); };
            p->addRow ("", del, 28);
        }

        if (settings.catalog.sources.empty())
            p->addNote ("Nenhum input ainda. Aperte ADICIONAR INPUT.");

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

            auto* del = new juce::TextButton ("REMOVER OUTPUT");
            const std::string nameCopy = out.name;
            del->onClick = [this, nameCopy] { settings.outputs.remove (nameCopy); rebuildTabs(); };
            p->addRow ("", del, 28);
        }

        if (settings.outputs.outputs.empty())
            p->addNote ("Nenhum output ainda. Aperte ADICIONAR OUTPUT.");

        dbSlider (*p, "Ganho do master", settings.routing.masterGainDb, -20.0f, 10.0f,
                  [this] (float v) { settings.routing.masterGainDb = v; mix.masterGainDb.store (v); });
        return p;
    }

    /** Guarda o tipo escolhido por linha; sem escolha, deduz do que esta gravado. */
    Transport inputTransport (size_t i, const mesa::SourceDef& d) const
    {
        auto it = inputT.find (i);
        if (it != inputT.end()) return Transport (it->second);
        return d.kind == int (mesa::InputKind::Network) ? Transport::Ndi : Transport::Asio;
    }
    Transport outputTransport (size_t i, const mesa::OutputDef& o) const
    {
        auto it = outputT.find (i);
        if (it != outputT.end()) return Transport (it->second);
        return o.kind == int (mesa::InputKind::Network) ? Transport::Ndi : Transport::Asio;
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
        pairBox (*p, "Monitor do controle", settings.routing.monitorOutputPair,
                 [this] (int v) { settings.routing.monitorOutputPair = v; });
        pairBox (*p, "Fone do operador", settings.routing.phonesOutputPair,
                 [this] (int v) { settings.routing.phonesOutputPair = v; });
        pairBox (*p, "Monitor do estudio", settings.routing.studioOutputPair,
                 [this] (int v) { settings.routing.studioOutputPair = v; });

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
        toggle (*p, "CUE sobrepoe o fone", mix.monitor.cueToPhones.load(),
                [this] (bool v) { mix.monitor.cueToPhones.store (v); });

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
        p->addTitle ("Automacao de cameras");
        toggle (*p, "Automacao geral", mix.automation.enabled.load(),
                [this] (bool v) { mix.automation.enabled.store (v); });
        toggle (*p, "Modo de teste", mix.automation.testMode.load(),
                [this] (bool v) { mix.automation.testMode.store (v); });
        toggle (*p, "Regra de dominancia", mix.automation.dominance.load(),
                [this] (bool v) { mix.automation.dominance.store (v); });
        dbSlider (*p, "Dominancia (dB)", mix.automation.dominanceDb.load(), 0.0f, 20.0f,
                  [this] (float v) { mix.automation.dominanceDb.store (v); });

        auto* wideDelay = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        wideDelay->setRange (0.0, 30000.0, 100.0);
        wideDelay->setValue (mix.automation.wideDelayMs.load(), juce::dontSendNotification);
        wideDelay->onValueChange = [this, wideDelay]
        { mix.automation.wideDelayMs.store (float (wideDelay->getValue())); };
        p->addRow ("Silencio antes do BG (ms)", wideDelay);
        p->addNote ("Quanto tempo sem ninguem falando antes de voltar ao plano geral. "
                    "Curto demais e a mesa volta ao BG na respirada entre frases.");

        auto* minShot = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        minShot->setRange (200.0, 15000.0, 50.0);
        minShot->setValue (mix.automation.minShotMs.load(), juce::dontSendNotification);
        minShot->onValueChange = [this, minShot] { mix.automation.minShotMs.store (float (minShot->getValue())); };
        p->addRow ("Plano minimo (ms)", minShot);

        auto* wide = new juce::ComboBox();
        wide->addItem ("sem camera geral", 1);
        for (int k = 1; k <= 8; ++k) wide->addItem ("CAM " + juce::String (k), k + 1);
        wide->setSelectedId (mix.automation.wideCamera.load() + 1, juce::dontSendNotification);
        wide->onChange = [this, wide] { mix.automation.wideCamera.store (wide->getSelectedId() - 1); };
        p->addRow ("Camera geral", wide);

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

        p->addTitle ("Instalacao");
        p->addRow ("Arquivo de configuracao", makeReadOnly (settingsFile.getFullPathName().toStdString()));
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

    void save()
    {
        settings.routing.masterGainDb = mix.masterGainDb.load();
        mesa::applyRouting (settings, mix);
        mesa::applyOutputs (settings.outputs, mix);
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
    juce::Label statusLabel;
    juce::Label* ndiList = nullptr;
    mutable std::map<size_t, int> inputT, outputT;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConfigComponent)
};
