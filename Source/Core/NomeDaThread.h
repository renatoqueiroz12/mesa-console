#pragma once
#include <juce_core/juce_core.h>

/** Nome da thread atual, legivel de volta.

    O JUCE deixa DEFINIR o nome (setCurrentThreadName) mas nao oferece jeito de
    lê-lo depois — e o registro de queda precisa justamente ler. Guardamos numa
    variavel por thread, ao lado da chamada do JUCE, que continua valendo para
    o depurador e o gerenciador de tarefas. */
namespace mesa
{
    inline thread_local juce::String nomeDaThreadAtual;

    inline void batizaThread (const juce::String& nome)
    {
        nomeDaThreadAtual = nome;
        juce::Thread::setCurrentThreadName (nome);
    }

    inline juce::String nomeDaThread()
    {
        return nomeDaThreadAtual.isEmpty() ? juce::String ("(sem nome)")
                                           : nomeDaThreadAtual;
    }
}
