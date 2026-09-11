#pragma once
#include "../Core/NomeDaThread.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <cstring>

/** GPIO pelo LWRP, na porta 93 do no Axia.

    O protocolo e texto e a notacao e enxuta: cada porta tem 5 pinos, escritos
    como cinco letras. "l" e baixo (contato FECHADO, acionado) e "h" e alto
    (aberto). Maiuscula significa que aquele pino esta MUDANDO agora; minuscula,
    que ja esta estavel. Entao:

        GPO 2 lhhhh     fecha o pino 1 da porta 2, solta os outros
        ADD GPI         inscreve a mesa para receber mudancas das entradas

    Por que isto e melhor que placa de rele nova: o QOR desta instalacao ja tem
    8 entradas e 8 saidas com contato seco — foi o que o VER respondeu quando
    testamos o protocolo. Nao ha hardware para comprar nem driver para
    instalar, e a fiacao ja existe no rack.

    A conexao fica ABERTA, diferente das consultas de fonte: GPIO precisa
    reagir na hora, e reconectar a cada comando custaria dezenas de
    milissegundos que aparecem como atraso de luz de estudio. Se cair, uma
    thread reconecta sozinha — luz de ar que nao acende e falha visivel. */
class GpioClient
{
public:
    /** Mudanca vinda de uma entrada do no. */
    struct Evento
    {
        int porta = 0;      // 1..N
        int pino = 0;       // 1..5
        bool fechado = false;
    };

    /** A macro de nao-copiavel do JUCE tambem some com o construtor padrao. */
    GpioClient() = default;
    ~GpioClient() { stop(); }

    /** onEntrada e chamado na thread de rede: quem recebe deve so enfileirar. */
    void start (const juce::String& host, std::function<void (Evento)> onEntrada)
    {
        stop();
        endereco = host;
        callback = std::move (onEntrada);
        sair.store (false);
        worker = std::thread ([this] { loop(); });
    }

    void stop()
    {
        sair.store (true);
        {
            std::lock_guard<std::mutex> g (mutex);
            if (socket != nullptr) socket->close();
        }
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> g (mutex);
        socket = nullptr;
    }

    bool conectado() const noexcept { return ligado.load(); }
    juce::String erro() const { std::lock_guard<std::mutex> g (mutexErro); return ultimoErro; }
    int comandosEnviados() const noexcept { return enviados.load(); }
    int eventosRecebidos() const noexcept { return recebidos.load(); }

    /** Fecha ou abre UM pino, preservando os outros da mesma porta.

        O comando do LWRP escreve a porta inteira de uma vez; mandar so o pino
        desejado apagaria os demais. Por isso guardamos o estado local. */
    void setPino (int porta, int pino, bool fechado)
    {
        if (porta < 1 || pino < 1 || pino > 5) return;

        std::lock_guard<std::mutex> g (mutex);
        auto& estado = portas[porta];
        if (estado.empty()) estado = "hhhhh";
        estado[size_t (pino - 1)] = fechado ? 'l' : 'h';

        if (socket != nullptr && ligado.load())
        {
            const auto linha = "GPO " + juce::String (porta) + " " + estado + "\r\n";
            socket->write (linha.toRawUTF8(), int (linha.getNumBytesAsUTF8()));
            enviados.fetch_add (1);
        }
    }

    /** Pulso: fecha e solta depois de alguns milissegundos. E o que a maioria
        dos equipamentos espera para "start" e "stop" — contato momentaneo. */
    void pulso (int porta, int pino, int ms = 250)
    {
        setPino (porta, pino, true);
        juce::Timer::callAfterDelay (ms, [this, porta, pino] { setPino (porta, pino, false); });
    }

private:
    void loop()
    {
        // se cair, o log da queda diz o nome em vez de "(sem nome)"
        mesa::batizaThread ("gpio");

        while (! sair.load())
        {
            {
                std::lock_guard<std::mutex> g (mutex);
                socket = std::make_unique<juce::StreamingSocket>();
            }

            if (! socket->connect (endereco, 93, 3000))
            {
                anotaErro ("sem resposta em " + endereco + ":93");
                ligado.store (false);
                esperaOuSai (3000);
                continue;
            }

            ligado.store (true);
            anotaErro ({});

            // inscreve para receber mudancas das entradas e le o estado atual
            const char* inicio = "ADD GPI\r\nADD GPO\r\nGPI\r\nGPO\r\n";
            socket->write (inicio, int (std::strlen (inicio)));

            juce::String buffer;
            char bruto[1024];

            while (! sair.load() && socket->isConnected())
            {
                const int pronto = socket->waitUntilReady (true, 200);
                if (pronto < 0) break;
                if (pronto == 0) continue;

                const int n = socket->read (bruto, sizeof (bruto) - 1, false);
                if (n <= 0) break;
                bruto[n] = 0;
                buffer += juce::String::fromUTF8 (bruto, n);

                int quebra;
                while ((quebra = buffer.indexOfChar ('\n')) >= 0)
                {
                    processa (buffer.substring (0, quebra).trim());
                    buffer = buffer.substring (quebra + 1);
                }
            }

            ligado.store (false);
            {
                std::lock_guard<std::mutex> g (mutex);
                if (socket != nullptr) socket->close();
            }
            if (! sair.load()) { anotaErro ("conexao caiu, reconectando"); esperaOuSai (2000); }
        }
    }

    void processa (const juce::String& linha)
    {
        // GPI <porta> <5 letras>
        if (! linha.startsWith ("GPI ")) return;

        auto partes = juce::StringArray::fromTokens (linha, " ", "");
        if (partes.size() < 3) return;

        const int porta = partes[1].getIntValue();
        const auto pinos = partes[2];
        if (pinos.length() < 5) return;

        for (int i = 0; i < 5; ++i)
        {
            const auto c = pinos[i];
            // maiuscula = mudando agora; e a borda que interessa
            if (c == 'L' || c == 'H')
            {
                recebidos.fetch_add (1);
                if (callback) callback ({ porta, i + 1, c == 'L' });
            }
        }
    }

    void esperaOuSai (int ms)
    {
        const auto fim = juce::Time::getMillisecondCounter() + juce::uint32 (ms);
        while (! sair.load() && juce::Time::getMillisecondCounter() < fim)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }

    void anotaErro (const juce::String& e)
    {
        std::lock_guard<std::mutex> g (mutexErro);
        ultimoErro = e;
    }

    juce::String endereco;
    std::function<void (Evento)> callback;
    std::unique_ptr<juce::StreamingSocket> socket;
    std::thread worker;
    std::atomic<bool> sair { true }, ligado { false };
    std::atomic<int> enviados { 0 }, recebidos { 0 };
    std::mutex mutex;
    mutable std::mutex mutexErro;
    juce::String ultimoErro;
    std::map<int, std::string> portas;   // estado local de cada porta

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GpioClient)
};
