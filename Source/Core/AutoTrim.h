#pragma once
#include "DspUtil.h"
#include <atomic>

namespace mesa {

struct AutoTrimParams
{
    std::atomic<bool>  enabled       { false };
    std::atomic<float> targetDb      { -20.0f };  // nivel RMS desejado
    std::atomic<float> maxGainDb     {  18.0f };
    std::atomic<float> minGainDb     { -18.0f };
    std::atomic<float> gateDb        { -45.0f };  // abaixo disso nao corrige (silencio/ruido)
    std::atomic<float> speedDbPerSec {   3.0f };  // limite de variacao
};

/** Ganho automatico estrutural do canal — nao e um plugin.
    Roda uma vez por bloco, a partir do RMS medido na entrada. */
class AutoTrim
{
public:
    void prepare (double sr) noexcept { sampleRate = sr; gainDb = 0.0f; }

    float update (float inputRmsDb, int numSamples, const AutoTrimParams& p) noexcept
    {
        const float step = p.speedDbPerSec.load (std::memory_order_relaxed)
                         * float (numSamples) / float (sampleRate);

        if (! p.enabled.load (std::memory_order_relaxed))
        {
            gainDb -= std::clamp (gainDb, -step, step);   // volta a 0 dB suavemente
            return gainDb;
        }

        if (inputRmsDb > p.gateDb.load (std::memory_order_relaxed))
        {
            const float err = p.targetDb.load (std::memory_order_relaxed) - (inputRmsDb + gainDb);
            gainDb += std::clamp (err, -step, step);
            gainDb  = std::clamp (gainDb,
                                  p.minGainDb.load (std::memory_order_relaxed),
                                  p.maxGainDb.load (std::memory_order_relaxed));
        }

        return gainDb;
    }

    float currentDb() const noexcept { return gainDb; }

private:
    double sampleRate = 48000.0;
    float  gainDb = 0.0f;
};

} // namespace mesa
