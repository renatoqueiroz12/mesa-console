#pragma once
#include "Channel.h"
#include <atomic>
#include <vector>

namespace mesa {

enum class TriggerState { Idle = 0, Candidate, Active, Cooldown };

inline const char* triggerStateName (TriggerState s) noexcept
{
    switch (s)
    {
        case TriggerState::Idle:      return "IDLE";
        case TriggerState::Candidate: return "CANDIDATE";
        case TriggerState::Active:    return "ACTIVE";
        case TriggerState::Cooldown:  return "COOLDOWN";
    }
    return "?";
}

struct TriggerEvent
{
    enum class Type { None = 0, Fired, Released, Rejected };
    Type  type = Type::None;
    int   channel = -1;
    int   camera = 0;
    float levelDb = kMinusInfDb;
    float durationMs = 0.0f;      // quanto durou a condicao (util no log de rejeicao)
};

/** Maquina de estados de um canal.
    Nao e "if nivel > threshold": exige permanencia, usa histerese para sair,
    e respeita cooldown. E o que separa fala de estalo, respiracao e pancada. */
class ChannelTrigger
{
public:
    void reset() noexcept { state = TriggerState::Idle; timer = 0.0f; below = 0.0f; }

    TriggerEvent update (int channelIndex, float levelDb, const TriggerParams& p,
                         bool dominant, bool channelOpen, float blockMs) noexcept
    {
        TriggerEvent ev;
        ev.channel = channelIndex;
        ev.camera  = p.camera.load (std::memory_order_relaxed);
        ev.levelDb = levelDb;

        if (! p.enabled.load (std::memory_order_relaxed) || ! channelOpen)
        {
            reset();
            publish();
            return ev;
        }

        const float thrOn  = p.thresholdDb.load (std::memory_order_relaxed);
        const float thrOff = thrOn - p.hysteresisDb.load (std::memory_order_relaxed);

        switch (state)
        {
            case TriggerState::Idle:
                if (levelDb > thrOn && dominant) { state = TriggerState::Candidate; timer = 0.0f; }
                break;

            case TriggerState::Candidate:
                timer += blockMs;
                if (levelDb < thrOn)
                {
                    ev.type = TriggerEvent::Type::Rejected;
                    ev.durationMs = timer;
                    reset();
                }
                else if (timer >= p.triggerMs.load (std::memory_order_relaxed))
                {
                    ev.type = TriggerEvent::Type::Fired;
                    ev.durationMs = timer;
                    state = TriggerState::Active;
                    below = 0.0f;
                }
                break;

            case TriggerState::Active:
                if (levelDb < thrOff)
                {
                    below += blockMs;
                    if (below >= p.releaseMs.load (std::memory_order_relaxed))
                    {
                        ev.type = TriggerEvent::Type::Released;
                        state = TriggerState::Cooldown;
                        timer = p.cooldownMs.load (std::memory_order_relaxed);
                    }
                }
                else below = 0.0f;
                break;

            case TriggerState::Cooldown:
                timer -= blockMs;
                if (timer <= 0.0f) reset();
                break;
        }

        publish();
        return ev;
    }

    TriggerState current() const noexcept
    { return TriggerState (published.load (std::memory_order_relaxed)); }
    float elapsedMs() const noexcept { return publishedTimer.load (std::memory_order_relaxed); }

private:
    void publish() noexcept
    {
        published.store (int (state), std::memory_order_relaxed);
        publishedTimer.store (timer, std::memory_order_relaxed);
    }

    TriggerState state = TriggerState::Idle;
    float timer = 0.0f, below = 0.0f;
    std::atomic<int>   published { int (TriggerState::Idle) };
    std::atomic<float> publishedTimer { 0.0f };
};

} // namespace mesa
