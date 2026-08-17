#pragma once
#include "Dsp.h"
#include <memory>
#include <array>
#include <vector>

namespace mesa {

enum class DspType { Eq = 0, Compressor, Gate, DeEsser, Limiter, NumTypes };

inline const char* dspTypeName (int t) noexcept
{
    switch (DspType (t))
    {
        case DspType::Eq:         return "EQ 3 bandas";
        case DspType::Compressor: return "Compressor";
        case DspType::Gate:       return "Gate";
        case DspType::DeEsser:    return "De-esser";
        case DspType::Limiter:    return "Limiter";
        default:                  return "?";
    }
}

/** Cadeia de processamento do canal, com ordem configuravel.
    A troca de ordem NAO usa lock: monta-se a nova ordem no buffer inativo e
    publica-se com um store atomico. O callback de audio le sempre uma ordem
    completa e valida — nunca uma lista pela metade. */
class DspRack
{
public:
    static constexpr int kMaxSlots = int (DspType::NumTypes);

    void prepare (double sampleRate, int maxBlockSize)
    {
        eq   = std::make_unique<Eq3>();
        comp = std::make_unique<Compressor>();
        gate = std::make_unique<Gate>();
        dees = std::make_unique<DeEsser>();
        lim  = std::make_unique<Limiter>();

        procs[int (DspType::Eq)]         = eq.get();
        procs[int (DspType::Compressor)] = comp.get();
        procs[int (DspType::Gate)]       = gate.get();
        procs[int (DspType::DeEsser)]    = dees.get();
        procs[int (DspType::Limiter)]    = lim.get();

        for (auto* p : procs) p->prepare (sampleRate, maxBlockSize);

        // ordem padrao de console: gate, EQ, compressor, de-esser, limiter
        setOrder ({ int (DspType::Gate), int (DspType::Eq), int (DspType::Compressor),
                    int (DspType::DeEsser), int (DspType::Limiter) });
        for (auto& e : enabled) e.store (false);
    }

    /** thread de UI */
    void setOrder (const std::vector<int>& newOrder)
    {
        const int next = 1 - active.load (std::memory_order_relaxed);
        int k = 0;
        for (int t : newOrder)
            if (t >= 0 && t < kMaxSlots && k < kMaxSlots) orders[size_t (next)][size_t (k++)] = t;
        counts[size_t (next)] = k;
        active.store (next, std::memory_order_release);   // publicacao atomica
    }

    void setEnabled (DspType t, bool on) noexcept
    { enabled[size_t (t)].store (on, std::memory_order_relaxed); }
    bool isEnabled (DspType t) const noexcept
    { return enabled[size_t (t)].load (std::memory_order_relaxed); }

    std::vector<int> order() const
    {
        const int a = active.load (std::memory_order_acquire);
        return std::vector<int> (orders[size_t (a)].begin(),
                                 orders[size_t (a)].begin() + counts[size_t (a)]);
    }

    /** thread de audio */
    void process (float* x, int n) noexcept
    {
        const int a = active.load (std::memory_order_acquire);
        const int c = counts[size_t (a)];
        for (int i = 0; i < c; ++i)
        {
            const int t = orders[size_t (a)][size_t (i)];
            if (enabled[size_t (t)].load (std::memory_order_relaxed))
                procs[size_t (t)]->process (x, n);
        }
    }

    bool anyEnabled() const noexcept
    {
        for (auto& e : enabled) if (e.load (std::memory_order_relaxed)) return true;
        return false;
    }

    Eq3&        equaliser()  noexcept { return *eq; }
    Compressor& compressor() noexcept { return *comp; }
    Gate&       noiseGate()  noexcept { return *gate; }
    DeEsser&    deEsser()    noexcept { return *dees; }
    Limiter&    limiter()    noexcept { return *lim; }

private:
    std::unique_ptr<Eq3> eq; std::unique_ptr<Compressor> comp;
    std::unique_ptr<Gate> gate; std::unique_ptr<DeEsser> dees;
    std::unique_ptr<Limiter> lim;

    std::array<IDspProcessor*, kMaxSlots> procs {};
    std::array<std::atomic<bool>, kMaxSlots> enabled {};
    std::array<std::array<int, kMaxSlots>, 2> orders {};
    std::array<int, 2> counts { 0, 0 };
    std::atomic<int> active { 0 };
};

} // namespace mesa
