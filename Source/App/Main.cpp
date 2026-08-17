#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"

class MesaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Mesa Console"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        // Captura de queda: sem isso, um crash de madrugada nao deixa nada
        // alem do processo sumido. Com isso, fica o local exato no disco.
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            auto f = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                        .getParentDirectory().getChildFile ("mesa-crash.log");
            f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                          + "  QUEDA\n" + juce::SystemStats::getStackBacktrace() + "\n\n",
                          false, false, "\n");
        });

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
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
            : DocumentWindow (title, juce::Colours::black, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (12), true);   // 12 faders
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MesaApplication)
