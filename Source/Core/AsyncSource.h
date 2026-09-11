#pragma once
#include <chrono>
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
    void push (const float* samples, int n) noexcept
    {
        ring.write (samples, n);
        empurradas.fetch_add (n, std::memory_order_relaxed);
    }

    /** Amostras que CHEGARAM por segundo, medidas de verdade.

        A taxa que a fonte declara e a que ela entrega podem divergir, e quando
        divergem o audio sai picotado sem que buraco ou falha acusem nada: a
        fila entrega o que tem, so que em menos quantidade do que o tempo pede.
        Foi assim com um dispositivo que declarava 48000 e entregava 14000. */
    double amostrasPorSegundo() noexcept
    {
        // relogio padrao, e nao o do JUCE: o motor compila e e testado sem
        // ele, e uma dependencia a toa aqui quebraria a suite
        using Relogio = std::chrono::steady_clock;
        const double agora = double (std::chrono::duration_cast<std::chrono::milliseconds> (
                                         Relogio::now().time_since_epoch()).count());
        const long long total = empurradas.load (std::memory_order_relaxed);

        if (ultimaMedidaMs <= 0.0) { ultimaMedidaMs = agora; ultimoTotal = total; return 0.0; }

        const double dt = (agora - ultimaMedidaMs) / 1000.0;
        if (dt < 0.5) return ultimaTaxa;

        ultimaTaxa = double (total - ultimoTotal) / dt;
        ultimaMedidaMs = agora;
        ultimoTotal = total;
        return ultimaTaxa;
    }

    /** Alguem consumiu esta fila desde a ultima verificacao?

        Um dispositivo estereo cria fila para os DOIS canais, mas normalmente
        so um esta ligado a um fader. A fila orfa enchia e transbordava para
        sempre — inofensivo para o audio, mas enchia o diagnostico de milhares
        de descartes e escondia problema de verdade. Quem entrega usa isto para
        nao alimentar fila que ninguem le. */
    bool temConsumidor() noexcept
    {
        const int agora = pulls.load (std::memory_order_relaxed);
        const bool houve = agora != ultimoPullVisto;
        ultimoPullVisto = agora;
        return houve;
    }

    /** thread de audio: devolve sempre um bloco valido de n amostras.

        Aqui mora a correcao de relogio. A fonte tem cristal proprio e nunca
        corre exatamente na nossa taxa; sem corrigir, a fila deriva ate estourar
        ou secar, e o resultado e um estalo em intervalo regular — o defeito
        mais dificil de diagnosticar depois, porque so aparece depois de muito
        tempo rodando. */
    const float* pull (int n) noexcept
    {
        pulls.fetch_add (1, std::memory_order_relaxed);
        // Trava de seguranca: pedir mais do que o buffer comporta escreveria
        // fora dele e corromperia memoria. Aconteceu de verdade — o
        // transmissor Livewire puxa 240 amostras por pacote e a fila fora
        // criada com o bloco da placa, que na UMC e 8.
        if (n > int (scratch.size()))
        {
            overflowPulls.fetch_add (1, std::memory_order_relaxed);
            n = int (scratch.size());
        }
        if (n <= 0) return scratch.data();

        connected.store (ring.used() > 0, std::memory_order_relaxed);

        if (! correctDrift)
        {
            ring.read (scratch.data(), n);
            return scratch.data();
        }

        const double blocksPerSec = sr / std::max (1, block);
        // razao total = diferenca conhecida de taxa x correcao fina de deriva
        const double ratio = nominal * driftCtl.update (double (ring.fillRatio()), blocksPerSec);

        // pullOne le uma amostra da fila. Contamos as que faltaram DE FATO:
        // a verificacao anterior olhava so o inicio do bloco, e a fila
        // esvaziava no meio — o contador dizia zero enquanto o audio saia
        // cheio de buracos de poucos milissegundos.
        int faltando = 0;
        resampler.process (scratch.data(), n, ratio, [this, &faltando]() noexcept
        {
            float v = 0.0f;
            if (! ring.read (&v, 1)) ++faltando;
            return v;
        });
        if (faltando > 0) amostrasFaltando.fetch_add (faltando, std::memory_order_relaxed);
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
    /** Quantas vezes alguem pediu mais do que a fila comporta. Diferente de
        zero significa fila dimensionada errado por quem a criou. */
    int  badPulls() const noexcept { return overflowPulls.load (std::memory_order_relaxed); }
    /** Blocos em que a fila nao tinha material suficiente. Este e o numero que
        importa: um por bloco, na escala do que o ouvido percebe. */
    int  faltas() const noexcept { return faltasNoBloco.load (std::memory_order_relaxed); }
    /** Amostras que a fila nao tinha na hora de entregar. Vira buraco no audio:
        e o numero que corresponde ao que se ouve. */
    int  amostrasEmFalta() const noexcept { return amostrasFaltando.load (std::memory_order_relaxed); }
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
    std::atomic<int> overflowPulls { 0 };
    std::atomic<int> faltasNoBloco { 0 };
    std::atomic<int> pulls { 0 };
    std::atomic<int> amostrasFaltando { 0 };
    std::atomic<long long> empurradas { 0 };
    double ultimaMedidaMs = 0.0, ultimaTaxa = 0.0;
    long long ultimoTotal = 0;
    int ultimoPullVisto = 0;
    std::atomic<bool> connected { false };
};

} // namespace mesa
