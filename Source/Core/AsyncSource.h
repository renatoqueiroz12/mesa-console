#pragma once
#include "DspUtil.h"
#include "RateConverter.h"
#include <vector>
#include <atomic>
#include <cstring>
#include <string>
#include <algorithm>

namespace mesa {

/** Fila circular de um produtor e um consumidor, sem lock.
    Produtor: thread da rede (NDI, AES67, playout). Consumidor: callback de audio.
    Nenhum dos dois espera pelo outro — e isso que protege a thread de audio. */
class RingBuffer
{
public:
    void prepare (int capacitySamples)
    {
        buf.assign (size_t (capacitySamples), 0.0f);
        cap = capacitySamples;
        writePos.store (0); readPos.store (0);
    }

    int capacity() const noexcept { return cap; }

    /** Lado do CONSUMIDOR: quanto ha para ler (acquire no writePos). */
    int used() const noexcept
    {
        const int w = writePos.load (std::memory_order_acquire);
        const int r = readPos .load (std::memory_order_relaxed);
        return w - r;
    }

    /** Lado do PRODUTOR: quanto cabe (acquire no readPos — sem isso ele pode
        sobrescrever a regiao que o consumidor ainda esta copiando). */
    int free() const noexcept
    {
        const int w = writePos.load (std::memory_order_relaxed);
        const int r = readPos .load (std::memory_order_acquire);
        return cap - (w - r);
    }

    float fillRatio() const noexcept { return cap > 0 ? float (used()) / float (cap) : 0.0f; }

    /** Chamado pela thread da rede. Descarta o excedente em vez de bloquear. */
    int write (const float* src, int n) noexcept
    {
        const int room = free();
        const int toWrite = n < room ? n : room;
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < toWrite; ++i)
            buf[size_t ((w + i) % cap)] = src[i];
        writePos.store (w + toWrite, std::memory_order_release);
        if (toWrite < n) overflows.fetch_add (1, std::memory_order_relaxed);
        return toWrite;
    }

    /** Chamado pelo callback de audio. Se faltar amostra, completa com silencio
        e conta o underrun — nunca devolve lixo e nunca espera. */
    bool read (float* dst, int n) noexcept
    {
        const int avail = used();
        const int toRead = n < avail ? n : avail;
        int r = readPos.load (std::memory_order_relaxed);
        for (int i = 0; i < toRead; ++i)
            dst[i] = buf[size_t ((r + i) % cap)];
        for (int i = toRead; i < n; ++i)
            dst[i] = 0.0f;
        readPos.store (r + toRead, std::memory_order_release);
        if (toRead < n) { underruns.fetch_add (1, std::memory_order_relaxed); return false; }
        return true;
    }

    void reset() noexcept { writePos.store (0); readPos.store (0); }

    std::atomic<int> underruns { 0 }, overflows { 0 };

private:
    std::vector<float> buf;
    int cap = 0;
    std::atomic<int> writePos { 0 }, readPos { 0 };
};

/** Uma fonte de audio que chega por fora do driver: NDI, AES67, playout local.
    O relogio dela NAO e o relogio da placa. A fila absorve o jitter; a diferenca
    de clock e corrigida por quem entrega (o framesync do NDI faz isso) ou por
    reamostragem adaptativa guiada por targetFill. */
class AsyncSource
{
public:
    enum class Kind { Ndi = 0, Aes67, Local };

    /** fillFraction e o ponto de equilibrio da fila, e portanto a LATENCIA
        desta fonte: fila funda tolera rajada e sobrevive a jitter, mas atrasa.
        O controlador de deriva segura a fila exatamente nesse ponto. */
    void prepare (int maxBlockSize, int bufferBlocks = 8, double hostSampleRate = 48000.0,
                  double fillFraction = 0.5)
    {
        block = maxBlockSize;
        sr = hostSampleRate;
        nominal = 1.0;
        fill = std::min (0.8, std::max (0.1, fillFraction));
        ring.prepare (maxBlockSize * bufferBlocks);
        scratch.assign (size_t (maxBlockSize), 0.0f);
        // pre-carga: sem nada na fila o primeiro bloco ja da underrun
        std::vector<float> zeros (size_t (double (maxBlockSize * bufferBlocks) * fill), 0.0f);
        ring.write (zeros.data(), int (zeros.size()));
        target = ring.fillRatio();

        resampler.prepare();
        driftCtl.prepare (target);
        lost.store (false);
    }

    /** thread da rede / do dispositivo secundario */
    void push (const float* samples, int n) noexcept { ring.write (samples, n); }

    /** thread de audio: devolve sempre um bloco valido de n amostras.

        Aqui mora a correcao de relogio. A fonte tem cristal proprio e nunca
        corre exatamente na nossa taxa; sem corrigir, a fila deriva ate estourar
        ou secar, e o resultado e um estalo em intervalo regular — o defeito
        mais dificil de diagnosticar depois, porque so aparece depois de muito
        tempo rodando. */
    const float* pull (int n) noexcept
    {
        connected.store (ring.used() > 0, std::memory_order_relaxed);

        if (! correctDrift)
        {
            ring.read (scratch.data(), n);
            return scratch.data();
        }

        const double blocksPerSec = sr / std::max (1, block);
        // razao total = diferenca conhecida de taxa x correcao fina de deriva
        const double ratio = nominal * driftCtl.update (double (ring.fillRatio()), blocksPerSec);

        // pullOne le uma amostra da fila; se faltar, devolve silencio e conta
        resampler.process (scratch.data(), n, ratio, [this]() noexcept
        {
            float v = 0.0f;
            ring.read (&v, 1);
            return v;
        });
        return scratch.data();
    }

    /** Latencia que esta fila esta acrescentando, em milissegundos. */
    double latencyMs() const noexcept
    {
        return sr > 0.0 ? 1000.0 * ring.used() / sr : 0.0;
    }

    /** Razao FIXA entre a taxa da fonte e a nossa.

        Sem isto, uma placa em 44100 alimentando uma mesa em 48000 exigiria
        correcao de ~9%, muito alem do teto de 400 ppm do controlador — a fila
        vive estourando e o audio sai distorcido. O controlador so deve cuidar
        da deriva de cristal, que e minuscula; a diferenca GRANDE de taxa e
        conhecida e entra aqui. */
    void setSourceSampleRate (double sourceRate) noexcept
    {
        nominal = (sourceRate > 0.0 && sr > 0.0) ? sourceRate / sr : 1.0;
    }
    double sourceRatio() const noexcept { return nominal; }

    /** Desligar so faz sentido quando quem entrega ja corrige o relogio —
        o framesync do NDI, por exemplo. */
    void setDriftCorrection (bool on) noexcept { correctDrift = on; }

    double correctionPpm() const noexcept { return driftCtl.ppm(); }
    bool   driftSettled()  const noexcept { return driftCtl.isSettled(); }

    /** Marcado quando o dispositivo some. O audio segue: a fila entrega
        silencio e o canal acusa ausencia de sinal. */
    void setLost (bool v) noexcept { lost.store (v, std::memory_order_relaxed); }
    bool isLost() const noexcept   { return lost.load (std::memory_order_relaxed); }

    /** Positivo = fila enchendo (relogio da fonte mais rapido que o da placa). */
    float drift() const noexcept { return ring.fillRatio() - target; }

    int  underruns() const noexcept { return ring.underruns.load (std::memory_order_relaxed); }
    int  overflows() const noexcept { return ring.overflows.load (std::memory_order_relaxed); }
    bool isConnected() const noexcept { return connected.load (std::memory_order_relaxed); }

    std::string name;
    Kind kind = Kind::Ndi;

private:
    RingBuffer ring;
    std::vector<float> scratch;
    int block = 0;
    double sr = 48000.0;
    double nominal = 1.0;
    double fill = 0.5;
    float target = 0.5f;
    VariableResampler resampler;
    DriftController driftCtl;
    bool correctDrift = true;
    std::atomic<bool> lost { false };
    std::atomic<bool> connected { false };
};

} // namespace mesa
