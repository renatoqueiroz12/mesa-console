#pragma once
#include <juce_core/juce_core.h>
#include "../Core/RemoteCommand.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/** Recebe comandos de texto de fora: cartucheira, playout, automacao.

    Duas portas: UDP para quem dispara e esquece (cartucheira), TCP para quem
    quer conexao mantida. As duas falam o mesmo texto.

    O socket roda em thread propria e so ENFILEIRA. Aplicar comando mexe em
    canal e pode disparar logica, entao isso acontece na thread da interface,
    onde ja e seguro. Nada de rede encosta no callback de audio. */
class CommandReceiver
{
public:
    struct Incoming
    {
        mesa::RemoteCommand cmd;
        juce::String raw, from;
    };

    CommandReceiver() = default;
    ~CommandReceiver() { stop(); }

    void start (int udpPort, int tcpPort)
    {
        stop();
        quit.store (false);

        if (udpPort > 0)
        {
            udp = std::make_unique<juce::DatagramSocket>();
            if (udp->bindToPort (udpPort))
            {
                udpPortInUse = udpPort;
                udpThread = std::thread ([this] { udpLoop(); });
            }
            else
            {
                lastError = "nao consegui abrir a porta UDP " + juce::String (udpPort)
                          + " (outro programa ja esta usando?)";
                udp.reset();
            }
        }

        if (tcpPort > 0)
        {
            tcp = std::make_unique<juce::StreamingSocket>();
            if (tcp->createListener (tcpPort))
            {
                tcpPortInUse = tcpPort;
                tcpThread = std::thread ([this] { tcpLoop(); });
            }
            else
            {
                lastError = "nao consegui escutar TCP na porta " + juce::String (tcpPort);
                tcp.reset();
            }
        }
    }

    void stop()
    {
        quit.store (true);
        if (udp != nullptr) udp->shutdown();
        if (tcp != nullptr) tcp->close();
        if (udpThread.joinable()) udpThread.join();
        if (tcpThread.joinable()) tcpThread.join();
        udp.reset();
        tcp.reset();
    }

    /** Chamado pela interface: leva o que chegou desde a ultima vez. */
    std::vector<Incoming> take()
    {
        std::lock_guard<std::mutex> g (mutex);
        auto out = std::move (pending);
        pending.clear();
        return out;
    }

    bool listeningUdp() const noexcept { return udp != nullptr; }
    bool listeningTcp() const noexcept { return tcp != nullptr; }
    int  udpPort() const noexcept { return udpPortInUse; }
    int  tcpPort() const noexcept { return tcpPortInUse; }
    juce::String error() const { return lastError; }
    int received() const noexcept { return count.load(); }

private:
    void queueLines (const juce::String& text, const juce::String& from)
    {
        // um datagrama pode trazer varias linhas; uma conexao TCP quase sempre traz
        auto lines = juce::StringArray::fromLines (text);
        std::lock_guard<std::mutex> g (mutex);
        for (const auto& l : lines)
        {
            const auto trimmed = l.trim();
            if (trimmed.isEmpty()) continue;
            Incoming in;
            in.raw = trimmed;
            in.from = from;
            in.cmd = mesa::parseRemoteCommand (trimmed.toStdString());
            pending.push_back (in);
            count.fetch_add (1);
        }
    }

    void udpLoop()
    {
        char buf[1024];
        while (! quit.load())
        {
            if (udp == nullptr) return;
            const int ready = udp->waitUntilReady (true, 200);
            if (ready <= 0) continue;

            juce::String senderIp;
            int senderPort = 0;
            const int n = udp->read (buf, sizeof (buf) - 1, false, senderIp, senderPort);
            if (n <= 0) continue;
            buf[n] = 0;
            queueLines (juce::String::fromUTF8 (buf, n), senderIp + " UDP");
        }
    }

    void tcpLoop()
    {
        char buf[1024];
        while (! quit.load())
        {
            if (tcp == nullptr) return;
            std::unique_ptr<juce::StreamingSocket> conn (tcp->waitForNextConnection());
            if (conn == nullptr) continue;

            const juce::String who = conn->getHostName() + " TCP";
            while (! quit.load() && conn->isConnected())
            {
                const int ready = conn->waitUntilReady (true, 200);
                if (ready < 0) break;
                if (ready == 0) continue;
                const int n = conn->read (buf, sizeof (buf) - 1, false);
                if (n <= 0) break;
                buf[n] = 0;
                queueLines (juce::String::fromUTF8 (buf, n), who);
            }
        }
    }

    std::unique_ptr<juce::DatagramSocket> udp;
    std::unique_ptr<juce::StreamingSocket> tcp;
    std::thread udpThread, tcpThread;
    std::atomic<bool> quit { true };
    std::atomic<int> count { 0 };
    std::mutex mutex;
    std::vector<Incoming> pending;
    int udpPortInUse = 0, tcpPortInUse = 0;
    juce::String lastError;
};
