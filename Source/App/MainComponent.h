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
#include "GpioClient.h"
#include "SpeechWriter.h"
#include "Recorder.h"
#include "../Core/Rastro.h"
#include "SpeechStreamer.h"
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
        mesa::rastro ("componente: inicio");
        // ONDE a configuracao mora.
        //
        // Ate aqui ela ficava ao lado do exe, dentro de build/ — pasta
        // descartavel, que some a cada reconfiguracao do CMake. Levava junto
        // nomes de canal, thresholds, holds, cameras: tudo que o operador
        // ajustou. Configuracao nao pode morar em pasta de compilacao.
        //
        // Agora fica na area de dados do usuario, que sobrevive a build limpo,
        // a troca de versao e ate a apagar o projeto inteiro.
        auto pasta = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("MesaConsole");
        pasta.createDirectory();

        mesa::rastro ("componente: pastas");
        settingsFile = pasta.getChildFile ("settings.json");
        sceneFile    = pasta.getChildFile ("scene.json");

        // Migracao: se houver configuracao antiga ao lado do exe e ainda nao
        // houver na pasta nova, traz junto — ninguem perde o que ja ajustou.
        auto antigaPasta = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                              .getParentDirectory();
        for (juce::File* par : { &settingsFile, &sceneFile })
        {
            auto antiga = antigaPasta.getChildFile (par->getFileName());
            if (! par->existsAsFile() && antiga.existsAsFile())
                antiga.copyFileTo (*par);
        }

        if (settingsFile.existsAsFile())
            mesa::settingsFromJson (settingsFile.loadFileAsString().toStdString(), settings);
        else
            settingsFile.replaceWithText (mesa::settingsToJson (settings));

        mesa::rastro ("componente: abrindo audio ("
                      + (settings.deviceState.empty() ? juce::String ("padrao")
                                                      : juce::String ("estado salvo)")));
        auto err = engine.start (numChannels, 8, juce::String (settings.deviceState));
        openError = err;

        if (settings.deviceState.empty())
            pendingLog.add ("sem placa escolhida: abrindo o audio do Windows. "
                            "Para menor latencia, escolha o ASIO em Sistema.");

        if (engine.placaSalvaAusente.isNotEmpty())
            pendingLog.add ("placa salva nao existe nesta maquina: \""
                            + engine.placaSalvaAusente
                            + "\" — abrindo a padrao. Confira em Sistema.");

        mesa::rastro ("componente: roteamento");
        mesa::applyRouting (settings, engine.mixer);
        mesa::rastro ("componente: comandos");
        sender = std::make_unique<CommandSender> (engine.automation, settings);

        mesa::rastro ("componente: cena");
        mesa::Scene scene;
        if (sceneFile.existsAsFile()
            && mesa::sceneFromJson (sceneFile.loadFileAsString().toStdString(), scene))
            mesa::applyScene (scene, engine.mixer, false);
        else
        {
            setupBenchDefaults();
            sceneFile.replaceWithText (mesa::sceneToJson (mesa::captureScene (engine.mixer, "BANCADA")));
            logToFile ("primeira execucao: cena de fabrica criada");
        }

        mesa::rastro ("componente: interface");
        addAndMakeVisible (bridge);

        layerA = std::make_unique<SurfaceButton> ("A", theme::busGreen, 28.0f);
        layerB = std::make_unique<SurfaceButton> ("B", theme::busGreen, 28.0f);
        layerA->setSub ("LAYER\n1-8");
        layerB->setSub ("LAYER\n9-16");
        layerA->onClick = [this] { setLayer (0); };
        layerB->onClick = [this] { setLayer (1); };
        addAndMakeVisible (*layerA);
        addAndMakeVisible (*layerB);

        theme::tally().onAir = juce::Colour (settings.tallyOnAir);
        theme::tally().armed = juce::Colour (settings.tallyArmed);
        theme::tally().wait  = juce::Colour (settings.tallyWait);
        theme::tally().idle  = juce::Colour (settings.tallyIdle);

        logFile = settingsFile.getSiblingFile ("mesa.log");
        startedMs = juce::Time::getMillisecondCounterHiRes();
        logToFile ("configuracao em: " + settingsFile.getParentDirectory().getFullPathName());
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

        iniciaGpio();
        mesa::rastro ("iniciando a fala");
        iniciaFala();

        // o gravador recebe direto do callback; a fila e dele
        engine.aoGravar = [this] (const float* x, int n) { gravador.alimenta (x, n); };

        mesa::rastro ("placas secundarias");
        secondaries.setLatencyMode (settings.secondaryLatencyMode);
        mesa::rastro ("criando o hub de rede");
        hub = std::make_unique<NetworkHub> (engine, secondaries);
        hub->setPlacaLivewire (juce::String (settings.livewirePlaca));
        hub->setTipoDeCarga (settings.livewireCarga);
        hub->aoRegistrar = [this] (const juce::String& m) { pendingLog.add (m); };
        mesa::rastro ("ligando a rede");
        rebindNetwork();

        masterPanel = std::make_unique<MasterPanel> (engine.mixer, engine.automation);
        masterPanel->onOpenAutomation = [this]
        {
            log ("painel de automacao ainda nao portado");
        };
        addAndMakeVisible (*masterPanel);

        // ---- barra lateral de janela
        //
        // Sem barra de titulo do sistema, a mesa precisa oferecer as tres
        // acoes que o operador ainda vai querer: encolher para olhar outra
        // coisa, sair do modo tela cheia, e fechar. Ficam numa faixa estreita
        // na borda, longe dos faders — nao se aperta sem querer.
        // simbolos de janela padrao: traco, quadrado e X — os mesmos do Windows
        janelaMin = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xe2\x80\x93"), theme::textDim, 16.0f);
        janelaMin->onClick = [this]
        {
            // Sai da tela cheia ANTES de encolher.
            //
            // Em modo quiosque o pedido de minimizar era ignorado e a janela
            // se reexpandia — o botao parecia fazer o contrario do que diz.
            auto& d = juce::Desktop::getInstance();
            if (d.getKioskModeComponent() != nullptr)
            {
                d.setKioskModeComponent (nullptr, false);
                if (janelaTela != nullptr) janelaTela->setActive (false);
            }

            if (auto* peer = getPeer()) peer->setMinimised (true);
        };
        addAndMakeVisible (*janelaMin);

        janelaTela = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xe2\x96\xa1"), theme::textDim, 15.0f);
        janelaTela->setActive (true);
        janelaTela->onClick = [this]
        {
            auto& d = juce::Desktop::getInstance();
            const bool estaCheia = d.getKioskModeComponent() != nullptr;
            d.setKioskModeComponent (estaCheia ? nullptr : getTopLevelComponent(), false);
            janelaTela->setActive (! estaCheia);
        };
        addAndMakeVisible (*janelaTela);

        // ---- transporte de gravacao
        //
        // Simbolos e nao palavras: circulo, duas barras e quadrado sao lidos
        // sem traducao, e a coluna e estreita demais para texto.
        recRec = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xe2\x97\x8f"),
                                                  theme::onRed, 17.0f);
        recRec->onClick = [this] { alternaGravacao (canalParaGravar()); };
        addAndMakeVisible (*recRec);

        recPause = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xe2\x9d\x9a"),
                                                    theme::prev, 15.0f);
        recPause->onClick = [this]
        {
            if (! gravador.estaGravando()) return;
            gravador.pausa (! gravador.estaPausado());
            recPause->setActive (gravador.estaPausado());
            log (gravador.estaPausado() ? "gravacao pausada" : "gravacao retomada");
        };
        addAndMakeVisible (*recPause);

        recStop = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xe2\x96\xa0"),
                                                   juce::Colour (0xff8a9099), 15.0f);
        recStop->onClick = [this] { if (gravador.estaGravando()) alternaGravacao (-1); };
        addAndMakeVisible (*recStop);

        janelaSair = std::make_unique<SurfaceButton> (juce::String::fromUTF8 ("\xc3\x97"), theme::onRed, 17.0f);
        janelaSair->onClick = [this]
        {
            // confirma: fechar a mesa no meio do ar por clique errado seria
            // o pior defeito possivel de interface
            juce::NativeMessageBox::showOkCancelBox (
                juce::MessageBoxIconType::WarningIcon, "Sair da mesa",
                "Fechar o Mesa Console? O audio para.",
                nullptr,
                juce::ModalCallbackFunction::create ([] (int r)
                {
                    if (r == 1) juce::JUCEApplication::getInstance()->systemRequestedQuit();
                }));
        };
        addAndMakeVisible (*janelaSair);

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

        // Painel da transcricao, no lugar do log.
        //
        // O log tecnico foi para as configuracoes: ele serve para investigar
        // defeito, e defeito se investiga sentado. O que interessa a quem esta
        // operando e o que esta sendo DITO no ar — e isso merece a tela.
        painelFala.setMultiLine (true);
        painelFala.setReadOnly (true);
        painelFala.setFont (theme::mono (12.0f));
        painelFala.setColour (juce::TextEditor::backgroundColourId, theme::oledBg);
        painelFala.setColour (juce::TextEditor::textColourId, theme::oled);
        painelFala.setColour (juce::TextEditor::outlineColourId, juce::Colours::black);
        addAndMakeVisible (painelFala);

        // Estado bem a vista: transcricao que caiu em silencio e pior que
        // transcricao desligada — o operador confia num texto que parou de
        // chegar sem ninguem avisar.
        estadoFala.setFont (theme::mono (11.0f, true));
        estadoFala.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (estadoFala);

        netLog.setMultiLine (true);
        netLog.setReadOnly (true);
        netLog.setFont (theme::mono (11.0f));
        netLog.setColour (juce::TextEditor::backgroundColourId, theme::oledBg);
        netLog.setColour (juce::TextEditor::textColourId, theme::oledDim);
        netLog.setColour (juce::TextEditor::outlineColourId, juce::Colours::black);
        addAndMakeVisible (netLog);

        buildStrips();
        // Resolucao de referencia: 1920x1080. O layout e proporcional, entao
        // ele acompanha janela maior ou menor — mas e nesta medida que as
        // proporcoes foram pensadas.
        // Tamanho que CABE na tela desta maquina.
        //
        // Era 1920x1080 fixo. Em monitor menor a janela nascia maior que a
        // tela: o rodape e os botoes de janela ficavam fora, e nao havia como
        // alcanca-los. A mesa e desenhada para 1920x1080 e continua sendo —
        // aqui so garantimos que ela nao ultrapasse o que existe.
        {
            const auto area = juce::Desktop::getInstance().getDisplays()
                                  .getPrimaryDisplay() != nullptr
                            ? juce::Desktop::getInstance().getDisplays()
                                  .getPrimaryDisplay()->userArea
                            : juce::Rectangle<int> (0, 0, 1920, 1080);

            setSize (juce::jmin (1920, area.getWidth()),
                     juce::jmin (1080, area.getHeight()));
        }
        mesa::rastro ("componente: ligando o relogio");
        startTimerHz (25);
    }

    /** Grava cena e configuracoes.

        A cena guarda o que o operador ajusta no dia a dia: nome do canal,
        threshold, hold, camera, buses, fader. Ate aqui ela era escrita UMA vez,
        no primeiro arranque, e nunca mais — tudo que fosse ajustado depois
        morria ao fechar a mesa. Nao era problema de "versao nova": era em todo
        reinicio.

        Guarda o arquivo anterior como .bak antes de sobrescrever. Se a mesa cair
        no meio da escrita, o ajuste de ontem continua recuperavel. */
    void salvarEstado (const char* motivo)
    {
        if (sceneFile.getFullPathName().isEmpty()) return;

        const auto json = mesa::sceneToJson (mesa::captureScene (engine.mixer, "ATUAL"));
        if (json.size() < 32) return;                 // nunca grava lixo por cima

        if (sceneFile.existsAsFile())
            sceneFile.copyFileTo (sceneFile.getSiblingFile ("scene.json.bak"));
        sceneFile.replaceWithText (json);

        // a placa faz parte do estado: sem isto ela volta ao padrao do Windows
        settings.deviceState = engine.estadoAtual().toStdString();

        const auto cfg = mesa::settingsToJson (settings);
        if (cfg.size() > 32)
        {
            if (settingsFile.existsAsFile())
                settingsFile.copyFileTo (settingsFile.getSiblingFile ("settings.json.bak"));
            settingsFile.replaceWithText (cfg);
        }
        logToFile (juce::String ("estado salvo (") + motivo + ")");
    }

    ~MainComponent() override
    {
        salvarEstado ("fechando");

        // Desligar na ordem certa, e cedo. O crash de saida vinha daqui: o
        // processo terminava com threads de rede ainda vivas e bibliotecas
        // ja descarregadas. Nada disso e opcional no encerramento.
        heartbeat.stopTimer();
        stopTimer();
        receiver.stop();
        gpio.stop();
        escritorFala.stop();
        if (transcritor.isRunning()) transcritor.kill();
        fluxoFala.stop();
        gravador.stop();
        if (hub != nullptr) hub->shutdown();
        secondaries.closeAll();
        NdiEngine::instance().shutdown();
        logToFile ("desligamento ordenado concluido");
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


        // Botoes de janela no canto superior DIREITO, em linha.
        //
        // Estavam empilhados na vertical a esquerda, com a ordem invertida e
        // simbolos improvisados. Todo programa do Windows poe minimizar,
        // maximizar e fechar nessa ordem, deitados, no alto a direita — e a
        // mao do operador ja vai ali sem pensar. Inventar posicao propria em
        // mesa de ar custa segundos que nao existem.
        auto linhaTopo = inner.removeFromTop (84);

        auto faixaBotoes = linhaTopo.removeFromTop (26);
        const int largBotaoJanela = 44;
        janelaSair->setBounds (faixaBotoes.removeFromRight (largBotaoJanela));
        janelaTela->setBounds (faixaBotoes.removeFromRight (largBotaoJanela));
        janelaMin ->setBounds (faixaBotoes.removeFromRight (largBotaoJanela));

        bridge.setBounds (linhaTopo);
        inner.removeFromTop (8);

        // COLUNA DA DIREITA: painel master em cima, controles e log embaixo.
        //
        // Antes havia uma faixa inferior atravessando a mesa inteira, so para
        // dois botoes, uma caixa de marcar e o log. Ela roubava altura das
        // tiras — e altura de tira e curso de fader, que e o que mais importa
        // numa mesa. Tudo isso cabe folgado sob o painel de automacao.
        auto colDireita = inner.removeFromRight (300);
        inner.removeFromRight (8);

        auto rodape = colDireita.removeFromBottom (190);
        colDireita.removeFromBottom (8);
        masterPanel->setBounds (colDireita);

        auto linhaBotoes = rodape.removeFromTop (28);
        cfgButton .setBounds (linhaBotoes.removeFromLeft (145));
        linhaBotoes.removeFromLeft (6);
        pageButton.setBounds (linhaBotoes);
        rodape.removeFromTop (4);

        testMode.setBounds (rodape.removeFromTop (22));
        rodape.removeFromTop (4);
        // com transcricao ligada, a tela mostra a fala; senao, o log
        const bool mostraFala = settings.falaTempoReal;
        painelFala.setVisible (mostraFala);
        netLog.setVisible (! mostraFala);
        if (mostraFala)
        {
            estadoFala.setVisible (true);
            estadoFala.setBounds (rodape.removeFromTop (18));
            rodape.removeFromTop (2);
        }
        else estadoFala.setVisible (false);

        (mostraFala ? painelFala : netLog).setBounds (rodape);

        auto layerCol = inner.removeFromLeft (64);

        // transporte de gravacao no pe da coluna esquerda
        auto transporte = layerCol.removeFromBottom (40);
        const int lb = (transporte.getWidth() - 4) / 3;
        recRec  ->setBounds (transporte.removeFromLeft (lb));
        transporte.removeFromLeft (2);
        recPause->setBounds (transporte.removeFromLeft (lb));
        transporte.removeFromLeft (2);
        recStop ->setBounds (transporte);
        layerCol.removeFromBottom (6);

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
            s->onRec = [this] (int idx) { alternaGravacao (idx); };
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
        menu->aoGravar = [this] (int canal) { alternaGravacao (canal); };
        menu->aoConsultarGravacao = [this] { return gravador.estaGravando(); };
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
        configOpen = true;
        cfg->onLivewirePlaca = [this] (const juce::String& ip)
        {
            // o proprio setPlacaLivewire ja derruba o que estava aberto na
            // placa antiga; aqui so religamos
            hub->setPlacaLivewire (ip);
            log ("placa do Livewire: " + ip + " (deduzida da varredura)");
            rebindNetwork();
        };
        // avisa tambem ao arrancar: quem herda uma configuracao pronta nunca
        // abre a aba onde o aviso aparece
        if (const auto c = mesa::conflitosDeSaida (settings); ! c.empty())
            pendingLog.add ("ATENCAO conflito de saida: " + juce::String (c));

        cfg->onClosed = [this]
        {
            configOpen = false;
            rebindNetwork();
            salvarEstado ("configuracoes fechadas");
        };
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
            case mesa::RemoteCommand::Action::Texto:
            {
                // LIMITE DURO no tamanho.
                //
                // O transcritor chegou a mandar o paragrafo inteiro do programa
                // a cada duas frases, crescendo sem parar. Gravar e desenhar
                // isso segurou a mesa a ponto de o AUDIO travar — e mesa que
                // para o som nao vai ao ar. Nenhuma frase falada legitima passa
                // de 400 caracteres; o que passa disso e defeito de quem
                // enviou, e a mesa nao pode afundar por causa dele.
                juce::String texto (c.name);
                if (texto.length() > 400)
                {
                    texto = texto.substring (texto.length() - 400);
                    ++falasCortadas;
                }

                log ("FALA: " + texto);
                gravaTranscricao (texto);
                mostraFala (texto);
                return;
            }
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
                ch.params.faderDb.store (juce::jlimit (theme::kFaderBottomDb, theme::kFaderTopDb, c.value));
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
          // Para localizar a perda: comparando o barramento com o que sai da
          // placa, o degrau aparece sozinho. Sem os dois numeros lado a lado,
          // "esta baixo" nao diz se a perda esta no canal, no barramento, no
          // master ou depois.
          << "  |  PGM1 bus "
          << juce::String (engine.mixer.masterMeterL.peakDb(), 1) << " dBFS"
          << "  master " << juce::String (settings.routing.masterGainDb, 1) << " dB"
          << "  |  saidas";
        {
            const int n = juce::jmin (engine.canaisSaida.load(), 8);
            for (int ch = 0; ch < n; ++ch)
                l << " " << juce::String (ch + 1) << ":"
                  << juce::String (20.0f * std::log10 (juce::jmax (1.0e-6f,
                          engine.picoCanal[size_t (ch)].load())), 0);
        }
        l
          << "  |  cmds " << juce::String (receiver.received())
          << (settings.falaTempoReal
                  ? "  |  fala " + juce::String (fluxoFala.conectado() ? "ON" : "OFF")
                    + " " + juce::String (linhasTranscritas) + " linhas "
                    + juce::String (double (fluxoFala.amostrasEnviadas()) / 16000.0, 0) + "s"
                    + (fluxoFala.descartes() > 0
                           ? "  DESCARTES " + juce::String (fluxoFala.descartes())
                           : juce::String())
                    + (falasCortadas > 0
                           ? "  CORTADAS " + juce::String (falasCortadas)
                           : juce::String())
                  : juce::String())
          << (hub != nullptr ? hub->estadoLivewire() + hub->estadoNdi() : juce::String())
          << (hub != nullptr && hub->anunciosEnviados() > 0
                  ? "  |  anuncios " + juce::String (hub->anunciosEnviados())
                  : juce::String())
          << "  |  secundarias " << juce::String (secondaries.count());

        // Dois faders carregando o MESMO input entregam o mesmo sinal duas
        // vezes ao bus: soma 6 dB e parece defeito de audio. Nao proibimos —
        // as vezes e proposital — mas avisamos uma vez.
        if (! avisouDuplicado)
        {
            const int n = engine.mixer.numChannels();
            for (int a = 0; a < n && ! avisouDuplicado; ++a)
            {
                const auto& na = engine.mixer.channel (a).name;
                if (na.empty()) continue;
                for (int b = a + 1; b < n; ++b)
                    if (engine.mixer.channel (b).name == na)
                    {
                        avisouDuplicado = true;
                        log ("ATENCAO: o input \"" + juce::String (na) + "\" esta em dois "
                             "faders (CH" + juce::String (a + 1) + " e CH" + juce::String (b + 1)
                             + ") — o sinal soma duas vezes no bus");
                        break;
                    }
            }
        }

        // fila mal dimensionada corrompe memoria em silencio: se aparecer, tem
        // que gritar no log
        for (int i = 0; i < AudioEngine::kMaxNetSlots; ++i)
            if (auto* q = engine.netSlot[size_t (i)].load())
                if (q->badPulls() > 0 && ! avisouFila)
                {
                    avisouFila = true;
                    log ("ALERTA: fila de rede pediu mais do que comporta ("
                         + juce::String (q->badPulls()) + "x) — avise o desenvolvedor");
                }

        for (int i = 0; i < secondaries.count(); ++i)
            if (auto* d = secondaries.at (i))
                l << "  |  " << d->deviceName()
                  << " " << juce::String (int (d->sampleRate())) << "Hz/"
                  << juce::String (int (d->amostrasPorSegundo())) << "real"
                  << " bloco " << juce::String (d->blocoDoDispositivo())
                  << " ch " << juce::String (d->canaisAtivos())
                  << (d->recebendo() ? (" recebendo " + juce::String (d->blocos()) + " blocos")
                                     : juce::String (" SEM AUDIO (abriu mas nao entrega)"))
                  << " buracos " << juce::String (d->buracos())
                  << " faltas " << juce::String (d->faltas())
                  << " descartes " << juce::String (d->descartes())
                  << " quedas " << juce::String (d->dropouts())
                  << (d->isLost() ? " PERDIDA" : "");

        logToFile (l);

        // salva junto do batimento: se a mesa cair, perde-se no maximo um
        // minuto de ajuste em vez de um dia inteiro
        salvarEstado ("batimento");

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
    /** Arquivo proprio para a transcricao, um por dia.

        Separado do mesa.log de proposito: o log da mesa serve para investigar
        defeito e e cortado quando cresce; a transcricao e material de estudo —
        e dela que sai a resposta sobre com que frequencia um VT seria acionado
        por voz e quais expressoes o locutor usa de verdade. Misturar as duas
        coisas perde as duas. */
    /** Acrescenta a fala ao painel da tela, com hora. */
    void mostraFala (const juce::String& texto)
    {
        const auto agora = juce::Time::getCurrentTime();
        // Acrescenta no fim, sem reescrever o texto inteiro: reconstruir a
        // caixa a cada frase fica caro conforme ela cresce, e isso roda na
        // mesma thread que desenha a mesa.
        painelFala.moveCaretToEnd();
        painelFala.insertTextAtCaret (agora.formatted ("%H:%M:%S") + "  " + texto + "\n");

        // Aparar so de vez em quando: ler o texto inteiro para medir o
        // tamanho custa caro quando a frase chega dez vezes por segundo.
        if (++desdeUltimaApara >= 50)
        {
            desdeUltimaApara = 0;
            const auto t = painelFala.getText();
            if (t.length() > 12000)
            {
                painelFala.setText (t.substring (t.length() - 8000), false);
                painelFala.moveCaretToEnd();
            }
        }
    }

    void gravaTranscricao (const juce::String& texto)
    {
        if (texto.trim().isEmpty()) return;

        const auto agora = juce::Time::getCurrentTime();
        const auto dia = agora.formatted ("%Y-%m-%d");

        // Arquivo ABERTO, nao reaberto a cada frase.
        //
        // Com o texto chegando de dez em dez palavras, abrir e fechar o
        // arquivo a cada linha vira dezenas de acessos a disco por segundo —
        // na mesma thread que desenha a mesa. Foi o que a travou.
        if (arquivoFala == nullptr || diaDaFala != dia)
        {
            auto pasta = settingsFile.getParentDirectory().getChildFile ("transcricoes");
            pasta.createDirectory();
            arquivoFala = std::make_unique<juce::FileOutputStream> (
                              pasta.getChildFile (dia + ".txt"));
            // o fluxo ja abre no fim do arquivo; nao ha o que posicionar
            diaDaFala = dia;
        }

        if (arquivoFala != nullptr)
        {
            arquivoFala->writeText (agora.formatted ("%H:%M:%S") + "  " + texto + "\n",
                                    false, false, "\n");
            if (++desdeUltimoFlush >= 20) { arquivoFala->flush(); desdeUltimoFlush = 0; }
        }
        ++linhasTranscritas;
    }

    void logToFile (const juce::String& line)
    {
        if (logFile.getFullPathName().isEmpty()) return;
        logFile.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                            + "  " + line + "\n", false, false, "\n");
    }

    /** Registra mudanca de estado de cada trigger.

        O log so mostrava o comando SAINDO. Quando nada saia, nao havia como
        saber se o trigger nem chegou a candidato, se ficou preso no cooldown
        ou se disparou e o modo de teste barrou. Agora a sequencia inteira
        aparece, com o nivel do momento. */
    void logTriggerChanges()
    {
        const int n = engine.mixer.numChannels();
        if (int (lastTrigState.size()) != n) lastTrigState.assign (size_t (n), -1);

        for (int i = 0; i < n; ++i)
        {
            auto& ch = engine.mixer.channel (i);
            if (! ch.params.trigger.enabled.load()) continue;

            const int st = int (engine.automation.stateOf (i));
            if (st == lastTrigState[size_t (i)]) continue;
            lastTrigState[size_t (i)] = st;

            const auto tap = mesa::TapPoint (ch.params.trigger.source.load());
            log ("CH" + juce::String (i + 1) + "  trigger -> "
                 + mesa::triggerStateName (mesa::TriggerState (st))
                 + "   (nivel " + juce::String (ch.tapDb (tap), 1)
                 + " / threshold " + juce::String (ch.params.trigger.thresholdDb.load(), 1)
                 + " dBFS)");
        }
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

    void iniciaFala()
    {
        if (! settings.falaEnabled) return;
        const auto pasta = settings.falaPasta.empty()
                             ? settingsFile.getParentDirectory().getChildFile ("fala")
                             : juce::File (settings.falaPasta);
        escritorFala.start (pasta, engine.sampleRate.load());
        pendingLog.add ("transcricao: trechos em " + pasta.getFullPathName());
    }

    /** Recolhe trechos prontos dos canais e manda gravar. */
    void recolheFala()
    {
        if (! settings.falaEnabled) return;

        for (int i = 0; i < engine.mixer.numChannels(); ++i)
        {
            auto& ch = engine.mixer.channel (i);
            if (! ch.temFalaPronta()) continue;

            const auto& t = ch.trechoDeFala();
            escritorFala.enfileira (t);
            log ("fala CH" + juce::String (i + 1) + ": trecho de "
                 + juce::String (1000.0 * t.size() / engine.sampleRate.load() / 1000.0, 1)
                 + " s gravado");
            ch.limpaTrechoDeFala();
            ch.marcaFalaRecolhida();
        }
    }

    void iniciaGpio()
    {
        if (! settings.gpioEnabled) return;

        const juce::String no = settings.gpioNode.empty()
                                    ? juce::String (settings.livewireNode)
                                    : juce::String (settings.gpioNode);
        if (no.isEmpty()) { pendingLog.add ("GPIO: sem endereco do no"); return; }

        gpio.start (no, [this] (GpioClient::Evento e)
        {
            // vem da thread de rede: so enfileira, aplica no timer
            std::lock_guard<std::mutex> g (mutexGpio);
            entradasGpio.push_back (e);
        });
        pendingLog.add ("GPIO ligado em " + no + ":93");
    }

    /** Entrada do no liga ou desliga o canal mapeado. */
    void aplicaEntradasGpio()
    {
        std::vector<GpioClient::Evento> lote;
        {
            std::lock_guard<std::mutex> g (mutexGpio);
            lote.swap (entradasGpio);
        }

        for (const auto& e : lote)
            for (int i = 0; i < engine.mixer.numChannels(); ++i)
            {
                const auto* def = settings.catalog.find (engine.mixer.channel (i).name);
                if (def == nullptr) continue;
                if (def->gpiPorta != e.porta || def->gpiPino != e.pino) continue;

                pressOnOff (i, e.fechado);
                log ("GPI porta " + juce::String (e.porta) + " pino " + juce::String (e.pino)
                     + (e.fechado ? " fechou" : " abriu") + " -> CH" + juce::String (i + 1)
                     + (e.fechado ? " ON" : " OFF"));
            }
    }

    /** ON/OFF do canal aciona a saida mapeada: luz de ar, rele, tally. */
    void atualizaSaidasGpio()
    {
        if (! settings.gpioEnabled || ! gpio.conectado()) return;

        const int n = engine.mixer.numChannels();
        if (int (ultimoOn.size()) != n) ultimoOn.assign (size_t (n), -1);

        for (int i = 0; i < n; ++i)
        {
            const auto* def = settings.catalog.find (engine.mixer.channel (i).name);
            if (def == nullptr || def->gpoPorta <= 0 || def->gpoPino <= 0) continue;

            const int agora = engine.mixer.channel (i).params.on.load() ? 1 : 0;
            if (agora == ultimoOn[size_t (i)]) continue;   // so na borda
            ultimoOn[size_t (i)] = agora;

            gpio.setPino (def->gpoPorta, def->gpoPino, agora == 1);
            log ("CH" + juce::String (i + 1) + (agora ? " ON" : " OFF")
                 + " -> GPO porta " + juce::String (def->gpoPorta)
                 + " pino " + juce::String (def->gpoPino)
                 + (agora ? " FECHA" : " ABRE"));
        }
    }

    /** Leva de volta ao fader o que mudou no catalogo e NAO e ajuste do
        operador.

        Marcar "transcrever" no input e nao ver efeito nenhum ate recarregar o
        fader na mao e o tipo de detalhe que faz o operador achar que a funcao
        esta quebrada. Cuidado ao ampliar isto: fader, ON/OFF, bus e CUE sao do
        operador e NAO podem ser sobrescritos pelo catalogo. */
    void sincronizaDoCatalogo()
    {
        for (int i = 0; i < engine.mixer.numChannels(); ++i)
        {
            auto& ch = engine.mixer.channel (i);
            if (const auto* def = settings.catalog.find (ch.name))
            {
                ch.params.fala.enabled    .store (def->transcrever);
                ch.params.fala.thresholdDb.store (def->falaThresholdDb);
                ch.params.fala.maxTrechoMs.store (def->falaMaxTrechoMs);

                // Input existe, mas ninguem escolheu de onde vem o audio.
                const bool nada = def->index < 0
                               && def->streamName.empty()
                               && def->deviceName.empty()
                               && def->livewireChannel == 0;
                ch.params.semFonte.store (nada);

                // REAPONTA O CANAL para a posicao atual da fonte.
                //
                // O canal guarda um numero de POSICAO na lista de fontes de
                // rede, e essa lista e remontada a cada rearranjo. Ao tirar uma
                // fonte, as seguintes andam uma casa para tras — e o canal, que
                // guardou o numero antigo, passa a tocar a fonte do vizinho.
                // Foi assim que o Livewire de outra maquina apareceu no canal
                // de um Livewire desligado.
                ch.params.inputKind .store (def->kind);
                ch.params.inputIndex.store (nada ? -1 : def->index);
            }
            else
                ch.params.semFonte.store (! ch.name.empty());
        }
    }

    /** Escolhe o canal da transcricao continua e liga o fluxo.

        Precisa rodar DEPOIS de o catalogo ser aplicado aos canais: no arranque
        a marca "transcrever" ainda nao chegou aos faders, e procurar por ela
        antes disso nao encontra nada — era por isso que o fluxo nunca subia. */
    /** Sobe o transcritor como processo FILHO.

        Do ponto de vista de quem opera, a transcricao vive dentro da mesa:
        abre e funciona. Por dentro continua sendo outro processo, e isso nao e
        detalhe — WebSocket com TLS dentro do processo de audio e o tipo de
        coisa que ja derrubou esta mesa duas vezes com o NDI. Aqui, se a API
        falhar ou o transcritor travar, o ar nao sente. */
    void iniciaTranscritor()
    {
        if (! settings.falaTempoReal) return;
        if (transcritor.isRunning()) return;

        auto script = settings.falaScript.empty()
            ? juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                  .getParentDirectory().getChildFile ("transcritor_tempo_real.py")
            : juce::File (settings.falaScript);

        if (! script.existsAsFile())
        {
            log ("transcritor nao encontrado em " + script.getFullPathName());
            return;
        }

        juce::StringArray cmd;
        cmd.add (settings.falaPython.empty() ? "python" : settings.falaPython);
        cmd.add (script.getFullPathName());
        cmd.add ("--chave-da-mesa");
        cmd.add ("--porta-audio");
        cmd.add (juce::String (settings.falaPortaAudio));

        // SEM pedir a saida do processo.
        //
        // Pedir e nao ler foi o que travou a mesa: o tubo enche, o transcritor
        // trava ao imprimir, para de ler o audio, e a escrita da mesa no socket
        // fica presa esperando ele consumir. Um bloqueio puxa o outro. A saida
        // dele so servia para o terminal, que agora nem existe.
        if (transcritor.start (cmd, 0))
            log ("transcritor iniciado pela mesa");
        else
            log ("nao consegui iniciar o transcritor — confira Python no sistema");
    }

    void atualizaTranscricao()
    {
        if (! settings.falaTempoReal) return;
        iniciaTranscritor();

        for (int i = 0; i < engine.mixer.numChannels(); ++i)
            if (engine.mixer.channel (i).params.fala.enabled.load())
            {
                if (engine.canalTranscricao.load() == i) return;   // ja e esse

                engine.canalTranscricao.store (i);
                engine.aoTranscrever = [this] (const float* x, int n)
                { fluxoFala.alimenta (x, n); };
                fluxoFala.start (settings.falaPortaAudio, engine.sampleRate.load());
                log ("transcricao em tempo real: CH" + juce::String (i + 1)
                     + " -> porta " + juce::String (settings.falaPortaAudio));
                return;
            }

        // nenhum canal marcado: desliga em vez de mandar silencio para a nuvem
        if (engine.canalTranscricao.load() >= 0)
        {
            engine.canalTranscricao.store (-1);
            fluxoFala.stop();
            log ("transcricao em tempo real: nenhum canal marcado, fluxo parado");
        }
    }

    /** Leva o catalogo de outputs para o roteamento do motor.

        O catalogo guardava a escolha certa e ninguem a aplicava: o
        rebindOutputs so cuida de destinos de REDE, e quem manda o audio para a
        placa e o roteamento por barramento. Resultado: escolher a saida
        funcionava na hora — porque a tela tambem mexia no motor — e sumia ao
        reabrir, quando so o roteamento antigo era aplicado. */
    void aplicaSaidasDaPlaca()
    {
        for (const auto& o : settings.outputs.outputs)
        {
            if (o.kind == int (mesa::InputKind::Network)) continue;
            if (o.pair < 0) continue;
            if (o.busSource < 0 || o.busSource >= mesa::kNumBuses) continue;

            settings.routing.busOutputPair[o.busSource] = o.pair;
            settings.routing.busGainDb[o.busSource]     = o.ganhoDb;
        }
        mesa::applyRouting (settings, engine.mixer);
    }

    void rebindNetwork()
    {
        sincronizaDoCatalogo();
        aplicaSaidasDaPlaca();

        // Fecha as placas secundarias que sairam do catalogo. Sem isto, uma
        // placa tirada de uso continuava aberta ate a mesa encerrar.
        {
            std::vector<juce::String> emUso;
            for (const auto& s2 : settings.catalog.sources)
                if (! s2.deviceName.empty()) emUso.push_back (juce::String (s2.deviceName));
            for (const auto& o : settings.outputs.outputs)
                if (! o.deviceName.empty()) emUso.push_back (juce::String (o.deviceName));
            secondaries.fechaAsQueSairam (emUso);
        }
        atualizaTranscricao();
        hub->setPlacaLivewire (juce::String (settings.livewirePlaca));
        // Descoberta so fica de pe se alguma fonte de fato usa NDI. Manter a
        // thread da biblioteca varrendo a rede sem necessidade ja derrubou a
        // mesa uma vez; nao ha motivo para pagar esse risco de graca.
        bool usesNdi = false;
        for (const auto& src : settings.catalog.sources)
            if (! src.streamName.empty()) { usesNdi = true; break; }

        // Uma vez ligada, a descoberta so para no encerramento. Religar em
        // operacao ja derrubou a mesa.
        if (usesNdi || configOpen) NdiEngine::instance().startDiscovery();

        const double sr = engine.sampleRate.load();
        const int    bl = juce::jmax (32, engine.blockSize.load());
        hub->rebind        (settings.catalog, sr, bl);
        hub->rebindOutputs (settings.outputs, sr, bl);

        // DEPOIS de os transmissores existirem.
        //
        // Estava antes, e anunciava uma lista vazia: os transmissores nascem
        // no rebindOutputs, logo acima. Anunciar o que ainda nao existe nao
        // falha com erro — simplesmente nao anuncia nada, e o log dizia "0
        // saidas Livewire" sem que nada parecesse errado.
        hub->setIdentidadeAnuncio (settings.livewireHwid, settings.livewireUdpc);
        hub->permiteAnuncio (settings.livewireAnuncio);
        hub->atualizaAnuncio (juce::String (settings.network.machineName));
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

        // Diagnostico da transcricao: sem isto, "nao grava nada" nao tem
        // investigacao — pode ser marca que nao chegou ao fader, nivel abaixo
        // do limiar, ou canal sem sinal.
        if (settings.falaEnabled && ++contaFala >= 250)   // ~10 s
        {
            contaFala = 0;
            for (int i = 0; i < engine.mixer.numChannels(); ++i)
            {
                auto& ch = engine.mixer.channel (i);
                if (! ch.params.fala.enabled.load()) continue;
                const float nivel = ch.tapDb (mesa::TapPoint::Input);
                const float limiar = ch.params.fala.thresholdDb.load();
                logToFile ("fala CH" + juce::String (i + 1) + ": nivel "
                           + juce::String (nivel, 1) + " / limiar "
                           + juce::String (limiar, 1) + " dBFS"
                           + (nivel > limiar ? "  ACIMA" : "  abaixo")
                           + (ch.falandoAgora()
                                  ? "  gravando ha " + juce::String (ch.duracaoFalaMs() / 1000.0, 1) + "s"
                                  : juce::String ("  aguardando fala"))
                           + "  gravados " + juce::String (escritorFala.gravados()));
            }
        }

        for (const auto& l : pendingLog) log (l);
        pendingLog.clear();

        // Teto por passada: uma rajada de transcricao pode enfileirar dezenas
        // de mensagens, e processar todas de uma vez segura a interface. O que
        // passar do teto e guardado aqui e sai no proximo ciclo, 40 ms depois.
        {
            auto entrada = receiver.take();
            for (auto& in : entrada) pendentesRemoto.push_back (in);

            const int teto = 12;
            for (int i = 0; i < teto && ! pendentesRemoto.empty(); ++i)
            {
                applyRemote (pendentesRemoto.front());
                pendentesRemoto.erase (pendentesRemoto.begin());
            }

            // fila que so cresce e sinal de que nao damos conta: descarta o
            // mais VELHO, que na transcricao ja nao interessa
            if (pendentesRemoto.size() > 200)
                pendentesRemoto.erase (pendentesRemoto.begin(),
                                       pendentesRemoto.begin() + 100);
        }

        logTriggerChanges();
        recolheFala();
        aplicaEntradasGpio();
        atualizaSaidasGpio();

        auto probs = hub->problems();
        if (engine.mixer.automation.testMode.load())
            probs.insert (probs.begin(), "MODO DE TESTE: nada e enviado ao vMix");
        alertText = probs.empty() ? juce::String()
                                  : juce::String (probs[0])
                                    + (probs.size() > 1
                                           ? "  (+" + juce::String (int (probs.size()) - 1) + ")"
                                           : "");

        if (settings.falaTempoReal)
        {
            const bool ok = fluxoFala.conectado();
            estadoFala.setText (ok ? "TRANSCRICAO ON" : "TRANSCRICAO OFF",
                                juce::dontSendNotification);
            estadoFala.setColour (juce::Label::textColourId,
                                  ok ? theme::busGreen : theme::onRed);
        }

        if (gravador.estaGravando())
        {
            alertText = (gravador.estaPausado() ? "GRAVACAO PAUSADA  " : "GRAVANDO  ")
                      + juce::String (gravador.segundos(), 0) + " s";
            recRec->setActive (! gravador.estaPausado());
        }
        else recRec->setActive (false);

        statusText = juce::String ("v") + mesa::kVersion + "   |   "
                   + (openError.isEmpty() ? engine.deviceName : "ERRO: " + openError)
                   + "   |   " + juce::String (engine.sampleRate.load(), 0) + " Hz"
                   + "   |   buffer " + juce::String (engine.blockSize.load())
                   + "   |   latencia " + juce::String (engine.latencyMs.load(), 2) + " ms"
                   + "   |   carga " + juce::String (engine.cpuLoad.load(), 1) + " %"
                   + "   |   cam " + juce::String (engine.automation.camera())
                   + (engine.automation.msUntilWide (engine.mixer) > 0.0
                          ? " (BG em " + juce::String (
                                engine.automation.msUntilWide (engine.mixer) / 1000.0, 1) + "s)"
                          : juce::String())
                   + "   |   layer " + juce::String (layer == 0 ? "A" : "B")
                   + "   |   enviados " + juce::String (sender->sent.load())
                   + " / falhas " + juce::String (sender->failed.load());

        const int n = engine.mixer.numChannels();
        for (int i = 0; i < strips.size(); ++i)
        {
            const int g = layer * kFadersPerLayer + i;
            if (g < n)
            {
                strips[i]->setTriggerState (engine.automation.stateOf (g));
                const int cam = engine.mixer.channel (g).params.trigger.camera.load();
                strips[i]->setOnAir (cam > 0 && cam == engine.automation.camera());
            }
            strips[i]->setGravando (gravador.estaGravando()
                                    && engine.canalGravado.load() == layer * kFadersPerLayer + i);
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
    std::unique_ptr<SurfaceButton> janelaMin, janelaTela, janelaSair;
    std::unique_ptr<SurfaceButton> recRec, recPause, recStop;
    juce::TextButton cfgButton, pageButton;
    juce::ToggleButton testMode;
    juce::TextEditor netLog, painelFala;
    juce::Label estadoFala;
    juce::Rectangle<int> chassis, statusArea;
    SecondaryDevices secondaries;
    CommandReceiver receiver;
    GpioClient gpio;
    SpeechWriter escritorFala;
    SpeechStreamer fluxoFala;
    int linhasTranscritas = 0;
    juce::ChildProcess transcritor;
    std::unique_ptr<juce::FileOutputStream> arquivoFala;
    juce::String diaDaFala;
    std::vector<CommandReceiver::Incoming> pendentesRemoto;
    int desdeUltimoFlush = 0, desdeUltimaApara = 0;
    int falasCortadas = 0;
    Recorder gravador;

public:
    /** Liga e desliga a gravacao de diagnostico de um canal. */
    juce::String alternaGravacao (int canal)
    {
        if (settings.falaTempoReal)
        {
            const bool ok = fluxoFala.conectado();
            estadoFala.setText (ok ? "TRANSCRICAO ON" : "TRANSCRICAO OFF",
                                juce::dontSendNotification);
            estadoFala.setColour (juce::Label::textColourId,
                                  ok ? theme::busGreen : theme::onRed);
        }

        if (gravador.estaGravando())
        {
            const auto f = gravador.arquivoAtual();
            const double s = gravador.segundos();
            const int perdidas = gravador.amostrasPerdidas();
            engine.canalGravado.store (-1);
            gravador.stop();
            log ("gravacao encerrada: " + f.getFileName() + "  "
                 + juce::String (s, 1) + " s"
                 + (perdidas > 0 ? "  (perdeu " + juce::String (perdidas) + " amostras)" : ""));
            return f.getFullPathName();
        }

        // no ponto "programa" nao ha canal: grava o PGM 1
        if (settings.recPonto != 2 && (canal < 0 || canal >= engine.mixer.numChannels()))
            return {};

        const auto pasta = settings.recPasta.empty()
                             ? settingsFile.getParentDirectory().getChildFile ("gravacoes")
                             : juce::File (settings.recPasta);
        const double taxa = settings.recTaxaDaPlaca ? engine.sampleRate.load() : 48000.0;
        const auto etiqueta = settings.recPonto == 2 ? juce::String ("PGM1")
                                                     : "CH" + juce::String (canal + 1);
        const auto f = gravador.start (pasta, taxa, etiqueta, settings.recBits);
        if (f == juce::File()) { log ("gravacao: nao consegui criar o arquivo"); return {}; }

        engine.pontoGravacao.store (settings.recPonto);
        engine.canalGravado.store (canal);
        static const char* pontos[] = { "entrada crua", "pos-fader", "PGM 1" };
        log (juce::String ("gravando ") + etiqueta + " ("
             + pontos[juce::jlimit (0, 2, settings.recPonto)] + ", "
             + juce::String (int (taxa)) + " Hz, " + juce::String (settings.recBits)
             + " bits) em " + f.getFileName());
        return f.getFullPathName();
    }

    bool estaGravando() const { return gravador.estaGravando(); }

    /** Canal que o botao do transporte grava: o que estiver com CUE ligado,
        senao o primeiro com fonte. Assim o transporte funciona sem obrigar o
        operador a abrir o menu do canal. */
    int canalParaGravar() const
    {
        for (int i = 0; i < engine.mixer.numChannels(); ++i)
            if (engine.mixer.channel (i).params.cue.load()) return i;
        for (int i = 0; i < engine.mixer.numChannels(); ++i)
            if (! engine.mixer.channel (i).name.empty()) return i;
        return 0;
    }
    double segundosGravados() const { return gravador.segundos(); }
    juce::File pastaGravacoes() const
    { return settingsFile.getParentDirectory().getChildFile ("gravacoes"); }

private:
    std::mutex mutexGpio;
    std::vector<GpioClient::Evento> entradasGpio;
    std::vector<int> ultimoOn;
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
    bool configOpen = false;
    bool avisouFila = false;
    bool avisouDuplicado = false;
    int contaFala = 0;
    std::vector<int> lastTrigState;
    /** Fader guardado por PAUSE, para o PLAY seguinte retomar no mesmo ponto. */
    std::map<int, float> pausedFader;
    std::unique_ptr<NetworkHub> hub;
    juce::String alertText;
    bool warnedLostOnce = false;
    int layer = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
