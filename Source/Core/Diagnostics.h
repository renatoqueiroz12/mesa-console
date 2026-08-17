#pragma once
#include "DspUtil.h"
#include <atomic>

namespace mesa {

/** "Esse canal esta chegando alguma coisa?"
    Responde SEM depender do trigger: presenca de sinal, tempo de silencio,
    clipping e pico retido. E o primeiro degrau da validacao — antes de discutir
    threshold, e preciso saber se ha audio no cabo. */
class SignalPresence
{
public:
    std::atomic<float> presentThresholdDb { -50.0f };   // acima disso, ha sinal
    std::atomic<float> silenceAlarmMs     { 3000.0f };  // silencio que vira alarme

    void reset() noexcept
    {
        silence = 0.0f; active = 0.0f; peakHold = kMinusInfDb;
        publish(); everSeen.store (false, std::memory_order_relaxed);
        clipCount.store (0, std::memory_order_relaxed);
    }

    void update (float levelDb, float blockMs, bool clipped) noexcept
    {
        if (levelDb > presentThresholdDb.load (std::memory_order_relaxed))
        {
            silence = 0.0f;
            active += blockMs;
            everSeen.store (true, std::memory_order_relaxed);
        }
        else
        {
            silence += blockMs;
            active = 0.0f;
        }

        if (levelDb > peakHold) peakHold = levelDb;
        if (clipped) clipCount.fetch_add (1, std::memory_order_relaxed);
        publish();
    }

    bool  hasSignal() const noexcept { return present.load (std::memory_order_relaxed); }
    bool  everHadSignal() const noexcept { return everSeen.load (std::memory_order_relaxed); }
    float silenceMs() const noexcept { return silenceMsA.load (std::memory_order_relaxed); }
    float peakHoldDb() const noexcept { return peakHoldA.load (std::memory_order_relaxed); }
    int   clips() const noexcept { return clipCount.load (std::memory_order_relaxed); }

    /** Alarme de canal mudo: ja teve sinal e parou por tempo demais. */
    bool isSilentAlarm() const noexcept
    {
        return everHadSignal()
            && silenceMs() > silenceAlarmMs.load (std::memory_order_relaxed);
    }

private:
    void publish() noexcept
    {
        present   .store (silence < 200.0f, std::memory_order_relaxed);
        silenceMsA.store (silence, std::memory_order_relaxed);
        peakHoldA .store (peakHold, std::memory_order_relaxed);
    }

    float silence = 1e9f, active = 0.0f, peakHold = kMinusInfDb;
    std::atomic<bool>  present { false }, everSeen { false };
    std::atomic<float> silenceMsA { 1e9f }, peakHoldA { kMinusInfDb };
    std::atomic<int>   clipCount { 0 };
};

/** Calibrador de threshold.
    Em vez de chutar -35 dBFS, mede: escuta o canal por alguns segundos, separa
    o piso de ruido da fala e sugere um threshold no meio, com margem.
    Assim o mesmo microfone funciona em estudios diferentes. */
class ThresholdCalibrator
{
public:
    void start() noexcept
    {
        running.store (true, std::memory_order_relaxed);
        elapsed = 0.0f; floorDb = 0.0f; speechDb = kMinusInfDb; first = true;
    }
    void stop() noexcept { running.store (false, std::memory_order_relaxed); }
    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    void update (float levelDb, float blockMs) noexcept
    {
        if (! isRunning()) return;
        elapsed += blockMs;

        if (first) { floorDb = levelDb; speechDb = levelDb; first = false; }

        // piso: desce rapido, sobe muito devagar (segue o silencio)
        if (levelDb < floorDb) floorDb = levelDb;
        else floorDb += 0.00002f * blockMs * (levelDb - floorDb);

        // fala: sobe rapido, desce devagar (segue os picos)
        if (levelDb > speechDb) speechDb = levelDb;
        else speechDb += 0.00001f * blockMs * (levelDb - speechDb);

        floorA .store (floorDb,  std::memory_order_relaxed);
        speechA.store (speechDb, std::memory_order_relaxed);
        elapsedA.store (elapsed, std::memory_order_relaxed);
    }

    float noiseFloorDb() const noexcept { return floorA.load (std::memory_order_relaxed); }
    float speechLevelDb() const noexcept { return speechA.load (std::memory_order_relaxed); }
    float elapsedMs() const noexcept { return elapsedA.load (std::memory_order_relaxed); }

    /** Precisa de tempo e de uma separacao clara entre ruido e fala. */
    bool ready() const noexcept
    {
        return elapsedMs() > 5000.0f
            && (speechLevelDb() - noiseFloorDb()) > 12.0f;
    }

    /** Threshold sugerido: acima do ruido com folga, abaixo da fala com folga. */
    float suggestedThresholdDb() const noexcept
    {
        const float f = noiseFloorDb(), s = speechLevelDb();
        const float mid = f + (s - f) * 0.45f;
        return std::clamp (mid, f + 8.0f, s - 8.0f);
    }

private:
    std::atomic<bool> running { false };
    float elapsed = 0.0f, floorDb = 0.0f, speechDb = kMinusInfDb;
    bool  first = true;
    std::atomic<float> floorA { kMinusInfDb }, speechA { kMinusInfDb }, elapsedA { 0.0f };
};

} // namespace mesa
