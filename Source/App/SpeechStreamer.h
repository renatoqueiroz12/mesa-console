#pragma once
#include "../Core/NomeDaThread.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/** Manda audio CONTINUO para o transcritor, por socket local.

    Diferente do SpeechWriter, que espera o trecho fechar e grava um arquivo.
    Aqui o audio flui sem parar, e o transcritor devolve texto enquanto a
    pessoa ainda fala. E a diferenca entre 17 segundos de espera e menos de um.

    Por que socket local e nao a nuvem direto daqui:

    Uma biblioteca de rede dentro do processo de audio ja nos custou duas
    quedas. Transcricao em nuvem precisa de WebSocket, TLS, reconexao, fila de
    reenvio — cada uma dessas e uma chance de travar o programa que esta no ar.
    Do lado de fora, se o transcritor cair, a mesa nem percebe: o socket
    fecha, o audio para de ser enviado, e o ar segue.

    O formato e cru de proposito: 16 kHz, 16 bits, mono, sem cabecalho. E o que
    todas as APIs de streaming esperam, e evita conversao do outro lado. */
class SpeechStreamer
{
public:
    SpeechStreamer() = default;
    ~SpeechStreamer() { stop(); }

    void start (int portaLocal, double sampleRateEntrada)
    {
        stop();
        porta = portaLocal;
        srEntrada = sampleRateEntrada;
        sair.store (false);
        worker = std::thread ([this] { loop(); });
    }

    void stop()
    {
        sair.store (true);
        if (socket != nullptr) socket->close();
        if (worker.joinable()) worker.join();
        socket = nullptr;
    }

    bool conectado() const noexcept { return ligado.load(); }
    long long amostrasEnviadas() const noexcept { return enviadas.load(); }
    int descartes() const noexcept { return perdidas.load(); }

    /** Chamado do callback de audio. Nao aloca, nao bloqueia, nao espera. */
    void alimenta (const float* x, int n) noexcept
    {
        if (! ligado.load (std::memory_order_relaxed) || fila.empty()) return;

        const int cap = int (fila.size());
        const int w = escrita.load (std::memory_order_relaxed);
        const int r = leitura.load (std::memory_order_acquire);
        if (cap - (w - r) - 1 < n) { perdidas.fetch_add (1, std::memory_order_relaxed); return; }

        for (int i = 0; i < n; ++i) fila[size_t ((w + i) % cap)] = x[i];
        escrita.store (w + n, std::memory_order_release);
    }

private:
    void loop()
    {
        mesa::batizaThread ("fala-fluxo");
        // 8 s de folga: atravessa uma lentidao passageira do transcritor sem
        // perder nada. Audio e barato em memoria; texto perdido nao volta.
        fila.assign (size_t (srEntrada * 8.0), 0.0f);
        escrita.store (0); leitura.store (0);

        // decimacao 48k -> 16k com o mesmo filtro do gravador de fala: sem
        // ele, o que esta acima de 8 kHz volta rebatido e o reconhecedor erra
        std::vector<float> bloco;
        std::vector<juce::int16> saida;
        float z1 = 0.0f, z2 = 0.0f;

        while (! sair.load())
        {
            if (socket == nullptr || ! socket->isConnected())
            {
                ligado.store (false);
                socket = std::make_unique<juce::StreamingSocket>();
                if (! socket->connect ("127.0.0.1", porta, 1000))
                {
                    socket = nullptr;
                    esperaOuSai (2000);          // o transcritor pode subir depois
                    continue;
                }
                ligado.store (true);
                escrita.store (0); leitura.store (0);
                z1 = z2 = 0.0f;
            }

            const int cap = int (fila.size());
            const int w = escrita.load (std::memory_order_acquire);
            int r = leitura.load (std::memory_order_relaxed);
            int disponivel = w - r;

            const int fator = juce::jmax (1, int (std::round (srEntrada / 16000.0)));
            if (disponivel < fator * 160)        // ~10 ms de audio ja convertido
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                continue;
            }

            const int quantas = juce::jmin (disponivel, 8192);
            bloco.resize (size_t (quantas));
            for (int i = 0; i < quantas; ++i) bloco[size_t (i)] = fila[size_t ((r + i) % cap)];
            leitura.store (r + quantas, std::memory_order_release);

            passaBaixa (bloco, srEntrada, 7000.0f, z1, z2);

            saida.clear();
            saida.reserve (size_t (quantas / fator + 1));
            for (int i = 0; i + fator <= quantas; i += fator)
            {
                const float v = juce::jlimit (-1.0f, 1.0f, bloco[size_t (i)]);
                saida.push_back (juce::int16 (v * 32767.0f));
            }

            if (! saida.empty())
            {
                // Escrita so quando o outro lado esta pronto para receber.
                //
                // Sem esta checagem, um transcritor lento prende esta thread
                // dentro do write, e a fila de audio enche por tras. Melhor
                // perder audio do que segurar quem entrega.
                // Espera GENEROSA antes de desistir.
                //
                // Com 200 ms, um transcritor momentaneamente ocupado fazia a
                // mesa jogar audio fora o tempo todo — a API recebia fala
                // picada e o texto voltava lento e truncado. A protecao contra
                // travamento continua (nunca esperamos para sempre), mas
                // desistir cedo demais estraga a transcricao para evitar um
                // problema que a fila maior ja resolve.
                if (socket->waitUntilReady (false, kEsperaMs) <= 0)
                {
                    perdidas.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }

                const int bytes = int (saida.size()) * 2;
                if (socket->write (saida.data(), bytes) != bytes)
                {
                    socket->close();
                    ligado.store (false);
                    continue;
                }
                enviadas.fetch_add (saida.size());
            }
        }
    }

    /** Biquad passa-baixa com estado que ATRAVESSA os blocos.

        No gravador de arquivo o filtro roda ida e volta sobre o trecho todo.
        Aqui o audio e continuo e nao ha "trecho todo": zerar o estado a cada
        bloco produziria um clique a cada 8 mil amostras. */
    static void passaBaixa (std::vector<float>& x, double sr, float corte,
                            float& z1, float& z2)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * double (corte) / sr;
        const double cosw = std::cos (w), alpha = std::sin (w) / (2.0 * 0.7071);
        const double b0 = (1.0 - cosw) * 0.5, b1 = 1.0 - cosw, b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cosw, a2 = 1.0 - alpha;
        const float B0 = float (b0/a0), B1 = float (b1/a0), B2 = float (b2/a0);
        const float A1 = float (a1/a0), A2 = float (a2/a0);

        for (auto& v : x)
        {
            const float ent = v;
            const float sai = B0 * ent + z1;
            z1 = B1 * ent - A1 * sai + z2;
            z2 = B2 * ent - A2 * sai;
            v = sai;
        }
    }

    void esperaOuSai (int ms)
    {
        const auto fim = juce::Time::getMillisecondCounter() + juce::uint32 (ms);
        while (! sair.load() && juce::Time::getMillisecondCounter() < fim)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }

    std::unique_ptr<juce::StreamingSocket> socket;
    std::thread worker;
    std::atomic<bool> sair { true }, ligado { false };
    std::atomic<long long> enviadas { 0 };
    std::atomic<int> perdidas { 0 };
    std::vector<float> fila;
    std::atomic<int> escrita { 0 }, leitura { 0 };
    static constexpr int kEsperaMs = 2000;
    int porta = 8891;
    double srEntrada = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeechStreamer)
};
