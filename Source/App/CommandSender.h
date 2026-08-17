#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "../Core/AutomationEngine.h"
#include "../Core/Settings.h"

/** Thread de rede: consome a fila que o audio enche e abre o socket de verdade.
    O callback de audio NUNCA chega aqui — ele so empurra Command na fila.

    UDP: dispara e esquece (cartucheira).
    TCP: mantem conexao aberta e reconecta sozinho (vMix, porta 8099). */
class CommandSender : private juce::Thread
{
public:
    CommandSender (mesa::AutomationEngine& engine, const mesa::Settings& settings)
        : juce::Thread ("command-sender"), automation (engine), config (settings)
    {
        startThread (juce::Thread::Priority::normal);
    }

    ~CommandSender() override
    {
        stopThread (1000);
        for (auto* s : tcp) delete s;
    }

    /** Ultimas linhas enviadas, para a UI mostrar sem tocar na thread de audio. */
    juce::StringArray takeLog()
    {
        const juce::ScopedLock sl (logLock);
        auto copy = log;
        log.clear();
        return copy;
    }

    std::atomic<int> sent { 0 }, failed { 0 };

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            mesa::Command c;
            bool didWork = false;

            while (automation.commands.pop (c))
            {
                didWork = true;
                deliver (c);
            }

            if (! didWork)
                wait (10);      // fila vazia: dorme um pouco, sem ocupar CPU
        }
    }

    void deliver (const mesa::Command& c)
    {
        const auto text = juce::String (mesa::commandText (c));

        if (c.simulated)
        {
            addLog ("[TESTE] nao enviado: " + text);
            return;
        }

        const int idx = juce::jlimit (0, juce::jmax (0, int (config.targets.size()) - 1), c.target);
        if (config.targets.empty()) { failed++; addLog ("sem destino configurado"); return; }
        const auto& t = config.targets[size_t (idx)];

        const juce::String proto = juce::String (t.protocol).toUpperCase();
        const juce::String host  = juce::String (t.host);
        bool ok = false;

        if (proto == "UDP")
        {
            juce::DatagramSocket socket;
            const auto payload = text.toRawUTF8();
            ok = socket.write (host, t.port, payload, int (strlen (payload))) > 0;
        }
        else if (proto == "TCP")
        {
            ok = writeTcp (idx, t, text);
        }
        else
        {
            addLog ("protocolo nao suportado: " + juce::String (t.protocol));
        }

        if (ok) ++sent; else ++failed;
        addLog (juce::String (ok ? "OK   " : "FALHA ") + proto + " " + juce::String (t.host)
                + ":" + juce::String (t.port) + "  <-  " + text);
    }

    /** Mantem uma conexao por destino TCP e reconecta quando cai. */
    bool writeTcp (int idx, const mesa::CommandTarget& t, const juce::String& text)
    {
        while (int (tcp.size()) <= idx) tcp.push_back (nullptr);

        if (tcp[size_t (idx)] == nullptr || ! tcp[size_t (idx)]->isConnected())
        {
            delete tcp[size_t (idx)];
            tcp[size_t (idx)] = new juce::StreamingSocket();
            if (! tcp[size_t (idx)]->connect (juce::String (t.host), t.port, 300))
            {
                addLog ("nao conectou em " + juce::String (t.host) + ":" + juce::String (t.port));
                return false;
            }
        }

        auto payload = text + (t.appendNewline ? "\r\n" : "");
        const auto* raw = payload.toRawUTF8();
        const int n = int (strlen (raw));
        const bool ok = tcp[size_t (idx)]->write (raw, n) == n;
        if (! ok) { delete tcp[size_t (idx)]; tcp[size_t (idx)] = nullptr; }   // reconecta na proxima
        return ok;
    }

    void addLog (const juce::String& line)
    {
        const juce::ScopedLock sl (logLock);
        log.add (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + line);
        while (log.size() > 200) log.remove (0);
    }

    mesa::AutomationEngine& automation;
    const mesa::Settings& config;
    std::vector<juce::StreamingSocket*> tcp;
    juce::CriticalSection logLock;
    juce::StringArray log;
};
