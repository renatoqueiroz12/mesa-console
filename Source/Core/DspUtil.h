#pragma once
#include <cmath>
#include <algorithm>

namespace mesa {

constexpr float kMinusInfDb = -100.0f;
constexpr float kPi = 3.14159265358979323846f;

inline float dbToGain (float db) noexcept
{
    return db <= kMinusInfDb ? 0.0f : std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float g) noexcept
{
    return g <= 1.0e-6f ? kMinusInfDb : 20.0f * std::log10 (g);
}

/** Coeficiente de um filtro de 1 polo para uma constante de tempo em ms. */
inline float onePoleCoef (float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f || sampleRate <= 0.0) return 0.0f;
    return std::exp (-1.0f / (float (sampleRate) * ms * 0.001f));
}

/** Ganho suavizado. setTargetDb pode ser chamado por bloco; next() por amostra.
    Sem alocacao, sem lock: seguro dentro do callback de audio. */
class SmoothedGain
{
public:
    void prepare (double sampleRate, float timeMs = 20.0f) noexcept
    {
        coef = onePoleCoef (timeMs, sampleRate);
        current = target;
    }

    void setTargetDb (float db) noexcept { target = dbToGain (db); }
    void snap()                 noexcept { current = target; }

    float next() noexcept
    {
        current = target + coef * (current - target);
        return current;
    }

    float currentGain() const noexcept { return current; }

private:
    float coef = 0.0f, current = 0.0f, target = 0.0f;
};

} // namespace mesa
