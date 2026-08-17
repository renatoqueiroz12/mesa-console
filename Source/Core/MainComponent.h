#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "ChannelStrip.h"
#include "MeterBridge.h"
#include "MasterPanel.h"
#include "ConfigWindow.h"
#include "ChannelMenu.h"
#include "SecondaryDevices.h"
#include "NetworkHub.h"
#include "CommandReceiver.h"
#include "../Core/RemoteCommand.h"
#include <map>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #include <tlhelp32.h>
#endif
#include "Theme.h"
#include "CommandSender.h"
#include "../Core/Settings.h"
#include "../Core/Scene.h"
#include "../Core/Version.h"
#include "../Core/SourceCatalog.h"

/** Superficie da mesa. Fatia 1 do porte do mockup: chassi, ponte de medidores,
    8 faders por layer com A/B e a barra de status.
    O menu do canal (SOFT) e as 9 abas de configuracao entram nas proximas fatias;
    por enquanto o SOFT so avisa, e o seletor de dispositivo abre em janela. */
class MainComponent : public juce::Component, private juce::Timer
{
public:
    static constexpr int kFadersPerLayer = 8;

    explicit MainComponent (int numChannels) : engine (numChannels), bridge (engine.mixer)
    {
        settingsFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                           .getParentDirectory().getChildFile ("settings.json");
        if (settingsFile.existsAsFile())
            mesa::settingsFromJson (settingsFile.loadFileAsString().toStdString(), settings);
        else
            settingsFile.replaceWithText (mesa::settingsToJson (settings));

        auto err = engine.start (numChannels, 8);
        openError = err;

        mesa::applyRouting (settings, engine.mixer);
        sender = std::make_unique<CommandSender> (engine.automation, settings);

        sceneFile = settingsFile.getSiblingFile ("scene.json");
        mesa::Scene scene;
        if (sceneFile.existsAsFile()
            && mesa::sceneFromJson (sceneFile.loadFileAsString().toStdString(), scene))
            mesa::applyScene (scene, engine.mixer, false);
        else
        {
            setupBenchDefaults();
            sceneFile.replaceWithText (mesa::sceneToJson (mesa::captureScene (engine.mixer, "BANCADA")));
        }

        addAndMakeVisible (bridge);

        layerA = std::make_unique<SurfaceButton> ("A", theme::busGreen, 28.0f);
        layerB = std::make_unique<SurfaceButton> ("B", theme::busGreen, 28.0f);
        layerA->setSub ("LAYER\n1-8");
        layerB->setSub ("LAYER\n9-16");
        layerA->onClick = [this] { setLayer (0); };
        layerB->onClick = [this] { setLayer (1); };
        addAndMakeVisible (*layerA);
        addAndMakeVisible (*layerB);

        logFile = settingsFile.getSiblingFile ("mesa.log");
        startedMs = juce::Time::getMillisecondCounterHiRes();
        logToFile (juce::String ("=== mesa iniciada ===  v") + mesa::kVersion
                   + " (" + mesa::kBuildName + ", " + mesa::kBuildDate + ")  |  "
                   + juce::SystemStats::getOperatingSystemName()
                   + "  |  " + juce::String (juce::SystemStats::getMemorySizeInMegabytes()) + " MB"
                   + "  |  " + juce::String (juce::SystemStats::getNumCpus()) + " CPUs");

        // Batimento: uma linha por minuto. Se o arquivo parar numa hora e a
        // mesa nao registrar encerramento, foi queda — e sabemos QUANDO.
        heartbeat.onTimer = [this] { writeHeartbeat(); };
        heartbeat.startTimer (60000);

        if (settings.remoteEnabled)
        {
            receiver.start (settings.remoteUdpPort, settings.remoteTcpPort);
            if (receiver.error().isNotEmpty())
                pendingLog.add ("RECEPTOR: " + receiver.error());
            else
                pendingLog.add ("recebendo comandos em UDP/TCP "
                                + juce::String (settings.remoteUdpPort));
        }

        secondaries.setLatencyMode (settings.secondaryLatencyMode);
        hub = std::make_unique<NetworkHub> (engine, secondaries);
        rebindNetwork();

        masterPanel = std::make_unique<MasterPanel> (engine.mixer, engine.automation);
        masterPanel->onOpenAutomation = [this]
        {
            log ("painel de automacao ainda nao portado");
        };
        addAndMakeVisible (*masterPanel);

        pageButton.setButtonText ("PAGINA: --");
        pageButton.onClick = [this] { nextPage(); };
        addAndMakeVisible (pageButton);
        updatePageButton();

        cfgButton.setButtonText ("CONFIGURACOES");
        cfgButton.onClick = [this] { openDeviceWindow(); };
        addAndMakeVisible (cfgButton);

        testMode.setButtonText ("Modo de teste (nao envia de verdade)");
        testMode.setToggleState (true, juce::dontSendNotification);
        testMode.onClick = [this] { engine.mixer.automation.testMode.store (testMode.getToggleState()); };
        addAndMakeVisible (testMode);

        netLog.setMultiLine (true);
        netLog.setReadOnly (true);
        netLog.setFont (theme::mono (11.0f));
        netLog.setColour (juce::TextEditor::backgroundColourId, theme::oledBg);
        netLog.setColour (juce::TextEditor::textColourId, theme::oledDim);
        netLog.setColour (juce::TextEditor::outlineColourId, juce::Colours::black);
        addAndMakeVisible (netLog);

        buildStrips();
        setSize (1366, 768);
        startTimerHz (25);
    }

    ~MainComponent() override
    {
        // Sem esta linha, "sumiu" e "foi fechada" ficam indistinguiveis no log.
        logToFile ("=== mesa encerrada normalmente ===");
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);
        theme::drawPanel (g, chassis, theme::chassisTop, theme::chassisBot, 10.0f);

        g.setColour (theme::textDim);
        g.setFont (theme::mono (11.0f));
        g.drawText (statusText, statusArea, juce::Justification::centredLeft, false);

        // alerta vermelho: silencioso quando esta tudo bem, impossivel de
        // ignorar quando nao esta
        if (alertText.isNotEmpty())
        {
            g.setColour (theme::onRed);
            g.setFont (theme::mono (11.0f, true));
            g.drawText (alertText, statusArea, juce::Justification::centredRight, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        statusArea = r.removeFromTop (18);
        r.removeFromTop (4);
        chassis = r;

        auto inner = chassis.reduced (10);
        bridge.setBounds (inner.removeFromTop (92));
        inner.removeFromTop (8);

        auto bottom = inner.removeFromBottom (86);
        auto logArea = bottom.removeFromRight (430);
        auto topRow = bottom.removeFromTop (30);
        cfgButton .setBounds (topRow.removeFromLeft (200));
        topRow.removeFromLeft (8);
        pageButton.setBounds (topRow.removeFromLeft (200));
        testMode .setBounds (bottom.removeFromTop (26).removeFromLeft (300));
        netLog.setBounds (logArea);
        inner.removeFromBottom (8);

        masterPanel->setBounds (inner.removeFromRight (250));
        inner.removeFromRight (8);

        auto layerCol = inner.removeFromLeft (52);
        layerA->setBounds (layerCol.removeFromTop (layerCol.getHeight() / 2).withTrimmedBottom (3));
        layerB->setBounds (layerCol.withTrimmedTop (3));
        inner.removeFromLeft (6);

        const int n = juce::jmax (1, strips.size());
        const int w = inner.getWidth() / n;
        for (auto* s : strips) s->setBounds (inner.removeFromLeft (w).reduced (3, 0));
    }

private:
    void buildStrips()
    {
        strips.clear();
        const int first = layer * kFadersPerLayer;
        for (int i = 0; i < kFadersPerLayer; ++i)
        {
            const int g = first + i;
            if (g >= engine.mixer.numChannels()) break;

            auto* s = new ChannelStrip (engine.mixer.channel (g), g,
                                        [this] (int idx) { softPressed (idx); });
            s->onPressOnOff = [this] (int idx, bool on) { pressOnOff (idx, on); };
            strips.add (s);
            addAndMakeVisible (s);
        }
        layerA->setActive (layer == 0);
        layerB->setActive (layer == 1);
        resized();
    }

    void setLayer (int l)
    {
        if (l == layer) return;
        layer = l;
        buildStrips();
    }

    /** ON/OFF manda o comando SEMPRE, inclusive com o canal ja naquele estado:
        relancar o cartucho e acao explicita do operador. */
    void pressOnOff (int idx, bool on)
    {
        auto& ch = engine.mixer.channel (idx);
        ch.params.on.store (on);
        if (ch.params.logicEnabled.load())
            engine.automation.fireChannelLogic (engine.mixer, idx, on);
    }

    void softPressed (int idx)
    {
        if (idx < 0 || idx >= engine.mixer.numChannels()) return;

        auto* menu = new ChannelMenu (engine.mixer, engine.automation, settings, idx);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (menu);
        o.dialogTitle = "Canal " + juce::String (idx + 1);
        o.dialogBackgroundColour = theme::surface;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    /** Passa para a proxima pagina. Canal aberto nao troca de fonte: fica
        pendente, mesma regra do carregamento de cena. */
    void nextPage()
    {
        auto& ps = settings.pages;
        if (ps.pages.empty())
        {
            log ("nenhuma pagina cadastrada (Configuracoes > Paginas)");
            return;
        }
        ps.active = (ps.active + 1) % int (ps.pages.size());

        std::vector<int> pending;
        const int n = mesa::applyPage (ps.pages[size_t (ps.active)], settings.catalog,
                                       engine.mixer, &pending);
        updatePageButton();

        log ("pagina: " + juce::String (ps.pages[size_t (ps.active)].name)
             + (n > 0 ? "  |  " + juce::String (n) + " fader(es) no ar, pendente(s)" : ""));
        for (auto* s : strips) s->refresh();
    }

    void updatePageButton()
    {
        auto& ps = settings.pages;
        pageButton.setButtonText (ps.pages.empty()
            ? "PAGINA: --"
            : "PAGINA: " + juce::String (ps.pages[size_t (juce::jlimit (0, int (ps.pages.size()) - 1, ps.active))].name));
    }

    void openDeviceWindow()
    {
        auto* cfg = new ConfigComponent (settings, engine.mixer, engine.automation,
                                         engine.deviceManager, settingsFile, &secondaries);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (cfg);
        o.dialogTitle = "Configuracoes";
        cfg->onClosed = [this] { rebindNetwork(); };
        o.dialogBackgroundColour = theme::surface;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    /** Aplica um comando vindo de fora.

        Ponto delicado: ligar o canal aqui NAO redispara a logica de saida. O
        canal ao abrir manda DECK1_PLAY para a cartucheira; se ela devolvesse
        um ON, e nos reagissemos mandando de novo, o laco nao teria fim. Quem
        mandou o comando ja sabe o que fez. */
    void applyRemote (const CommandReceiver::Incoming& in)
    {
        const auto& c = in.cmd;
        if (! c.valid())
        {
            log ("comando ignorado de " + in.from + ": " + in.raw);
            return;
        }

        // globais primeiro: nao dependem de canal
        switch (c.action)
        {
            case mesa::RemoteCommand::Action::AutomationOff:
                engine.automation.suspended.store (true);
                log ("<- " + in.from + "  AUTOMACAO SUSPENSA (VT no ar)");
                return;
            case mesa::RemoteCommand::Action::AutomationOn:
                engine.automation.suspended.store (false);
                engine.automation.suspendUntilMs.store (0.0);
                log ("<- " + in.from + "  AUTOMACAO LIBERADA");
                return;
            case mesa::RemoteCommand::Action::AutomationHold:
            {
                const double ms = double (c.value) * 1000.0;
                engine.automation.suspendUntilMs.store (engine.automation.nowMs() + ms);
                log ("<- " + in.from + "  AUTOMACAO suspensa por "
                     + juce::String (c.value, 0) + " s");
                return;
            }
            default: break;
        }

        int idx = c.channel;
        if (idx < 0)
        {
            for (int i = 0; i < engine.mixer.numChannels(); ++i)
                if (juce::String (engine.mixer.channel (i).name)
                        .equalsIgnoreCase (juce::String (c.name)))
                { idx = i; break; }
        }
        if (idx < 0 || idx >= engine.mixer.numChannels())
        {
            log ("canal nao encontrado: " + in.raw);
            return;
        }

        auto& ch = engine.mixer.channel (idx);
        juce::String what;

        switch (c.action)
        {
            case mesa::RemoteCommand::Action::On:
            {
                // Se veio de uma PAUSE, volta exatamente para onde o fader
                // estava: pausar nao pode apagar o ajuste do operador.
                auto it = pausedFader.find (idx);
                if (it != pausedFader.end())
                {
                    ch.params.faderDb.store (it->second);
                    what << " fader -> " << juce::String (it->second, 1) << " dB (retomada)";
                    pausedFader.erase (it);
                }
                // Senao, o pedido tipico do playout: entrar no ar EM NIVEL,
                // sem depender de onde o fader ficou da ultima vez.
                else if (settings.remoteOnSetsFader
                         && ch.params.faderDb.load() < settings.remoteOnFaderDb)
                {
                    ch.params.faderDb.store (settings.remoteOnFaderDb);
                    what << " fader -> " << juce::String (settings.remoteOnFaderDb, 1) << " dB";
                }
                ch.params.on.store (true);
                what = "ON" + what;
                break;
            }
            case mesa::RemoteCommand::Action::Pause:
                pausedFader[idx] = ch.params.faderDb.load();
                ch.params.on.store (false);
                what = "PAUSE (fader guardado em "
                     + juce::String (pausedFader[idx], 1) + " dB)";
                break;
            case mesa::RemoteCommand::Action::Off:
                // OFF e fim: esquece a retomada, o proximo ON entra em nivel
                pausedFader.erase (idx);
                ch.params.on.store (false);
                what = "OFF";
                break;
            case mesa::RemoteCommand::Action::Fader:
                ch.params.faderDb.store (juce::jlimit (-60.0f, 10.0f, c.value));
                what = "fader " + juce::String (c.value, 1) + " dB";
                break;
            case mesa::RemoteCommand::Action::Cue:
                ch.params.cue.store (c.flag);
                what = c.flag ? "CUE on" : "CUE off";
                break;
            case mesa::RemoteCommand::Action::Mute:
                ch.params.mute.store (c.flag);
                what = c.flag ? "MUTE on" : "MUTE off";
                break;
            case mesa::RemoteCommand::Action::Trim:
                ch.params.trimDb.store (juce::jlimit (-25.0f, 25.0f, c.value));
                what = "trim " + juce::String (c.value, 1) + " dB";
                break;
            default: break;
        }

        log ("<- " + in.from + "  CH" + juce::String (idx + 1) + "  " + what);
        for (auto* st : strips) st->refresh();
    }

    /** Batimento: a fotografia que permite achar defeito de operacao longa.

        Cada linha traz o que cresce com o tempo. Um numero que sobe sem parar
        ao longo das horas aponta o vazamento; um que estabiliza esta saudavel.
        Sem isso, "fechou de madrugada" nao tem investigacao possivel. */
    void writeHeartbeat()
    {
        const auto now = juce::Time::getCurrentTime();
        const double upMin = (juce::Time::getMillisecondCounterHiRes() - startedMs) / 60000.0;

        juce::String l;
        l << "ativa " << juce::String (upMin, 1) << " min"
          << "  |  RAM " << juce::String (memoryMb(), 1) << " MB"
          << "  |  handles " << juce::String (handleCount())
          << "  |  threads " << juce::String (threadCount())
          << "  |  carga " << juce::String (engine.cpuLoad.load(), 1) << "%"
          << "  |  disp " << engine.deviceName
          << "  |  buf " << juce::String (engine.blockSize.load())
          << "  |  log " << juce::String (netLog.getTotalNumChars()) << " chars"
          << "  |  cmds " << juce::String (receiver.received())
          << "  |  secundarias " << juce::String (secondaries.count());

        for (int i = 0; i < secondaries.count(); ++i)
            if (auto* d = secondaries.at (i))
                l << "  |  " << d->deviceName() << " falhas " << juce::String (d->glitches())
                  << " quedas " << juce::String (d->dropouts())
                  << (d->isLost() ? " PERDIDA" : "");

        logToFile (l);

        // Alerta antecipado: se a memoria dobrar em relacao ao arranque, algo
        // esta vazando e vale saber ANTES de a mesa morrer.
        const double mb = memoryMb();
        if (baselineMb <= 0.0 && upMin > 2.0) baselineMb = mb;
        if (baselineMb > 0.0 && mb > baselineMb * 2.0 && ! warnedMemory)
        {
            warnedMemory = true;
            logToFile ("AVISO: memoria dobrou desde o arranque ("
                       + juce::String (baselineMb, 1) + " -> " + juce::String (mb, 1) + " MB)");
        }
    }

    static double memoryMb()
    {
       #if JUCE_WINDOWS
        PROCESS_MEMORY_COUNTERS pmc {};
        if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
            return double (pmc.WorkingSetSize) / (1024.0 * 1024.0);
       #endif
        return 0.0;
    }

    static int handleCount()
    {
       #if JUCE_WINDOWS
        DWORD n = 0;
        if (GetProcessHandleCount (GetCurrentProcess(), &n)) return int (n);
       #endif
        return 0;
    }

    static int threadCount()
    {
       #if JUCE_WINDOWS
        // conta as threads deste processo percorrendo o snapshot do sistema
        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 te {}; te.dwSize = sizeof (te);
        const DWORD me = GetCurrentProcessId();
        int n = 0;
        if (Thread32First (snap, &te))
            do { if (te.th32OwnerProcessID == me) ++n; } while (Thread32Next (snap, &te));
        CloseHandle (snap);
        return n;
       #endif
        return 0;
    }

    /** Log em arquivo. O log de tela some quando o processo morre, e queda de
        madrugada sem rastro e impossivel de investigar. */
    void logToFile (const juce::String& line)
    {
        if (logFile.getFullPathName().isEmpty()) return;
        logFile.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                            + "  " + line + "\n", false, false, "\n");
    }

    void log (const juce::String& line)
    {
        logToFile (line);

        // Corte do log de tela. Sem isso ele cresce sem limite: em 24 h de
        // operacao vira dezenas de megabytes de texto num TextEditor, e a mesa
        // vai ficando lenta ate morrer. Foi provavelmente o que aconteceu.
        if (netLog.getTotalNumChars() > 60000)
        {
            const auto keep = netLog.getText().getLastCharacters (20000);
            netLog.setText (keep, false);
        }
        netLog.moveCaretToEnd();
        netLog.insertTextAtCaret (line + "\n");
    }

    void rebindNetwork()
    {
        const double sr = engine.sampleRate.load();
        const int    bl = juce::jmax (32, engine.blockSize.load());
        hub->rebind        (settings.catalog, sr, bl);
        hub->rebindOutputs (settings.outputs, sr, bl);
    }

    void timerCallback() override
    {
        // eventos de perda e recuperacao vao para o log assim que acontecem
        for (const auto& e : secondaries.takeEvents())
        {
            log (e);
            if (e.startsWith ("PLACA PERDIDA") && ! warnedLostOnce)
            {
                warnedLostOnce = true;
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon, "Placa de audio perdida",
                    e + "\n\nOs canais dessa placa ficam em silencio. "
                        "A mesa tenta reconectar sozinha a cada 2 segundos.");
            }
        }

        for (const auto& l : pendingLog) log (l);
        pendingLog.clear();

        for (const auto& in : receiver.take()) applyRemote (in);

        const auto probs = hub->problems();
        alertText = probs.empty() ? juce::String()
                                  : juce::String (probs[0])
                                    + (probs.size() > 1
                                           ? "  (+" + juce::String (int (probs.size()) - 1) + ")"
                                           : "");

        statusText = juce::String ("v") + mesa::kVersion + "   |   "
                   + (openError.isEmpty() ? engine.deviceName : "ERRO: " + openError)
                   + "   |   " + juce::String (engine.sampleRate.load(), 0) + " Hz"
                   + "   |   buffer " + juce::String (engine.blockSize.load())
                   + "   |   latencia " + juce::String (engine.latencyMs.load(), 2) + " ms"
                   + "   |   carga " + juce::String (engine.cpuLoad.load(), 1) + " %"
                   + "   |   cam " + juce::String (engine.automation.camera())
                   + "   |   layer " + juce::String (layer == 0 ? "A" : "B")
                   + "   |   enviados " + juce::String (sender->sent.load())
                   + " / falhas " + juce::String (sender->failed.load());

        const int n = engine.mixer.numChannels();
        for (int i = 0; i < strips.size(); ++i)
        {
            const int g = layer * kFadersPerLayer + i;
            if (g < n) strips[i]->setTriggerState (engine.automation.stateOf (g));
            strips[i]->refresh();
        }

        bridge.repaint();
        masterPanel->repaint();
        repaint (statusArea);

        for (const auto& l : sender->takeLog())
            log (l);
    }

    void setupBenchDefaults()
    {
        auto& mic = engine.mixer.channel (0);
        mic.name = "MIC BANCADA";
        mic.params.sourceType.store (int (mesa::SourceType::Operator));
        mic.params.on.store (true);
        mic.params.faderDb.store (0.0f);
        mic.params.trigger.enabled.store (true);
        mic.params.trigger.camera.store (1);
        mic.params.trigger.thresholdDb.store (-35.0f);
        mic.params.trigger.target.store (0);

        if (engine.mixer.numChannels() > 1)
        {
            auto& deck = engine.mixer.channel (1);
            deck.name = "CARTUCHEIRA";
            deck.params.sourceType.store (int (mesa::SourceType::ComputerPlayer));
            deck.params.faderDb.store (0.0f);
            deck.params.logicEnabled.store (true);
            deck.params.logicTarget.store (1);
            deck.params.onCommand .set ("DECK1_PLAY");
            deck.params.offCommand.set ("DECK1_PAUSE");
        }

        engine.mixer.automation.enabled.store (true);
        engine.mixer.automation.testMode.store (true);
        engine.mixer.automation.wideCamera.store (5);
    }

    AudioEngine engine;
    MeterBridge bridge;
    mesa::Settings settings;
    juce::File settingsFile, sceneFile;
    juce::String openError, statusText;
    std::unique_ptr<CommandSender> sender;
    juce::OwnedArray<ChannelStrip> strips;
    std::unique_ptr<SurfaceButton> layerA, layerB;
    std::unique_ptr<MasterPanel> masterPanel;
    juce::TextButton cfgButton, pageButton;
    juce::ToggleButton testMode;
    juce::TextEditor netLog;
    juce::Rectangle<int> chassis, statusArea;
    SecondaryDevices secondaries;
    CommandReceiver receiver;
    juce::StringArray pendingLog;
    juce::File logFile;

    /** Timer separado para o batimento, para nao misturar com o de 25 Hz. */
    struct Heartbeat : juce::Timer
    {
        std::function<void()> onTimer;
        void timerCallback() override { if (onTimer) onTimer(); }
    };
    Heartbeat heartbeat;
    double startedMs = 0.0, baselineMb = 0.0;
    bool warnedMemory = false;
    /** Fader guardado por PAUSE, para o PLAY seguinte retomar no mesmo ponto. */
    std::map<int, float> pausedFader;
    std::unique_ptr<NetworkHub> hub;
    juce::String alertText;
    bool warnedLostOnce = false;
    int layer = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
