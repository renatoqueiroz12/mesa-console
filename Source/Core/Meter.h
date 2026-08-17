#pragma once
#include "DspUtil.h"
#include <atomic>

namespace mesa {

/** Medicao de pico e RMS com balistica.
    process() roda na thread de audio; peakDb()/rmsDb() podem ser lidos pela UI. */
class Meter
{
public:
    void prepare (double sampleRate) noexcept
    {
        peakRelease = onePoleCoef (650.0f, sampleRate);   // queda do pico, para o olho
        rmsCoef     = onePoleCoef (300.0f, sampleRate);   // janela do RMS
        fastAttack  = onePoleCoef (5.0f,  sampleRate);    // envelope de deteccao
        fastRelease = onePoleCoef (60.0f, sampleRate);    // rapido: o trigger precisa disso
        peakEnv = rmsEnv = fastEnv = 0.0f;
        publish();
    }

    void process (const float* x, int n) noexcept
    {
        float p = peakEnv, r = rmsEnv, f = fastEnv;

        for (int i = 0; i < n; ++i)
        {
            const float a = std::fabs (x[i]);
            p = a > p ? a : peakRelease * p;

            const float sq = x[i] * x[i];
            r = sq + rmsCoef * (r - sq);

            f = a > f ? a + fastAttack * (f - a) : a + fastRelease * (f - a);

            if (a >= 0.999f)
                clipFlag.store (true, std::memory_order_relaxed);
        }

        peakEnv = p; rmsEnv = r; fastEnv = f;
        publish();
    }

    void silence() noexcept { peakEnv = rmsEnv = fastEnv = 0.0f; publish(); }

    float peakDb() const noexcept { return peakDbAtomic.load (std::memory_order_relaxed); }
    float rmsDb()  const noexcept { return rmsDbAtomic .load (std::memory_order_relaxed); }

    /** Envelope de deteccao: ataque de 5 ms, queda de 60 ms.
        E este que o Audio Trigger usa — o pico, com 650 ms de queda, mantinha o
        gatilho armado muito depois de a pessoa parar de falar. */
    float fastDb() const noexcept { return fastDbAtomic.load (std::memory_order_relaxed); }

    bool clipped (bool clear = true) noexcept
    {
        if (! clear) return clipFlag.load (std::memory_order_relaxed);
        return clipFlag.exchange (false, std::memory_order_relaxed);
    }

private:
    void publish() noexcept
    {
        peakDbAtomic.store (gainToDb (peakEnv), std::memory_order_relaxed);
        rmsDbAtomic .store (gainToDb (std::sqrt (rmsEnv)), std::memory_order_relaxed);
        fastDbAtomic.store (gainToDb (fastEnv), std::memory_order_relaxed);
    }

    float peakRelease = 0.0f, rmsCoef = 0.0f, fastAttack = 0.0f, fastRelease = 0.0f;
    float peakEnv = 0.0f, rmsEnv = 0.0f, fastEnv = 0.0f;
    std::atomic<float> peakDbAtomic { kMinusInfDb };
    std::atomic<float> rmsDbAtomic  { kMinusInfDb };
    std::atomic<float> fastDbAtomic { kMinusInfDb };
    std::atomic<bool>  clipFlag     { false };
};

} // namespace mesa
