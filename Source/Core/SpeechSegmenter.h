#pragma once
#include "DspUtil.h"
#include <atomic>
#include <cmath>
#include <vector>

namespace mesa {

/** Corta o audio de um canal em trechos de FALA, para transcrever.

    Por que segmentar em vez de mandar tudo: transcritor trabalha por trecho,
    e trecho que comeca e termina no silencio sai muito melhor do que corte
    arbitrario a cada N segundos, que parte palavra ao meio. Alem disso,
    silencio nao vira arquivo — numa emissora, a maior parte do dia um canal
    esta fechado, e transcrever silencio custaria caro sem entregar nada.

    A deteccao reaproveita a ideia do trigger de camera: envelope com ataque
    rapido e queda lenta, permanencia para nao abrir em estalo, e um tempo de
    silencio para fechar. Nao e o mesmo objeto porque os tempos uteis sao bem
    diferentes: camera corta em 300 ms, fala util comeca em segundos.

    Roda no callback de audio, entao nao aloca: os buffers sao dimensionados
    no prepare e o trecho pronto sai por troca de ponteiro. */
class SpeechSegmenter
{
public:
    struct Params
    {
        std::atomic<bool>  enabled     { false };
        std::atomic<float> thresholdDb { -42.0f };  // acima disso conta como fala
        std::atomic<float> abreMs      { 250.0f };  // permanencia para comecar
        std::atomic<float> fechaMs     { 900.0f };  // silencio que encerra o trecho
        std::atomic<float> minTrechoMs { 800.0f };  // menor que isso nao vale transcrever
        /** Teto do trecho.

            Existe porque fala contínua — musica, locucao sem pausa, VT — nunca
            entrega o silencio que fecharia o trecho naturalmente. Sem teto, o
            primeiro arquivo so sairia quando a fonte calasse, o que pode
            demorar um programa inteiro.

            12 s e um meio-termo: longo o bastante para o reconhecedor ter
            contexto e curto o bastante para o texto chegar enquanto ainda
            interessa. Acima de 20 s, a espera pela transcricao ja incomoda. */
        std::atomic<float> maxTrechoMs { 12000.0f };
        std::atomic<float> preRollMs   { 300.0f };  // audio ANTES do inicio detectado
    };

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        env.prepare (sampleRate, 5.0f, 120.0f);
        // rapido, mas nao instantaneo: veja o comentario em process()
        envAbre.prepare (sampleRate, 3.0f, 30.0f);

        const int maxAmostras = int (sampleRate * 30.0);   // teto de 30 s
        atual.reserve (size_t (maxAmostras));
        pronto.reserve (size_t (maxAmostras));

        // pre-roll: guarda o passado recente para nao perder a primeira silaba,
        // que e justamente onde a deteccao ainda estava decidindo
        preRoll.assign (size_t (sampleRate * 1.0), 0.0f);
        preWrite = 0;

        atual.clear();
        pronto.clear();
        falando = false;
        acimaMs = abaixoMs = 0.0;
        juce_unused (maxBlockSize);
    }

    /** Chamado no callback com o sinal do canal. Devolve true quando um trecho
        acabou de ficar pronto para ser recolhido por takeTrecho(). */
    bool process (const Params& p, const float* x, int n) noexcept
    {
        if (! p.enabled.load (std::memory_order_relaxed)) { reset(); return false; }

        const double blocoMs = 1000.0 * double (n) / sr;
        const float limiar = p.thresholdDb.load (std::memory_order_relaxed);

        // DUAS medidas, de proposito.
        //
        // Para ABRIR usamos o nivel do bloco, que cai junto com o som. Se
        // usassemos o envelope — que tem queda lenta —, um estalo de 100 ms
        // continuaria "soando" por mais tempo do que durou e acabaria
        // acumulando os 250 ms de permanencia. Era o que acontecia: tosse
        // virava trecho.
        //
        // Para FECHAR usamos o envelope, justamente porque a queda lenta e o
        // que atravessa a pausa curta entre palavras sem picotar a frase.
        // Medida de abertura com um POUCO de memoria (queda em 30 ms).
        //
        // Antes era o RMS cru do bloco. Com buffer de 8 amostras — que e o que
        // a placa desta instalacao usa — o RMS despenca perto de zero a cada
        // cruzamento da onda; o contador de permanencia zerava nesses
        // mergulhos e nunca alcancava os 250 ms. O sinal estava 25 dB acima do
        // limiar e mesmo assim nao abria.
        //
        // 30 ms de queda atravessa o cruzamento e continua curto o bastante
        // para nao confundir estalo com fala: um ruido de 100 ms soma ~130 ms,
        // longe dos 250 exigidos.
        const float rapido = envAbre.processBlock (x, n);
        const float lento  = env.processBlock (x, n);
        const bool abrindo  = rapido > limiar;
        const bool sustenta = lento  > limiar;

        // pre-roll sempre girando, mesmo em silencio
        for (int i = 0; i < n; ++i)
        {
            preRoll[size_t (preWrite)] = x[i];
            preWrite = (preWrite + 1) % int (preRoll.size());
        }

        if (! falando)
        {
            acimaMs = abrindo ? acimaMs + blocoMs : 0.0;
            if (acimaMs >= p.abreMs.load (std::memory_order_relaxed))
            {
                falando = true;
                abaixoMs = 0.0;
                atual.clear();
                copiaPreRoll (p.preRollMs.load (std::memory_order_relaxed));
            }
            else return false;
        }

        atual.insert (atual.end(), x, x + n);
        abaixoMs = sustenta ? 0.0 : abaixoMs + blocoMs;

        const double duracaoMs = 1000.0 * double (atual.size()) / sr;
        const bool fechouPorSilencio = abaixoMs >= p.fechaMs.load (std::memory_order_relaxed);
        const bool fechouPorTamanho  = duracaoMs >= p.maxTrechoMs.load (std::memory_order_relaxed);

        if (! fechouPorSilencio && ! fechouPorTamanho) return false;

        falando = false;
        acimaMs = 0.0;

        if (duracaoMs < p.minTrechoMs.load (std::memory_order_relaxed))
        {
            atual.clear();      // curto demais: tosse, batida, "uhum"
            return false;
        }

        pronto.swap (atual);
        atual.clear();
        cortadoPorTamanho = fechouPorTamanho;
        return true;
    }

    /** Recolhe o trecho pronto. A interface chama fora do callback. */
    const std::vector<float>& trecho() const noexcept { return pronto; }
    void limpaTrecho() noexcept { pronto.clear(); }
    bool foiCortadoPorTamanho() const noexcept { return cortadoPorTamanho; }

    bool estaFalando() const noexcept { return falando; }
    double duracaoAtualMs() const noexcept { return 1000.0 * double (atual.size()) / sr; }

    void reset() noexcept
    {
        falando = false;
        acimaMs = abaixoMs = 0.0;
        atual.clear();
    }

private:
    void copiaPreRoll (float ms)
    {
        const int quantas = std::min (int (sr * double (ms) / 1000.0), int (preRoll.size()));
        for (int i = 0; i < quantas; ++i)
        {
            const int idx = (preWrite - quantas + i + int (preRoll.size() * 2))
                          % int (preRoll.size());
            atual.push_back (preRoll[size_t (idx)]);
        }
    }

    /** Envelope com ataque rapido e queda lenta, em dB. */
    struct Envelope
    {
        void prepare (double sampleRate, float ataqueMs, float quedaMs)
        {
            ca = std::exp (-1.0f / (float (sampleRate) * ataqueMs * 0.001f));
            cq = std::exp (-1.0f / (float (sampleRate) * quedaMs  * 0.001f));
            v = 0.0f;
        }
        float processBlock (const float* x, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = std::abs (x[i]);
                v = a > v ? ca * v + (1.0f - ca) * a
                          : cq * v + (1.0f - cq) * a;
            }
            return gainToDb (v < 1.0e-7f ? 1.0e-7f : v);
        }
        float ca = 0.0f, cq = 0.0f, v = 0.0f;
    };

    static void juce_unused (int) {}

    Envelope env, envAbre;
    double sr = 48000.0;
    std::vector<float> atual, pronto, preRoll;
    int preWrite = 0;
    bool falando = false, cortadoPorTamanho = false;
    double acimaMs = 0.0, abaixoMs = 0.0;
};

} // namespace mesa
