#pragma once
#include <juce_core/juce_core.h>
#include "../Core/AsyncSource.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

/** A mesa como FONTE Livewire.

    O canal vira endereco pela mesma conta da recepcao: canal = y*256 + z, no
    grupo 239.192.y.z, porta 5004. Um stream padrao carrega 240 quadros por
    pacote — 5 ms de estereo a 48 kHz em 24 bits, dando os 1440 bytes que os
    equipamentos Axia esperam.

    Sobre o relogio, que e o ponto delicado: quem transmite deveria estar
    travado no PTP da rede. Nao estamos. O que fazemos e ritmar o envio pelo
    relogio da maquina e deixar a correcao adaptativa da fila absorver a
    diferenca entre a nossa placa e a rede — a mesma peca que ja usamos na
    recepcao e nas placas secundarias. Funciona porque o receptor tem buffer
    proprio; e o ponto que precisa de teste longo com o no real.

    A mesa NAO aparece sozinha na lista de fontes do QOR: aquela lista vem do
    protocolo de anuncio, proprietario e nao documentado. O caminho e apontar o
    Primary source do no para o numero do canal. O audio e o mesmo. */
class LivewireSender
{
public:
    LivewireSender (mesa::AsyncSource& esquerdo, mesa::AsyncSource& direito)
        : left (esquerdo), right (direito) {}

    ~LivewireSender() { stop(); }

    static juce::String addressForChannel (int canal)
    {
        return "239.192." + juce::String ((canal >> 8) & 0xff)
             + "." + juce::String (canal & 0xff);
    }

    bool start (int numero, double sampleRate = 48000.0)
    {
        stop();
        if (numero <= 0 || numero > 32767) { erro = "canal fora da faixa 1..32767"; return false; }

        grupo = addressForChannel (numero);
        canal = numero;
        sr = sampleRate;

        socket = std::make_unique<juce::DatagramSocket> (true);
        if (! socket->bindToPort (0))
        {
            erro = "nao consegui abrir socket de saida";
            socket = nullptr;
            return false;
        }
        socket->setMulticastLoopbackEnabled (true);

        ssrc = juce::Random::getSystemRandom().nextInt();
        seq = juce::uint16 (juce::Random::getSystemRandom().nextInt (65535));
        ts = juce::uint32 (juce::Random::getSystemRandom().nextInt());

        erro.clear();
        sair.store (false);
        worker = std::thread ([this] { loop(); });
        return true;
    }

    void stop()
    {
        sair.store (true);
        if (worker.joinable()) worker.join();
        socket = nullptr;
    }

    bool running() const noexcept { return worker.joinable(); }
    int  channel() const noexcept { return canal; }
    juce::String address() const { return grupo; }
    juce::String error() const { return erro; }
    int  packets() const noexcept { return enviados.load(); }

private:
    static constexpr int kQuadros = 240;                 // 5 ms a 48 kHz
    static constexpr int kCarga   = kQuadros * 6;        // 2 canais x 24 bits
    static constexpr int kCabec   = 12;                  // cabecalho RTP

    void loop()
    {
        std::vector<unsigned char> pacote (size_t (kCabec + kCarga), 0);
        // O segundo argumento nao e enfeite: sem ele o compilador le isto como
        // DECLARACAO DE FUNCAO, nao de vetor.
        std::vector<float> l (size_t (kQuadros), 0.0f);
        std::vector<float> r (size_t (kQuadros), 0.0f);

        using clock = std::chrono::steady_clock;
        auto proximo = clock::now();
        const auto intervalo = std::chrono::microseconds (
            int64_t (1000000.0 * double (kQuadros) / sr));

        while (! sair.load())
        {
            // ritmo pelo relogio real: e ele que o receptor espera, nao o da
            // nossa placa. A diferenca entre os dois vira folga na fila e a
            // correcao adaptativa cuida disso.
            proximo += intervalo;
            std::this_thread::sleep_until (proximo);
            if (sair.load()) break;

            std::memcpy (l.data(), left .pull (kQuadros), size_t (kQuadros) * sizeof (float));
            std::memcpy (r.data(), right.pull (kQuadros), size_t (kQuadros) * sizeof (float));

            // cabecalho RTP
            auto* p = pacote.data();
            p[0] = 0x80;                       // versao 2, sem padding nem CSRC
            p[1] = 96;                         // carga dinamica, como o Livewire usa
            p[2] = (unsigned char) (seq >> 8);
            p[3] = (unsigned char) (seq & 0xff);
            escreve32 (p + 4, ts);
            escreve32 (p + 8, juce::uint32 (ssrc));
            ++seq;
            ts += juce::uint32 (kQuadros);

            // L24 big-endian intercalado
            auto* d = p + kCabec;
            for (int i = 0; i < kQuadros; ++i)
            {
                escreve24 (d, l[size_t (i)]); d += 3;
                escreve24 (d, r[size_t (i)]); d += 3;
            }

            if (socket != nullptr
                && socket->write (grupo, 5004, pacote.data(), int (pacote.size())) > 0)
                enviados.fetch_add (1);
        }
    }

    static void escreve32 (unsigned char* d, juce::uint32 v) noexcept
    {
        d[0] = (unsigned char) (v >> 24); d[1] = (unsigned char) (v >> 16);
        d[2] = (unsigned char) (v >> 8);  d[3] = (unsigned char) v;
    }

    /** float -1..1 -> 24 bits com sinal, big-endian, com limite duro: estourar
        o formato produziria estalo, nao distorcao suave. */
    static void escreve24 (unsigned char* d, float x) noexcept
    {
        const float lim = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
        int v = int (lim * 8388607.0f);
        d[0] = (unsigned char) ((v >> 16) & 0xff);
        d[1] = (unsigned char) ((v >> 8)  & 0xff);
        d[2] = (unsigned char) (v & 0xff);
    }

    mesa::AsyncSource& left;
    mesa::AsyncSource& right;
    std::unique_ptr<juce::DatagramSocket> socket;
    std::thread worker;
    std::atomic<bool> sair { true };
    std::atomic<int> enviados { 0 };
    juce::String grupo, erro;
    int canal = 0;
    double sr = 48000.0;
    juce::uint16 seq = 0;
    juce::uint32 ts = 0;
    int ssrc = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivewireSender)
};
