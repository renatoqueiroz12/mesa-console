#pragma once
#include "Meter.h"
#include "AutoTrim.h"
#include "DspRack.h"
#include "Diagnostics.h"
#include "AutoMixer.h"
#include <vector>
#include <atomic>
#include <string>
#include <array>
#include <algorithm>

namespace mesa {

/** Pontos onde o Audio Trigger Engine podera medir o sinal (fase 7). */
enum class TapPoint { Input = 0, PostAutoTrim, PostDsp, PostFader, NumTaps };

/** Tipo da fonte carregada no strip. Determina a logica automatica:
    mute de monitores, cough, talkback e mix-minus — nao e so um rotulo. */
enum class SourceType
{
    Operator = 0, Producer, CrGuest, StudioGuest, ExternalMic,
    Line, Phone, Codec, ComputerPlayer, StudioFeed, NumTypes
};

/** Microfone dentro do controle: abrir o canal muta monitor CR e preview. */
inline bool mutesControlRoom (int t) noexcept
{
    return t == int (SourceType::Operator) || t == int (SourceType::Producer)
        || t == int (SourceType::CrGuest);
}
/** Microfone no estudio: abrir o canal muta o monitor do estudio. */
inline bool mutesStudio (int t) noexcept { return t == int (SourceType::StudioGuest); }

/** Fontes que recebem backfeed (mix-minus) proprio. */
inline bool needsMixMinus (int t) noexcept
{
    return t == int (SourceType::Phone) || t == int (SourceType::Codec)
        || t == int (SourceType::StudioFeed);
}

/** Texto curto que atravessa a fronteira UI -> audio sem alocar e sem lock.

    Usa seqlock: o escritor marca a sequencia como impar, escreve e volta para par.
    O leitor le a sequencia antes e depois; se mudou, tenta de novo. Assim o
    callback nunca entrega meio comando — e nunca espera por ninguem.
    (Duplo buffer nao bastava: um escritor rapido volta ao mesmo buffer enquanto
    o leitor ainda esta copiando dele.) */
class CommandText
{
public:
    static constexpr int kMaxLen = 120;
    static constexpr int kMaxRetries = 8;

    /** thread de UI */
    void set (const std::string& text)
    {
        const unsigned s = seq.load (std::memory_order_relaxed);
        seq.store (s + 1, std::memory_order_relaxed);          // impar: escrevendo
        std::atomic_thread_fence (std::memory_order_release);

        const int n = int (std::min (text.size(), size_t (kMaxLen - 1)));
        for (int i = 0; i < n; ++i)
            buf[size_t (i)].store (text[size_t (i)], std::memory_order_relaxed);
        buf[size_t (n)].store ('\0', std::memory_order_relaxed);
        len.store (n, std::memory_order_relaxed);

        seq.store (s + 2, std::memory_order_release);          // par: pronto
    }

    /** thread de audio: copia para destino fixo. Nunca aloca, nunca bloqueia. */
    int copyTo (char* dst, int dstSize) const noexcept
    {
        for (int attempt = 0; attempt < kMaxRetries; ++attempt)
        {
            const unsigned s1 = seq.load (std::memory_order_acquire);
            if (s1 & 1u) continue;                             // escrita em curso

            const int n = std::min (len.load (std::memory_order_relaxed), dstSize - 1);
            for (int i = 0; i < n; ++i) dst[i] = buf[size_t (i)].load (std::memory_order_relaxed);
            dst[n] = '\0';

            std::atomic_thread_fence (std::memory_order_acquire);
            if (seq.load (std::memory_order_relaxed) == s1) return n;   // leitura limpa
        }
        dst[0] = '\0';        // desistiu: melhor vazio do que texto corrompido
        return 0;
    }

    bool empty() const noexcept { return len.load (std::memory_order_relaxed) == 0; }

    std::string str() const
    {
        char tmp[kMaxLen];
        const int n = copyTo (tmp, kMaxLen);
        return std::string (tmp, size_t (n));
    }

private:
    std::array<std::atomic<char>, kMaxLen> buf {};
    std::atomic<int> len { 0 };
    std::atomic<unsigned> seq { 0 };
};

/** Parametros do Audio Trigger. Ja fazem parte da cena; o engine que os consome
    entra na fase 7. Mantidos aqui porque a logica segue a fonte, nao o fader. */
struct TriggerParams
{
    std::atomic<bool>  enabled      { false };
    std::atomic<int>   source       { int (TapPoint::Input) };
    std::atomic<float> thresholdDb  { -35.0f };
    std::atomic<float> triggerMs    { 150.0f };
    std::atomic<float> holdMs       { 800.0f };
    std::atomic<float> releaseMs    { 400.0f };
    std::atomic<float> cooldownMs   { 500.0f };
    std::atomic<float> hysteresisDb {   5.0f };
    std::atomic<int>   camera       { 0 };     // 0 = sem camera

    /** Comando literal enviado quando este canal dispara. Vazio = a mesa monta
        um CUT a partir do numero da camera. Preenchido, manda exatamente isto —
        serve para overlay, transicao, atalho do vMix ou outro sistema. */
    CommandText command;
    CommandText releaseCommand;   // opcional: enviado quando o canal solta
    std::atomic<int> target { 0 };   // indice em Settings::targets
};

/** De onde vem o sinal do canal. */
enum class InputKind { Device = 0, Network };    // placa ASIO  |  NDI / AES67 / playout

struct ChannelParams
{
    std::atomic<int>      inputKind  { int (InputKind::Device) };
    std::atomic<int>      inputIndex { -1 };     // indice na lista da origem; -1 = sem fonte
    std::atomic<int>      sourceType { int (SourceType::Line) };
    std::atomic<float>    trimDb     { 0.0f };   // trim manual (-25..+25)
    std::atomic<float>    faderDb    { -100.0f };
    std::atomic<float>    panPos     { 0.0f };   // -1 esq .. +1 dir
    std::atomic<bool>     on         { false };
    std::atomic<bool>     mute       { false };
    std::atomic<bool>     cue        { false };
    std::atomic<unsigned> busMask    { 1u };     // bit 0 = PGM1 ... bit 3 = PGM4

    // --- mix-minus (feed to source): so vale para telefone, codec e feed de estudio
    std::atomic<int>   feedSource { 0 };   // 0 = auto (PGM1 no ar / Phone fora), 1..4 = PGM fixo
    std::atomic<int>   feedOutputPair { -1 };
    std::atomic<float> feedDimDb { -10.0f };   // quanto abaixa o programa durante o talkback
    std::atomic<bool>  talkTo { false };       // operador falando com esta fonte
    AutoTrimParams        autoTrim;
    AutoMixParams         autoMix;
    TriggerParams         trigger;

    /** LOGICA DO CANAL (fader-start): comandos disparados na BORDA de ON/OFF.
        E assim que o fader liga e para a cartucheira, o playout ou a luz de ar. */
    CommandText      onCommand, offCommand;
    std::atomic<int> logicTarget { 0 };
    std::atomic<bool> logicEnabled { false };
};

/** Um strip: entrada -> meter -> auto trim -> trim -> [DSP rack] -> fader -> mute/on.
    Tudo pre-alocado; process() nao aloca, nao trava e nao chama nada lento. */
class Channel
{
public:
    void prepare (double sampleRate_, int maxBlockSize)
    {
        sampleRate = sampleRate_;
        const double sr = sampleRate_;
        scratch .assign (size_t (maxBlockSize), 0.0f);
        preFader.assign (size_t (maxBlockSize), 0.0f);
        meterIn .prepare (sr);
        meterDsp.prepare (sr);
        meterOut.prepare (sr);
        rack.prepare (sr, maxBlockSize);
        autoTrim.prepare (sr);
        autoMixer.prepare (sr, maxBlockSize);
        trimGain .prepare (sr, 25.0f);
        faderGain.prepare (sr, 20.0f);
        presence.reset();
        trimGain .setTargetDb (0.0f);  trimGain .snap();
        faderGain.setTargetDb (kMinusInfDb); faderGain.snap();
    }

    /** in pode ser nullptr (canal sem fonte). Retorna o buffer mono processado. */
    const float* process (const float* in, int n) noexcept
    {
        float* dst = scratch.data();

        if (in == nullptr)
        {
            for (int i = 0; i < n; ++i) { dst[i] = 0.0f; preFader[size_t (i)] = 0.0f; }
            meterIn.silence(); meterDsp.silence(); meterOut.silence();
            for (int t = 0; t < int (TapPoint::NumTaps); ++t)
                taps[t].store (kMinusInfDb, std::memory_order_relaxed);
            return dst;
        }

        for (int i = 0; i < n; ++i) dst[i] = in[i];

        // 1. medicao de entrada
        meterIn.process (dst, n);
        taps[int (TapPoint::Input)].store (meterIn.fastDb(), std::memory_order_relaxed);

        // 2. auto trim + trim manual, aplicados como um unico ganho suavizado
        const float atDb = autoTrim.update (meterIn.rmsDb(), n, params.autoTrim);
        trimGain.setTargetDb (params.trimDb.load (std::memory_order_relaxed) + atDb);
        for (int i = 0; i < n; ++i) dst[i] *= trimGain.next();
        taps[int (TapPoint::PostAutoTrim)].store (meterIn.fastDb()
                                                  + gainToDb (trimGain.currentGain()),
                                                  std::memory_order_relaxed);

        // 3. rack de DSP: gate, EQ, compressor, de-esser, limiter e, futuramente, VST3
        rack.process (dst, n);
        meterDsp.process (dst, n);
        taps[int (TapPoint::PostDsp)].store (meterDsp.fastDb(), std::memory_order_relaxed);

        // guarda o sinal pre-fader: e ele que alimenta o CUE (PFL) e o Audio Trigger
        for (int i = 0; i < n; ++i) preFader[size_t (i)] = dst[i];

        // 4. nivelador: mede o sinal PRE-fader e decide onde o fader deve estar.
        // Medir depois do fader fecharia uma malha que oscila.
        const bool open = params.on.load (std::memory_order_relaxed)
                       && ! params.mute.load (std::memory_order_relaxed);

        const float operatorDb = params.faderDb.load (std::memory_order_relaxed);
        const float rideDb = autoMixer.process (params.autoMix, preFader.data(), n,
                                                operatorDb, open);
        if (params.autoMix.enabled.load (std::memory_order_relaxed) && open)
        {
            // escreve de volta: o fader ANDA na tela, e o operador ve o que a
            // mesa esta fazendo em vez de adivinhar
            params.faderDb.store (rideDb, std::memory_order_relaxed);
        }

        // 5. fader + on/off + mute
        faderGain.setTargetDb (open ? rideDb : kMinusInfDb);
        for (int i = 0; i < n; ++i) dst[i] *= faderGain.next();

        meterOut.process (dst, n);
        taps[int (TapPoint::PostFader)].store (meterOut.fastDb(), std::memory_order_relaxed);

        // diagnostico: ha sinal chegando? esta clipando? o calibrador precisa medir?
        const float blockMs = float (n) / float (sampleRate) * 1000.0f;
        presence.update (meterIn.fastDb(), blockMs, meterIn.clipped (true));
        calibrator.update (meterIn.fastDb(), blockMs);

        return dst;
    }

    float tapDb (TapPoint t) const noexcept
    {
        return taps[int (t)].load (std::memory_order_relaxed);
    }

    float autoTrimDb() const noexcept { return autoTrim.currentDb(); }

    /** Onde o nivelador colocou o fader. Igual ao fader quando esta desligado. */
    float autoMixFaderDb() const noexcept { return autoMixer.currentFaderDb(); }

    /** Sinal pos-trim e pos-DSP, antes do fader e do ON/OFF. */
    const float* preFaderData() const noexcept { return preFader.data(); }

    /** Sinal pos-fader do ultimo bloco — usado pelo mix-minus. */
    const float* processedData() const noexcept { return scratch.data(); }

    bool isMicOpen() const noexcept
    {
        return params.on.load (std::memory_order_relaxed)
            && ! params.mute.load (std::memory_order_relaxed);
    }

    ChannelParams params;
    Meter meterIn, meterDsp, meterOut;
    DspRack rack;
    SignalPresence      presence;      // "esta chegando audio neste canal?"
    ThresholdCalibrator calibrator;    // mede piso de ruido e fala

    /** Nome da fonte carregada no strip. Tocado apenas pela thread de UI/config,
        nunca lido dentro de process(). */
    std::string name;

    /** Rotulo que a propria fonte manda (nome do cartucho, tempo restante).
        Chega pela metadata do NDI e aparece na segunda linha do OLED. */
    std::string sourceLabel;

private:
    std::vector<float> scratch, preFader;
    double sampleRate = 48000.0;
    AutoTrim      autoTrim;
    AutoMixer     autoMixer;
    SmoothedGain  trimGain, faderGain;
    std::atomic<float> taps[int (TapPoint::NumTaps)] {
        { kMinusInfDb }, { kMinusInfDb }, { kMinusInfDb }, { kMinusInfDb } };
};

} // namespace mesa
