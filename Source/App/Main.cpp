#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "../Core/NomeDaThread.h"
#include "../Core/Version.h"
#include "../Core/Rastro.h"

/** Escala da interface — fora da classe para poder rodar ANTES de qualquer
janela nascer, que e o que o JUCE pede. */
/** Quanto encolher para a mesa caber nesta tela. */
inline float escalaParaCaber()
{
    auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (disp == nullptr) return 1.0f;

    const auto area = disp->userArea;
    const float porLargura = float (area.getWidth())  / 1920.0f;
    const float porAltura  = float (area.getHeight()) / 1080.0f;
    const float cabe = juce::jmin (porLargura, porAltura);

    // nunca AUMENTA: em tela grande a mesa fica no tamanho de desenho,
    // com margem em volta, que e melhor do que esticar
    return juce::jlimit (0.5f, 1.0f, cabe);
}

inline void aplicaEscala()
{
    mesa::Settings cfg;
    const auto arquivo = juce::File::getSpecialLocation (
                             juce::File::userApplicationDataDirectory)
                         .getChildFile ("MesaConsole").getChildFile ("settings.json");
    mesa::loadSettings (arquivo.getFullPathName().toStdString(), cfg);

    const float escala = cfg.routing.escalaInterface > 0.01f
                       ? cfg.routing.escalaInterface
                       : escalaParaCaber();

    juce::Desktop::getInstance().setGlobalScaleFactor (escala);
}

class MesaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Mesa Console"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    /** Segunda instancia traz a primeira para a frente.

        A recusa de multiplas instancias ja existia acima; faltava o que fazer
        quando alguem tenta abrir de novo — sem isto o duplo clique parecia nao
        funcionar. */
    void anotherInstanceStarted (const juce::String&) override
    {
        // traz a que ja esta rodando para a frente, em vez de abrir outra
        if (mainWindow != nullptr)
        {
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

    void initialise (const juce::String&) override
    {
        mesa::rastroComeca();
        // Captura de queda: sem isso, um crash de madrugada nao deixa nada
        // alem do processo sumido. Com isso, fica o local exato no disco.
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            // Junto das configuracoes, que e pasta SEMPRE GRAVAVEL.
            //
            // Ficava ao lado do executavel. Numa maquina onde a pasta do
            // programa nao aceita escrita, a queda acontecia e o registro nao
            // saia — ficavamos sabendo que caiu e nada mais. Registro que
            // depende de permissao e registro que falta na hora errada.
            auto pasta = juce::File::getSpecialLocation (
                             juce::File::userApplicationDataDirectory)
                         .getChildFile ("MesaConsole");
            pasta.createDirectory();
            auto f = pasta.getChildFile ("mesa-crash.log");
            // QUAL thread caiu.
            //
            // A pilha do Windows vem sem os nossos simbolos e termina em
            // BaseThreadInitThunk: da para saber que morreu numa thread
            // secundaria e nao qual. Com o nome, a busca deixa de ser no
            // escuro — desde que cada thread nossa se apresente (ver os
            // setCurrentThreadName espalhados pelo codigo).
            const auto nome = mesa::nomeDaThread();

            f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                          + "  QUEDA na thread \"" + nome + "\"\n"
                          + juce::SystemStats::getStackBacktrace() + "\n\n",
                          false, false, "\n");
        });

        // Escala ANTES de existir janela.
        //
        // O JUCE pede que o fator global seja definido antes de qualquer
        // janela nascer. Chamando de dentro do construtor, a janela ja estava
        // meio montada quando a escala mudava — e este arranque caiu tao cedo
        // que nem o log chegou a abrir.
        mesa::rastro ("antes da escala");
        aplicaEscala();
        mesa::rastro ("escala aplicada");

        mesa::rastro ("criando a janela");
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        mesa::rastro ("janela criada");
    }

    void shutdown() override
    {
        auto f = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getParentDirectory().getChildFile ("mesa.log");
        f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                      + "  === shutdown pedido pelo sistema ou pelo usuario ===\n",
                      false, false, "\n");
        mainWindow = nullptr;
    }

    /** O Windows pede para fechar (logoff, desligamento, gerenciador de tarefas).
        Registrar isso separa "alguem mandou fechar" de "quebrou sozinha". */
    void systemRequestedQuit() override
    {
        auto f = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getParentDirectory().getChildFile ("mesa.log");
        f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                      + "  === o SISTEMA pediu encerramento ===\n", false, false, "\n");
        quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& title)
            : DocumentWindow (title + "  v" + mesa::kVersion,
                              juce::Colours::black, DocumentWindow::allButtons)
        {
            // Sem barra de titulo do sistema: a mesa ocupa a tela inteira,
            // inclusive por cima da barra de tarefas. Numa mesa de ar, ver o
            // Windows atras e convite a clicar no lugar errado no meio do
            // programa.
            setUsingNativeTitleBar (false);
            setTitleBarHeight (0);
            // a superficie diz de quantos canais precisa: 8 por camada, duas
            // camadas. Numero fixo aqui ja deixou o layer B pela metade.
            setContentOwned (new MainComponent (MainComponent::kCanaisNecessarios), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

            // Cobre a barra de tarefas — mas so entra em tela cheia se couber.
            //
            // Em monitor menor que o desenho da mesa, o modo quiosque deixava
            // as bordas fora do alcance e nem os botoes de janela apareciam.
            // Melhor abrir em janela normal, onde da para rolar e mover, do
            // que cobrir uma tela que nao cabe.
            if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                if (disp->userArea.getWidth() >= 1920 && disp->userArea.getHeight() >= 1080)
                    juce::Desktop::getInstance().setKioskModeComponent (this, false);
        }



        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MesaApplication)
