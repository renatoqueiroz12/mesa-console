#pragma once
#include <juce_core/juce_core.h>

/** Rastro de arranque: uma linha por etapa, gravada na hora.

    Quando a mesa morre antes de abrir o log, nao sobra nada — nem registro de
    queda, porque o mecanismo do sistema nem sempre pega. Aqui cada etapa
    escreve e FECHA o arquivo imediatamente: o que estiver escrito sobrevive a
    qualquer forma de morte, e a ultima linha diz onde ela parou.

    Custa uma escrita por etapa, so no arranque. E barato para o que resolve. */
namespace mesa
{
    inline void rastro (const juce::String& etapa)
    {
        auto pasta = juce::File::getSpecialLocation (
                         juce::File::userApplicationDataDirectory)
                     .getChildFile ("MesaConsole");
        pasta.createDirectory();

        auto f = pasta.getChildFile ("arranque.log");
        f.appendText (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ")
                      + etapa + "\n", false, false, "\n");
    }

    /** Limpa o rastro anterior e marca o inicio. */
    inline void rastroComeca()
    {
        auto pasta = juce::File::getSpecialLocation (
                         juce::File::userApplicationDataDirectory)
                     .getChildFile ("MesaConsole");
        pasta.createDirectory();
        pasta.getChildFile ("arranque.log").deleteFile();
        rastro ("=== arranque ===");
    }
}
