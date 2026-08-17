#pragma once
#include "Channel.h"
#include <memory>
#include <algorithm>

namespace mesa {

constexpr int kNumBuses = 4;   // PGM 1..4

struct AutomationParams
{
    std::atomic<bool>  enabled     { true };
    std::atomic<bool>  testMode    { false };
    std::atomic<bool>  dominance   { true };
    std::atomic<float> dominanceDb { 6.0f };
    std::atomic<float> minShotMs   { 1200.0f };
    std::atomic<int>   wideCamera  { 0 };
    /** Silencio necessario antes de voltar ao plano geral. Sem isso a mesa
        volta ao BG na primeira respirada entre frases. */
    std::atomic<float> wideDelayMs { 3000.0f };
};

struct BusParams
{
    std::atomic<float> gainDb     { 0.0f };
    std::atomic<int>   outputPair { -1 };   // par de saidas do dispositivo; -1 = nao roteado
};

enum class MonitorSource { Pgm1 = 0, Pgm2, Pgm3, Pgm4, Ext1, Ext2 };

/** Seção de monitoração: monitor do controle, fone e CUE.
    Os mutes NAO sao manuais — seguem o tipo de fonte dos canais abertos. */
struct MonitorParams
{
    std::atomic<int>   source      { int (MonitorSource::Pgm1) };
    std::atomic<float> monitorDb   { -18.0f };
    std::atomic<float> phonesDb    { -15.0f };
    std::atomic<float> cueDb       { -12.0f };
    std::atomic<float> studioDb    { -18.0f };
    std::atomic<float> dimDb       { -12.0f };   // aplicado durante talkback
    std::atomic<bool>  cueToPhones { true };     // CUE sobrepoe o fone quando ativo
    std::atomic<bool>  talkToStudio{ false };

    std::atomic<int> ext1Pair    { -1 };   // par de ENTRADAS para a externa 1
    std::atomic<int> ext2Pair    { -1 };
    std::atomic<int> monitorPair { -1 };   // pares de SAIDA
    std::atomic<int> phonesPair  { -1 };
    std::atomic<int> studioPair  { -1 };
    std::atomic<int> cuePair     { -1 };
};

/** Soma dos canais nos buses, master, CUE e monitoração.
    Memoria alocada em prepare(); process() e livre de alocacao e de lock. */
class MixerEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels)
    {
        sr = sampleRate; maxBlock = maxBlockSize;

        // Trocar de dispositivo (ou de sample rate) NAO pode destruir os canais:
        // a cena carregada se perderia e qualquer referencia guardada pela
        // superficie viraria ponteiro solto. So recria quando a QUANTIDADE muda.
        const bool rebuild = int (channels.size()) != numChannels;
        if (rebuild)
        {
            channels.clear();
            channels.reserve (size_t (numChannels));
            for (int i = 0; i < numChannels; ++i)
            {
                auto ch = std::make_unique<Channel>();
                ch->params.inputIndex.store (i);
                channels.push_back (std::move (ch));
            }
        }
        for (auto& ch : channels)
            ch->prepare (sampleRate, maxBlockSize);   // buffers e filtros ao novo bloco

        for (int b = 0; b < kNumBuses; ++b)
        {
            busL[b].assign (size_t (maxBlockSize), 0.0f);
            busR[b].assign (size_t (maxBlockSize), 0.0f);
            busMeter[b].prepare (sampleRate);
            busParams[b].outputPair.store (b);
        }
        for (auto* v : { &cueL, &cueR, &monL, &monR, &phL, &phR, &stL, &stR, &talkBus })
            v->assign (size_t (maxBlockSize), 0.0f);

        // um backfeed por canal: mix-minus e por fader, nao um aux compartilhado
        backfeed.clear();
        backfeed.resize (size_t (numChannels));
        for (auto& b : backfeed) b.assign (size_t (maxBlockSize), 0.0f);
        recL.assign (size_t (maxBlockSize), 0.0f);
        recR.assign (size_t (maxBlockSize), 0.0f);

        masterMeterL.prepare (sampleRate);
        masterMeterR.prepare (sampleRate);
        cueMeter.prepare (sampleRate);
        monitorMeter.prepare (sampleRate);

        masterGain.prepare (sampleRate, 25.0f); masterGain.setTargetDb (0.0f); masterGain.snap();
        monGain   .prepare (sampleRate, 30.0f); monGain.snap();
        phGain    .prepare (sampleRate, 30.0f); phGain.snap();
        stGain    .prepare (sampleRate, 30.0f); stGain.snap();
        cueGain   .prepare (sampleRate, 30.0f); cueGain.snap();
    }

    /** net[] traz as fontes que nao vem da placa (NDI, AES67, playout). Elas ja
        chegam aqui como blocos prontos de n amostras, puxados da fila antes do mix. */
    /** Leitura dos barramentos depois do process(), para envio a placas
        secundarias ou pela rede. Valido ate o proximo bloco. */
    const float* busLeft  (int b) const noexcept { return busL[size_t (b)].data(); }
    const float* busRight (int b) const noexcept { return busR[size_t (b)].data(); }
    const float* cueLeft()    const noexcept { return cueL.data(); }
    const float* cueRight()   const noexcept { return cueR.data(); }
    const float* monitorLeft()  const noexcept { return monL.data(); }
    const float* monitorRight() const noexcept { return monR.data(); }
    const float* phonesLeft()   const noexcept { return phL.data(); }
    const float* phonesRight()  const noexcept { return phR.data(); }
    const float* studioLeft()   const noexcept { return stL.data(); }
    const float* studioRight()  const noexcept { return stR.data(); }

    void process (const float* const* in, int numIn,
                  float* const* out, int numOut, int n,
                  const float* const* net = nullptr, int numNet = 0) noexcept
    {
        clear (n);

        bool crMicOpen = false, studioMicOpen = false, cueActive = false;

        for (auto& chPtr : channels)
        {
            auto& ch = *chPtr;
            const int idx  = ch.params.inputIndex.load (std::memory_order_relaxed);
            const int kind = ch.params.inputKind .load (std::memory_order_relaxed);
            const float* src = nullptr;
            if (kind == int (InputKind::Network))
                src = (net != nullptr && idx >= 0 && idx < numNet) ? net[idx] : nullptr;
            else
                src = (idx >= 0 && idx < numIn) ? in[idx] : nullptr;
            const float* sig = ch.process (src, n);
            const float* pre = ch.preFaderData();

            const int type = ch.params.sourceType.load (std::memory_order_relaxed);
            if (ch.isMicOpen())
            {
                if (mutesControlRoom (type)) crMicOpen = true;
                if (mutesStudio (type))      studioMicOpen = true;
            }

            const float p  = std::clamp (ch.params.panPos.load (std::memory_order_relaxed), -1.0f, 1.0f);
            const float a  = (p + 1.0f) * 0.25f * kPi;
            const float gl = std::cos (a), gr = std::sin (a);

            const unsigned mask = ch.params.busMask.load (std::memory_order_relaxed);
            for (int b = 0; b < kNumBuses; ++b)
            {
                if ((mask & (1u << b)) == 0) continue;
                float* dl = busL[b].data(); float* dr = busR[b].data();
                for (int i = 0; i < n; ++i) { dl[i] += sig[i] * gl; dr[i] += sig[i] * gr; }
            }

            // CUE e PFL: pre-fader e independente do ON/OFF
            if (ch.params.cue.load (std::memory_order_relaxed))
            {
                cueActive = true;
                for (int i = 0; i < n; ++i) { cueL[size_t (i)] += pre[i] * gl; cueR[size_t (i)] += pre[i] * gr; }
            }
        }

        // master aplicado no PGM 1
        masterGain.setTargetDb (masterGainDb.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i)
        {
            const float g = masterGain.next();
            busL[0][size_t (i)] *= g;
            busR[0][size_t (i)] *= g;
        }

        buildBackfeeds (n);
        buildMonitors (in, numIn, n, crMicOpen, studioMicOpen, cueActive);

        // ---- saidas
        for (int b = 0; b < kNumBuses; ++b)
        {
            const float bg = dbToGain (busParams[b].gainDb.load (std::memory_order_relaxed));
            writePair (out, numOut, busParams[b].outputPair.load (std::memory_order_relaxed),
                       busL[b].data(), busR[b].data(), n, bg);
        }
        writePair (out, numOut, monitor.monitorPair.load(), monL.data(), monR.data(), n, 1.0f);
        writePair (out, numOut, monitor.phonesPair .load(), phL.data(), phR.data(), n, 1.0f);
        writePair (out, numOut, monitor.studioPair .load(), stL.data(), stR.data(), n, 1.0f);
        writePair (out, numOut, monitor.cuePair    .load(), cueL.data(), cueR.data(), n, 1.0f);

        // backfeeds saem em mono, um por canal que precisa
        for (int i = 0; i < int (channels.size()); ++i)
        {
            const int pair = channels[size_t (i)]->params.feedOutputPair.load (std::memory_order_relaxed);
            if (pair < 0) continue;
            const int li = pair * 2, ri = li + 1;
            const float* b = backfeed[size_t (i)].data();
            if (li < numOut && out[li] != nullptr) for (int k = 0; k < n; ++k) out[li][k] = b[k];
            if (ri < numOut && out[ri] != nullptr) for (int k = 0; k < n; ++k) out[ri][k] = b[k];
        }

        for (int b = 0; b < kNumBuses; ++b) busMeter[b].process (busL[b].data(), n);
        masterMeterL.process (busL[0].data(), n);
        masterMeterR.process (busR[0].data(), n);
        cueMeter    .process (cueL.data(), n);
        monitorMeter.process (monL.data(), n);

        crMuted    .store (crMicOpen,     std::memory_order_relaxed);
        studioMuted.store (studioMicOpen, std::memory_order_relaxed);
        cueOn      .store (cueActive,     std::memory_order_relaxed);
    }

    int      numChannels()      const noexcept { return int (channels.size()); }
    Channel& channel (int i)          noexcept { return *channels[size_t (i)]; }
    const Channel& channel (int i) const noexcept { return *channels[size_t (i)]; }

    std::atomic<float> masterGainDb { 0.0f };
    BusParams          busParams[kNumBuses];
    AutomationParams   automation;
    MonitorParams      monitor;

    /** Sinal de backfeed de um canal, para medicao e diagnostico. */
    const float* backfeedData (int ch) const noexcept { return backfeed[size_t (ch)].data(); }

    Meter busMeter[kNumBuses];
    Meter masterMeterL, masterMeterR, cueMeter, monitorMeter;

    /** Estado que a superficie mostra em lampada: monitor mudo, CUE ativo. */
    std::atomic<bool> crMuted { false }, studioMuted { false }, cueOn { false };

private:
    void clear (int n) noexcept
    {
        for (int b = 0; b < kNumBuses; ++b)
        {
            std::fill (busL[b].begin(), busL[b].begin() + n, 0.0f);
            std::fill (busR[b].begin(), busR[b].begin() + n, 0.0f);
        }
        for (auto* v : { &cueL, &cueR, &monL, &monR, &phL, &phR, &stL, &stR })
            std::fill (v->begin(), v->begin() + n, 0.0f);
    }

    /** Mix-minus: cada fonte de telefone/codec ouve o programa MENOS ela mesma.
        Como a soma e linear, basta subtrair a propria contribuicao do bus. */
    void buildBackfeeds (int n) noexcept
    {
        for (int i = 0; i < int (channels.size()); ++i)
        {
            auto& ch = *channels[size_t (i)];
            float* dst = backfeed[size_t (i)].data();

            if (! needsMixMinus (ch.params.sourceType.load (std::memory_order_relaxed))
                || ch.params.feedOutputPair.load (std::memory_order_relaxed) < 0)
            {
                std::fill (dst, dst + n, 0.0f);
                continue;
            }

            const int fs = ch.params.feedSource.load (std::memory_order_relaxed);
            const int bus = (fs >= 1 && fs <= kNumBuses) ? fs - 1 : 0;

            const unsigned mask = ch.params.busMask.load (std::memory_order_relaxed);
            const bool contributes = (mask & (1u << bus)) != 0
                                   && ch.params.on.load (std::memory_order_relaxed);
            const float* own = ch.processedData();

            const float p  = std::clamp (ch.params.panPos.load (std::memory_order_relaxed), -1.0f, 1.0f);
            const float a  = (p + 1.0f) * 0.25f * kPi;
            const float gl = std::cos (a), gr = std::sin (a);
            const float masterG = (bus == 0) ? masterGain.currentGain() : 1.0f;

            const bool talking = ch.params.talkTo.load (std::memory_order_relaxed);
            const float dim = talking ? dbToGain (ch.params.feedDimDb.load (std::memory_order_relaxed)) : 1.0f;

            for (int k = 0; k < n; ++k)
            {
                float mix = 0.5f * (busL[bus][size_t (k)] + busR[bus][size_t (k)]);
                if (contributes)
                    mix -= 0.5f * (own[k] * gl + own[k] * gr) * masterG;   // tira o proprio sinal
                dst[k] = mix * dim + (talking ? talkBus[size_t (k)] : 0.0f);
            }
        }
    }

    void sourceBuffers (const float* const* in, int numIn, int srcIdx,
                        const float*& l, const float*& r) noexcept
    {
        if (srcIdx <= int (MonitorSource::Pgm4))
        {
            l = busL[srcIdx].data(); r = busR[srcIdx].data();
            return;
        }
        const int pair = (srcIdx == int (MonitorSource::Ext1))
                       ? monitor.ext1Pair.load (std::memory_order_relaxed)
                       : monitor.ext2Pair.load (std::memory_order_relaxed);
        const int li = pair * 2, ri = li + 1;
        l = (pair >= 0 && li < numIn) ? in[li] : nullptr;
        r = (pair >= 0 && ri < numIn) ? in[ri] : nullptr;
    }

    void buildMonitors (const float* const* in, int numIn, int n,
                        bool crMicOpen, bool studioMicOpen, bool cueActive) noexcept
    {
        const float* srcL = nullptr; const float* srcR = nullptr;
        sourceBuffers (in, numIn, monitor.source.load (std::memory_order_relaxed), srcL, srcR);

        const float dim = monitor.talkToStudio.load (std::memory_order_relaxed)
                        ? monitor.dimDb.load (std::memory_order_relaxed) : 0.0f;

        // monitor do controle: mudo enquanto houver microfone do CR aberto
        monGain.setTargetDb (crMicOpen ? kMinusInfDb
                                       : monitor.monitorDb.load (std::memory_order_relaxed) + dim);
        // fone do operador nao muta; o CUE e que o sobrepoe
        const bool cueOverride = cueActive && monitor.cueToPhones.load (std::memory_order_relaxed);
        phGain .setTargetDb (cueOverride ? kMinusInfDb
                                         : monitor.phonesDb.load (std::memory_order_relaxed));
        cueGain.setTargetDb (monitor.cueDb.load (std::memory_order_relaxed));
        stGain .setTargetDb (studioMicOpen ? kMinusInfDb
                                           : monitor.studioDb.load (std::memory_order_relaxed) + dim);

        for (int i = 0; i < n; ++i)
        {
            const float l = srcL ? srcL[i] : 0.0f, r = srcR ? srcR[i] : 0.0f;
            const float mg = monGain.next(), pg = phGain.next(),
                        cg = cueGain.next(), sg = stGain.next();

            monL[size_t (i)] = l * mg;  monR[size_t (i)] = r * mg;
            phL [size_t (i)] = l * pg;  phR [size_t (i)] = r * pg;
            stL [size_t (i)] = busL[0][size_t (i)] * sg;
            stR [size_t (i)] = busR[0][size_t (i)] * sg;

            cueL[size_t (i)] *= cg;     cueR[size_t (i)] *= cg;
            if (cueOverride) { phL[size_t (i)] += cueL[size_t (i)]; phR[size_t (i)] += cueR[size_t (i)]; }
        }
    }

    void writePair (float* const* out, int numOut, int pair,
                    const float* l, const float* r, int n, float gain) noexcept
    {
        if (pair < 0) return;
        const int li = pair * 2, ri = li + 1;
        if (li < numOut && out[li] != nullptr) for (int i = 0; i < n; ++i) out[li][i] = l[i] * gain;
        if (ri < numOut && out[ri] != nullptr) for (int i = 0; i < n; ++i) out[ri][i] = r[i] * gain;
    }

    std::vector<std::unique_ptr<Channel>> channels;
    std::vector<float> busL[kNumBuses], busR[kNumBuses];
    std::vector<float> cueL, cueR, monL, monR, phL, phR, stL, stR, talkBus, recL, recR;
    std::vector<std::vector<float>> backfeed;
    SmoothedGain masterGain, monGain, phGain, stGain, cueGain;
    double sr = 48000.0; int maxBlock = 512;
};

} // namespace mesa
