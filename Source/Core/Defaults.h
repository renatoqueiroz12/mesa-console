#pragma once
#include "MixerEngine.h"
#include "Channel.h"

namespace mesa {

/** Ajustes de fabrica.

    A razao de existir: os parametros do trigger e do nivelador interagem entre
    si, e sozinhos nenhum deles diz muito. Quem comeca do zero acaba mexendo em
    seis controles ao mesmo tempo sem saber qual causou o que — e o resultado
    costuma ser uma mesa que dispara com tosse ou que nao dispara nunca.

    Os valores abaixo sao um ponto de partida que funciona para fala de estudio
    com microfone em nivel razoavel. Nao sao o ideal para a sua sala: sao um
    NORTE, de onde vale ajustar uma coisa de cada vez.

    Deliberadamente conservadores em dois pontos: permanencia alta o bastante
    para nao disparar com estalo, e hold longo o bastante para nao voltar ao
    plano geral na respirada entre frases. Errar para o lado lento incomoda
    menos que errar para o lado nervoso. */
struct Defaults
{
    // ---- trigger de audio
    static constexpr float kThresholdDb  = -35.0f;  // ajuste pelo calibrador
    static constexpr float kTriggerMs    = 300.0f;  // separa fala de estalo
    static constexpr float kHysteresisDb =   6.0f;  // evita liga-desliga na borda
    static constexpr float kHoldMs       = 2500.0f; // atravessa a pausa entre frases
    static constexpr float kReleaseMs    = 400.0f;
    static constexpr float kCooldownMs   = 1000.0f;

    // ---- automacao de cameras
    static constexpr float kWideDelayMs  = 3000.0f; // silencio antes de voltar ao BG
    static constexpr float kMinShotMs    = 1500.0f; // evita pingue-pongue
    static constexpr float kManualHoldMs = 5000.0f;
    static constexpr float kDominanceDb  =   6.0f;

    // ---- nivelador de fader
    static constexpr float kAutoTargetDb =  -18.0f; // equivale ao 0 VU analogico
    static constexpr float kAutoMaxDb    =   10.0f;
    static constexpr float kAutoMinDb    =  -30.0f;
    static constexpr float kAutoFloorDb  =  -45.0f; // suba se subir sozinho no silencio
    static constexpr float kAutoSpeed    =    6.0f; // 4 a 8 e a faixa util

    // ---- ganho
    static constexpr float kTrimDb       = 0.0f;
    static constexpr float kAutoTrimTargetDb = -20.0f;
};

/** Aplica os ajustes de fabrica ao trigger de um canal.
    NAO toca na camera nem no comando: aquilo e escolha de instalacao, e perder
    isso num reset seria pior que o problema que o reset resolve. */
inline void resetTrigger (Channel& ch)
{
    auto& t = ch.params.trigger;
    t.source      .store (int (TapPoint::Input));
    t.thresholdDb .store (Defaults::kThresholdDb);
    t.triggerMs   .store (Defaults::kTriggerMs);
    t.hysteresisDb.store (Defaults::kHysteresisDb);
    t.holdMs      .store (Defaults::kHoldMs);
    t.releaseMs   .store (Defaults::kReleaseMs);
    t.cooldownMs  .store (Defaults::kCooldownMs);
}

/** Ajustes de fabrica do nivelador. Nao liga nem desliga a funcao. */
inline void resetAutoMix (Channel& ch)
{
    auto& a = ch.params.autoMix;
    a.targetDb     .store (Defaults::kAutoTargetDb);
    a.maxFaderDb   .store (Defaults::kAutoMaxDb);
    a.minFaderDb   .store (Defaults::kAutoMinDb);
    a.floorDb      .store (Defaults::kAutoFloorDb);
    a.speedDbPerSec.store (Defaults::kAutoSpeed);
}

/** Ajustes de fabrica de um canal inteiro, menos a fonte e o roteamento. */
inline void resetChannel (Channel& ch)
{
    resetTrigger (ch);
    resetAutoMix (ch);
    ch.params.trimDb.store (Defaults::kTrimDb);
    ch.params.autoTrim.targetDb.store (Defaults::kAutoTrimTargetDb);
}

/** Ajustes de fabrica da automacao de cameras. Preserva a camera padrao. */
inline void resetAutomation (MixerEngine& mix)
{
    auto& A = mix.automation;
    A.dominance   .store (true);
    A.dominanceDb .store (Defaults::kDominanceDb);
    A.minShotMs   .store (Defaults::kMinShotMs);
    A.wideDelayMs .store (Defaults::kWideDelayMs);
    A.manualHoldMs.store (Defaults::kManualHoldMs);
}

/** Tudo: todos os canais e a automacao. */
inline void resetAll (MixerEngine& mix)
{
    for (int i = 0; i < mix.numChannels(); ++i)
        resetChannel (mix.channel (i));
    resetAutomation (mix);
}

} // namespace mesa
