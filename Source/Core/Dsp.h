#pragma once
#include "DspUtil.h"
#include <atomic>
#include <cmath>

namespace mesa {

/** Abstracao de processador. O host nunca depende de um plugin especifico —
    VST3 entra por aqui, com a mesma interface dos internos. */
class IDspProcessor
{
public:
    virtual ~IDspProcessor() = default;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void process (float* x, int n) noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

// ------------------------------------------------------------------ biquad
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0.0f; }

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void peaking (double sr, float freq, float q, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * freq / float (sr);
        const float alpha = std::sin (w) / (2.0f * q);
        const float cw = std::cos (w);
        const float a0 = 1 + alpha / A;
        b0 = (1 + alpha * A) / a0; b1 = (-2 * cw) / a0; b2 = (1 - alpha * A) / a0;
        a1 = (-2 * cw) / a0;       a2 = (1 - alpha / A) / a0;
    }

    void lowShelf (double sr, float freq, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * freq / float (sr);
        const float cw = std::cos (w), sw = std::sin (w);
        const float alpha = sw / 2.0f * std::sqrt ((A + 1 / A) * (1 / 0.707f - 1) + 2);
        const float t = 2 * std::sqrt (A) * alpha;
        const float a0 = (A + 1) + (A - 1) * cw + t;
        b0 = A * ((A + 1) - (A - 1) * cw + t) / a0;
        b1 = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
        b2 = A * ((A + 1) - (A - 1) * cw - t) / a0;
        a1 = -2 * ((A - 1) + (A + 1) * cw) / a0;
        a2 = ((A + 1) + (A - 1) * cw - t) / a0;
    }

    void highShelf (double sr, float freq, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * freq / float (sr);
        const float cw = std::cos (w), sw = std::sin (w);
        const float alpha = sw / 2.0f * std::sqrt ((A + 1 / A) * (1 / 0.707f - 1) + 2);
        const float t = 2 * std::sqrt (A) * alpha;
        const float a0 = (A + 1) - (A - 1) * cw + t;
        b0 = A * ((A + 1) + (A - 1) * cw + t) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
        b2 = A * ((A + 1) + (A - 1) * cw - t) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cw) / a0;
        a2 = ((A + 1) - (A - 1) * cw - t) / a0;
    }
};

// ------------------------------------------------------------------ EQ
class Eq3 : public IDspProcessor
{
public:
    std::atomic<float> lowFreq { 120.0f }, lowGainDb { 0.0f };
    std::atomic<float> midFreq { 1000.0f }, midGainDb { 0.0f }, midQ { 1.0f };
    std::atomic<float> highFreq { 8000.0f }, highGainDb { 0.0f };

    void prepare (double sampleRate, int) override { sr = sampleRate; reset(); update(); }
    void reset() noexcept override { lo.reset(); mid.reset(); hi.reset(); }
    const char* name() const noexcept override { return "EQ 3 bandas"; }

    void process (float* x, int n) noexcept override
    {
        update();
        for (int i = 0; i < n; ++i)
            x[i] = hi.process (mid.process (lo.process (x[i])));
    }

private:
    void update() noexcept
    {
        const float lg = lowGainDb.load (std::memory_order_relaxed);
        const float mg = midGainDb.load (std::memory_order_relaxed);
        const float hg = highGainDb.load (std::memory_order_relaxed);
        if (lg != lastLow)  { lo.lowShelf (sr, lowFreq.load(), lg);  lastLow = lg; }
        if (mg != lastMid)  { mid.peaking (sr, midFreq.load(), midQ.load(), mg); lastMid = mg; }
        if (hg != lastHigh) { hi.highShelf (sr, highFreq.load(), hg); lastHigh = hg; }
    }
    Biquad lo, mid, hi;
    double sr = 48000.0;
    float lastLow = 1e9f, lastMid = 1e9f, lastHigh = 1e9f;
};

// ------------------------------------------------------------------ compressor
class Compressor : public IDspProcessor
{
public:
    std::atomic<float> thresholdDb { -18.0f }, ratio { 3.0f },
                       attackMs { 10.0f }, releaseMs { 120.0f }, makeupDb { 0.0f };

    void prepare (double sampleRate, int) override { sr = sampleRate; reset(); }
    void reset() noexcept override { env = 0.0f; gainDb = 0.0f; }
    const char* name() const noexcept override { return "Compressor"; }

    void process (float* x, int n) noexcept override
    {
        const float th = thresholdDb.load (std::memory_order_relaxed);
        const float rt = std::max (1.0f, ratio.load (std::memory_order_relaxed));
        const float mk = dbToGain (makeupDb.load (std::memory_order_relaxed));
        const float ac = onePoleCoef (attackMs.load (std::memory_order_relaxed), sr);
        const float rc = onePoleCoef (releaseMs.load (std::memory_order_relaxed), sr);

        for (int i = 0; i < n; ++i)
        {
            const float a = std::fabs (x[i]);
            env = a > env ? a + ac * (env - a) : a + rc * (env - a);

            const float lvl = gainToDb (env);
            const float over = lvl - th;
            const float target = over > 0.0f ? -over * (1.0f - 1.0f / rt) : 0.0f;
            gainDb = target + (over > 0.0f ? ac : rc) * (gainDb - target);

            x[i] *= dbToGain (gainDb) * mk;
        }
        reduction.store (gainDb, std::memory_order_relaxed);
    }

    std::atomic<float> reduction { 0.0f };   // dB de reducao, para o medidor

private:
    double sr = 48000.0;
    float env = 0.0f, gainDb = 0.0f;
};

// ------------------------------------------------------------------ gate
class Gate : public IDspProcessor
{
public:
    std::atomic<float> thresholdDb { -45.0f }, rangeDb { -60.0f },
                       attackMs { 2.0f }, holdMs { 120.0f }, releaseMs { 200.0f };

    void prepare (double sampleRate, int) override { sr = sampleRate; reset(); }
    void reset() noexcept override { env = 0.0f; gain = 0.0f; holdLeft = 0.0f; }
    const char* name() const noexcept override { return "Gate"; }

    void process (float* x, int n) noexcept override
    {
        const float th = dbToGain (thresholdDb.load (std::memory_order_relaxed));
        const float floorG = dbToGain (rangeDb.load (std::memory_order_relaxed));
        const float ac = onePoleCoef (attackMs.load (std::memory_order_relaxed), sr);
        const float rc = onePoleCoef (releaseMs.load (std::memory_order_relaxed), sr);
        const float holdSamples = holdMs.load (std::memory_order_relaxed) * 0.001f * float (sr);

        for (int i = 0; i < n; ++i)
        {
            const float a = std::fabs (x[i]);
            env = a > env ? a : a + 0.999f * (env - a);

            float target;
            if (env > th) { target = 1.0f; holdLeft = holdSamples; }
            else if (holdLeft > 0.0f) { target = 1.0f; holdLeft -= 1.0f; }
            else target = floorG;

            gain = target + (target > gain ? ac : rc) * (gain - target);
            x[i] *= gain;
        }
        open.store (gain > 0.5f, std::memory_order_relaxed);
    }

    std::atomic<bool> open { false };

private:
    double sr = 48000.0;
    float env = 0.0f, gain = 0.0f, holdLeft = 0.0f;
};

// ------------------------------------------------------------------ limiter
class Limiter : public IDspProcessor
{
public:
    std::atomic<float> ceilingDb { -1.0f }, releaseMs { 60.0f };

    void prepare (double sampleRate, int) override { sr = sampleRate; reset(); }
    void reset() noexcept override { gain = 1.0f; }
    const char* name() const noexcept override { return "Limiter"; }

    void process (float* x, int n) noexcept override
    {
        const float ceil = dbToGain (ceilingDb.load (std::memory_order_relaxed));
        const float rc = onePoleCoef (releaseMs.load (std::memory_order_relaxed), sr);

        for (int i = 0; i < n; ++i)
        {
            const float a = std::fabs (x[i] * gain);
            if (a > ceil) gain *= ceil / a;               // ataque instantaneo
            else          gain = 1.0f + rc * (gain - 1.0f);
            x[i] *= gain;
        }
    }

private:
    double sr = 48000.0;
    float gain = 1.0f;
};

// ------------------------------------------------------------------ de-esser
class DeEsser : public IDspProcessor
{
public:
    std::atomic<float> freq { 6500.0f }, thresholdDb { -25.0f }, ratio { 4.0f };

    void prepare (double sampleRate, int) override { sr = sampleRate; reset(); update(); }
    void reset() noexcept override { hp.reset(); env = 0.0f; }
    const char* name() const noexcept override { return "De-esser"; }

    void process (float* x, int n) noexcept override
    {
        update();
        const float th = thresholdDb.load (std::memory_order_relaxed);
        const float rt = std::max (1.0f, ratio.load (std::memory_order_relaxed));
        const float ac = onePoleCoef (1.0f, sr), rc = onePoleCoef (40.0f, sr);

        for (int i = 0; i < n; ++i)
        {
            const float side = std::fabs (hp.process (x[i]));   // deteccao so nas altas
            env = side > env ? side + ac * (env - side) : side + rc * (env - side);
            const float over = gainToDb (env) - th;
            if (over > 0.0f) x[i] *= dbToGain (-over * (1.0f - 1.0f / rt));
        }
    }

private:
    void update() noexcept
    {
        const float f = freq.load (std::memory_order_relaxed);
        if (f != lastF) { hp.peaking (sr, f, 1.2f, 12.0f); lastF = f; }
    }
    Biquad hp;
    double sr = 48000.0;
    float env = 0.0f, lastF = 0.0f;
};

} // namespace mesa
