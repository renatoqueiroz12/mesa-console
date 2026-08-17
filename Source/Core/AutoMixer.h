#pragma once
#include "DspUtil.h"
#include <atomic>
#include <cmath>

namespace mesa {

/** Nivelador de fader ("automix" no sentido de operador automatico).

    Move o FADER — nao um ganho escondido — para manter o canal num alvo de
    nivel. O operador ve o fader andando, que e metade do valor da funcao:
    ele entende o que a mesa esta fazendo e pode assumir a qualquer momento.

    Por que medir ANTES do fader: medir depois fecharia uma malha (sobe o
    fader, le mais alto, abaixa o fader) que oscila. Medindo o sinal pre-fader
    e calculando o ganho necessario, o calculo e direto e nao realimenta.

    Por que o piso: sem ele, na pausa da fala o nivelador persegue o ruido de
    sala ate o teto, e a primeira silaba seguinte estoura. Abaixo do piso ele
    congela onde esta.

    Nao confundir com automix por compartilhamento de ganho (Dugan), que abaixa
    microfones que nao estao falando. Sao funcoes diferentes. */
struct AutoMixParams
{
    std::atomic<bool>  enabled     { false };
    std::atomic<float> targetDb    { -18.0f };  // alvo de RMS pos-fader
    std::atomic<float> maxFaderDb  {  10.0f };  // ate onde pode subir
    std::atomic<float> minFaderDb  { -30.0f };  // ate onde pode descer
    std::atomic<float> floorDb     { -45.0f };  // abaixo disso, congela
    std::atomic<float> speedDbPerSec { 6.0f };  // quao rapido corrige
};

class AutoMixer
{
public:
    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        block = maxBlockSize;
        rms.prepare (sampleRate, 300.0f);   // janela longa: nivel de fala, nao silaba
        current = 0.0f;
        estimate = -120.0f;
        primed = false;
    }

    void reset() noexcept { primed = false; }

    /** Roda uma vez por bloco, com o sinal PRE-fader ja processado pelo DSP.
        Devolve a posicao de fader em dB que o canal deve assumir.
        Se estiver desligado ou o canal fechado, devolve o valor do operador. */
    float process (const AutoMixParams& p, const float* preFader, int n,
                   float operatorFaderDb, bool channelOn) noexcept
    {
        if (! p.enabled.load (std::memory_order_relaxed) || ! channelOn)
        {
            current = operatorFaderDb;   // acompanha, para nao dar salto ao ligar
            primed = true;
            return operatorFaderDb;
        }

        const float rmsDb = rms.processBlock (preFader, n);

        // Seguidor com subida rapida e QUEDA LENTA. Sem isso, no fim de cada
        // frase o nivel cai e o nivelador sobe o fader perseguindo o que esta
        // sumindo — a proxima palavra entra estourada. Segurando a estimativa,
        // a pausa simplesmente nao mexe em nada.
        if (rmsDb > estimate) estimate = rmsDb;
        else estimate -= kDecayDbPerSec * float (n) / float (sr);
        const float levelDb = estimate;

        if (! primed) { current = operatorFaderDb; primed = true; }

        // O piso e checado no nivel INSTANTANEO, nao na estimativa: se nao ha
        // som agora, o fader nao anda. A estimativa serve para dizer QUANTO
        // corrigir quando ha som — nao para decidir SE deve corrigir.
        // Segunda trava: so mexe quando o nivel de agora acompanha a estimativa.
        // Numa frase terminando, o instantaneo despenca abaixo dela e o fader
        // para na hora, em vez de subir atras do que esta acabando.
        const bool speaking = rmsDb > p.floorDb.load (std::memory_order_relaxed)
                           && rmsDb > estimate - kFallingDb;

        if (speaking)
        {
            const float target = p.targetDb.load (std::memory_order_relaxed);
            const float wanted = target - levelDb;   // ganho que falta, em dB

            const float lo = p.minFaderDb.load (std::memory_order_relaxed);
            const float hi = p.maxFaderDb.load (std::memory_order_relaxed);
            const float clamped = wanted < lo ? lo : (wanted > hi ? hi : wanted);

            // caminhada limitada: correcao instantanea seria bombeamento audivel
            const float step = p.speedDbPerSec.load (std::memory_order_relaxed)
                             * float (n) / float (sr);
            const float diff = clamped - current;
            if (diff >  step) current += step;
            else if (diff < -step) current -= step;
            else current = clamped;
        }
        return current;
    }

    float currentFaderDb() const noexcept { return current; }

private:
    /** RMS com janela longa, em dB. */
    struct RmsDetector
    {
        void prepare (double sampleRate, float windowMs)
        {
            coeff = std::exp (-1.0f / (float (sampleRate) * windowMs * 0.001f));
            acc = 0.0f;
        }
        float processBlock (const float* x, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float s = x[i] * x[i];
                acc = coeff * acc + (1.0f - coeff) * s;
            }
            return gainToDb (std::sqrt (acc < 1.0e-12f ? 1.0e-12f : acc));
        }
        float coeff = 0.0f, acc = 0.0f;
    };

    RmsDetector rms;
    double sr = 48000.0;
    int block = 0;
    static constexpr float kFallingDb     = 8.0f;   // margem antes de considerar "caindo"
    static constexpr float kDecayDbPerSec = 1.5f;   // quanto a estimativa cede na pausa
    float current = 0.0f;
    float estimate = -120.0f;
    bool primed = false;
};

} // namespace mesa
