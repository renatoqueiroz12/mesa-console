#pragma once
#include <juce_core/juce_core.h>
#include "../Core/AsyncSource.h"
#include <atomic>
#include <thread>
#include <vector>

/** Recepcao nativa de Livewire / AES67.

    O canal Livewire vira endereco por conta: canal = y*256 + z, no grupo
    239.192.y.z, porta UDP 5004. O canal 3101 e 239.192.12.29.

    Por que da para fazer sem PTP: o relogio da rede so e obrigatorio para quem
    TRANSMITE. Um receptor pode tocar no relogio da propria placa e absorver a
    diferenca com reamostragem adaptativa — que e exatamente o que a AsyncSource
    ja faz, testada com deriva simulada. Sem essa peca pronta, isto aqui nao
    seria viavel; com ela, e so decodificar o pacote.

    O formato e RTP com carga L24 big-endian intercalada. Um pacote padrao traz
    1440 bytes de audio (5 ms de estereo em 24 bits); os "live" trazem 72 bytes
    (0,25 ms). Nao precisamos saber qual e: lemos o que vier. */
class LivewireReceiver
{
public:
    LivewireReceiver (mesa::AsyncSource& leftQueue, mesa::AsyncSource& rightQueue)
        : left (leftQueue), right (rightQueue) {}

    ~LivewireReceiver() { stop(); }

    /** Endereco multicast a partir do numero do canal Livewire. */
    static juce::String addressForChannel (int channel)
    {
        const int y = (channel >> 8) & 0xff;
        const int z = channel & 0xff;
        return "239.192." + juce::String (y) + "." + juce::String (z);
    }

    /** localIp e a placa de rede por onde entrar no grupo. Em maquina com mais
        de uma interface, deixar o sistema escolher costuma dar no lugar errado. */
    bool start (int channel, const juce::String& localIp = {})
    {
        stop();
        if (channel <= 0 || channel > 32767) { lastError = "canal fora da faixa 1..32767"; return false; }

        group = addressForChannel (channel);
        chan = channel;

        socket = std::make_unique<juce::DatagramSocket> (false);
        socket->setEnablePortReuse (true);
        if (! socket->bindToPort (kPort))
        {
            lastError = "nao consegui abrir a porta " + juce::String (kPort)
                      + " (outro programa usando?)";
            socket = nullptr;
            return false;
        }
        if (! socket->joinMulticast (group))
        {
            lastError = "nao consegui entrar no grupo " + group
                      + " (a mesa esta na VLAN do Livewire? IGMP liberado no switch?)";
            socket = nullptr;
            return false;
        }
        if (localIp.isNotEmpty())
            socket->setMulticastLoopbackEnabled (false);

        lastError.clear();
        quit.store (false);
        worker = std::thread ([this] { pump(); });
        return true;
    }

    void stop()
    {
        quit.store (true);
        if (socket != nullptr) socket->shutdown();
        if (worker.joinable()) worker.join();
        if (socket != nullptr && group.isNotEmpty()) socket->leaveMulticast (group);
        socket = nullptr;
    }

    bool running() const noexcept { return worker.joinable(); }
    int  channel() const noexcept { return chan; }
    juce::String address() const { return group; }
    juce::String error() const { return lastError; }
    int  packets() const noexcept { return packetCount.load(); }
    bool receiving() const noexcept { return packetCount.load() > lastSeen; }

    /** Chamado pela interface uma vez por segundo, para dizer se ha fluxo. */
    bool pollAlive() noexcept
    {
        const int now = packetCount.load();
        const bool alive = now > lastSeen;
        lastSeen = now;
        return alive;
    }

private:
    void pump()
    {
        std::vector<char> buf (2048);
        std::vector<float> l (512), r (512);

        while (! quit.load())
        {
            if (socket == nullptr) return;
            if (socket->waitUntilReady (true, 200) <= 0) continue;

            juce::String from; int fromPort = 0;
            const int n = socket->read (buf.data(), int (buf.size()), false, from, fromPort);
            if (n <= kRtpHeader) continue;

            // cabecalho RTP fixo de 12 bytes; extensoes sao raras aqui e o bit
            // de extensao diz se ha mais. Ignoramos CSRC, que o Livewire nao usa.
            const auto* p = reinterpret_cast<const unsigned char*> (buf.data());
            int offset = kRtpHeader + 4 * (p[0] & 0x0f);
            if ((p[0] & 0x10) != 0 && n > offset + 4)          // extensao presente
                offset += 4 + 4 * ((p[offset + 2] << 8) | p[offset + 3]);
            if (n <= offset) continue;

            const int payload = n - offset;
            const int frames = payload / 6;                     // 2 canais x 3 bytes
            if (frames <= 0) continue;

            if (int (l.size()) < frames) { l.resize (size_t (frames)); r.resize (size_t (frames)); }

            const unsigned char* d = p + offset;
            for (int i = 0; i < frames; ++i)
            {
                l[size_t (i)] = sample24 (d);     d += 3;
                r[size_t (i)] = sample24 (d);     d += 3;
            }

            left .push (l.data(), frames);
            right.push (r.data(), frames);
            packetCount.fetch_add (1);
        }
    }

    /** L24 big-endian com sinal -> float -1..1. */
    static float sample24 (const unsigned char* d) noexcept
    {
        int v = (int (d[0]) << 16) | (int (d[1]) << 8) | int (d[2]);
        if (v & 0x800000) v -= 0x1000000;                       // extensao de sinal
        return float (v) * (1.0f / 8388608.0f);
    }

    static constexpr int kPort = 5004;
    static constexpr int kRtpHeader = 12;

    mesa::AsyncSource& left;
    mesa::AsyncSource& right;
    std::unique_ptr<juce::DatagramSocket> socket;
    std::thread worker;
    std::atomic<bool> quit { true };
    std::atomic<int> packetCount { 0 };
    int lastSeen = 0, chan = 0;
    juce::String group, lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivewireReceiver)
};
