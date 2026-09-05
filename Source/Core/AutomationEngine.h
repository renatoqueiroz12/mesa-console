#pragma once
#include "TriggerEngine.h"
#include <algorithm>
#include "MixerEngine.h"
#include <array>
#include <memory>
#include <string>

namespace mesa {

/** Comando para o mundo externo (vMix hoje, outro sistema amanha).
    O engine de audio NUNCA fala com a rede: ele enfileira e segue tocando. */
struct Command
{
    enum class Type { None = 0, Cut, Preview, OverlayOn, OverlayOff };
    Type  type = Type::None;
    int   camera = 0;
    int   channel = -1;
    bool  simulated = false;      // modo de teste: nao deve sair pela rede
    bool  manual    = false;      // disparado pelo operador, nao pelo audio
    int   target    = 0;          // indice do destino (vMix, cartucheira, ...)
    char  text[CommandText::kMaxLen] = { 0 };   // comando literal, se houver
    float levelDb = kMinusInfDb;
    double timeMs = 0.0;
};

/** Fila de um produtor (audio) e um consumidor (thread de rede), sem lock. */
class CommandQueue
{
public:
    static constexpr int kCapacity = 128;

    bool push (const Command& c) noexcept
    {
        const int w = write.load (std::memory_order_relaxed);
        const int r = read .load (std::memory_order_acquire);
        if (w - r >= kCapacity) { dropped.fetch_add (1, std::memory_order_relaxed); return false; }
        buf[size_t (w % kCapacity)] = c;
        write.store (w + 1, std::memory_order_release);
        return true;
    }

    bool pop (Command& out) noexcept
    {
        const int r = read .load (std::memory_order_relaxed);
        const int w = write.load (std::memory_order_acquire);
        if (r >= w) return false;
        out = buf[size_t (r % kCapacity)];
        read.store (r + 1, std::memory_order_release);
        return true;
    }

    int pending() const noexcept
    { return write.load (std::memory_order_acquire) - read.load (std::memory_order_relaxed); }

    std::atomic<int> dropped { 0 };

private:
    std::array<Command, kCapacity> buf {};
    std::atomic<int> write { 0 }, read { 0 };
};

/** Texto efetivo do comando: o que o usuario escreveu ou, na falta, um CUT
    montado a partir da camera. Roda na thread de rede, nao no audio. */
inline std::string commandText (const Command& c)
{
    if (c.text[0] != '\0') return std::string (c.text);
    return "FUNCTION Cut Input=" + std::to_string (c.camera);
}

/** Regras de producao: dominancia entre canais, plano minimo, hold e camera geral.
    Roda na thread de audio, uma vez por bloco. Nada aqui aloca. */
class AutomationEngine
{
public:
    /** Suspensao temporaria: enquanto vale, a automacao nao corta nada.

        Existe para o VT. Entrou materia da cartucheira, o estudio continua com
        microfone aberto e gente conversando — sem suspender, a mesa cortaria
        para quem tossisse no meio da materia. Quem sabe que ha VT no ar e a
        cartucheira, entao e ela que manda calar. */
    std::atomic<double> suspendUntilMs { 0.0 };
    std::atomic<bool>   suspended { false };

    /** Relogio interno, para quem precisa marcar prazo de suspensao. */
    double nowMs() const noexcept { return timeMs; }

    /** Quantas vezes a conversa cruzada levou ao plano aberto. */
    std::atomic<int> crossTalks { 0 };

    /** Ha dois ou mais falando agora. */
    bool emConversaCruzada() const noexcept { return multiTalkSince >= 0.0; }

    /** Quanto falta para o proximo corte poder sair, por causa do plano minimo.

        Existe porque a espera era invisivel: o trigger armava, a tally dizia
        PRONTO e nada acontecia por um tempo. Sem ver o relogio, parece defeito.
        Quando a camera padrao acabou de entrar, este contador esta cheio — e e
        justamente ai que a espera mais incomoda. */
    double msAtePoderCortar (const MixerEngine& mix) const noexcept
    {
        const double falta = double (mix.automation.minShotMs.load()) - (timeMs - lastCutMs);
        return falta > 0.0 ? falta : 0.0;
    }

    /** Quanto falta no cooldown daquele canal. */
    double msCooldown (const MixerEngine& mix, int canal) const noexcept
    {
        if (canal < 0 || canal >= int (triggers.size())) return 0.0;
        if (triggers[size_t (canal)]->current() != TriggerState::Cooldown) return 0.0;
        return double (mix.channel (canal).params.trigger.cooldownMs.load());
    }

    /** Quanto falta para voltar ao plano padrao, em ms. Zero quando ja voltou
        ou quando alguem ainda esta falando. Existe para a interface poder
        MOSTRAR a contagem: sem isso o operador acha que travou. */
    double msUntilWide (const MixerEngine& mix) const noexcept
    {
        const auto& A = mix.automation;
        if (A.wideCamera.load() <= 0) return 0.0;
        if (intendedCamera.load() == A.wideCamera.load()) return 0.0;

        const double falta = holdUntilMs - timeMs;
        return falta > 0.0 ? falta : 0.0;
    }

    bool isSuspended (double atMs) const noexcept
    {
        if (suspended.load (std::memory_order_relaxed)) return true;
        const double until = suspendUntilMs.load (std::memory_order_relaxed);
        return until > 0.0 && atMs < until;
    }

    void prepare (int numChannels)
    {
        prevOn.assign (size_t (numChannels), false);
        triggers.clear();
        triggers.reserve (size_t (numChannels));
        for (int i = 0; i < numChannels; ++i)
            triggers.push_back (std::make_unique<ChannelTrigger>());   // atomics nao sao copiaveis
        levels  .assign (size_t (numChannels), kMinusInfDb);
        timeMs = 0.0; lastCutMs = -1e9; holdUntilMs = 0.0; lastActiveMs = 0.0;
        wasQuiet = false;
        multiTalkSince = -1.0;
        liveCamera.store (0);
        intendedCamera.store (0);
    }

    void processBlock (MixerEngine& mix, float blockMs) noexcept
    {
        timeMs += blockMs;
        const auto& A = mix.automation;
        const int n = int (triggers.size());

        // Suspenso: nenhum corte sai. Os triggers seguem medindo, para o estado
        // estar correto quando a automacao voltar — congelar a medicao faria a
        // mesa "acordar" achando que ninguem fala.
        const bool quiet = isSuspended (timeMs);

        processLogic (mix, n);

        for (int i = 0; i < n; ++i)
        {
            const auto& ch = mix.channel (i);
            const int tap = ch.params.trigger.source.load (std::memory_order_relaxed);
            levels[size_t (i)] = ch.tapDb (TapPoint (tap));
        }

        bool anyActive = false;
        int  activeCamera = 0;        // inclui candidato: serve para o hold
        int  camConfirmada = 0;       // SO quem passou da permanencia
        float activeHoldMs = 0.0f;
        int  numFalando = 0;

        for (int i = 0; i < n; ++i)
        {
            auto& ch = mix.channel (i);
            const auto& tp = ch.params.trigger;

            const bool dominant = ! A.dominance.load (std::memory_order_relaxed)
                                || isDominant (i, mix, A.dominanceDb.load (std::memory_order_relaxed));

            const auto ev = triggers[size_t (i)]->update (
                                i, levels[size_t (i)], tp, dominant,
                                ch.params.on.load (std::memory_order_relaxed)
                                && ! ch.params.mute.load (std::memory_order_relaxed),
                                blockMs);

            if (triggers[size_t (i)]->current() == TriggerState::Active
             || triggers[size_t (i)]->current() == TriggerState::Candidate)
            {
                anyActive = true;
                if (triggers[size_t (i)]->current() == TriggerState::Active)
                {
                    ++numFalando;
                    if (camConfirmada == 0)
                        camConfirmada = ch.params.trigger.camera.load (std::memory_order_relaxed);
                }
                if (activeCamera == 0)
                {
                    activeCamera = ch.params.trigger.camera.load (std::memory_order_relaxed);
                    activeHoldMs = ch.params.trigger.holdMs.load (std::memory_order_relaxed);
                }
            }

            if (ev.type == TriggerEvent::Type::Fired)
            {
                if (quiet) { ++ignored; }
                else requestCut (mix, ev);
            }
            else if (ev.type == TriggerEvent::Type::Rejected)
                pushEvent (ev);
        }

        if (anyActive)
        {
            lastActiveMs = timeMs;

            // HOLD conta do FIM da fala, nao do corte.
            //
            // Antes ele era armado no instante do corte: quem falasse trinta
            // segundos ja teria o hold vencido no segundo 2,5, e ao calar a
            // camera trocava sem nenhuma retencao. A promessa da tela — "quanto
            // a camera fica DEPOIS que a pessoa para" — nao se cumpria.
            //
            // Empurrando o prazo enquanto ha fala, ele congela no ultimo
            // instante em que alguem falou e passa a valer dali.
            if (activeHoldMs > 0.0f)
                holdUntilMs = timeMs + double (activeHoldMs);
        }

        const int wide = A.wideCamera.load (std::memory_order_relaxed);

        // REAVALIACAO CONTINUA.
        //
        // O corte so nascia na BORDA do trigger. Se naquele instante alguma
        // regra bloqueava — plano minimo, quase sempre —, o disparo era
        // descartado e nunca mais tentado: a pessoa seguia falando e a mesa
        // ficava na geral. Tambem faltava borda ao voltar de VT e ao sair de
        // conversa cruzada, e cada caso desses virou um remendo separado.
        //
        // Uma regra so resolve os tres: se ha alguem falando, a camera dele nao
        // esta no ar e nada bloqueia agora, corta. O sistema passa a se
        // corrigir sozinho em vez de depender de acertar o instante exato.
        const bool bloqueado = timeMs - lastCutMs
                             < double (A.minShotMs.load (std::memory_order_relaxed));
        const bool naGeral = wide > 0 && intendedCamera.load() == wide
                          && A.geralInterrompivel.load (std::memory_order_relaxed);

        // SO camera confirmada: candidato ainda nao passou da permanencia, e
        // cortar por candidato desfaria o filtro que separa fala de estalo.
        if (! quiet && camConfirmada > 0
            && intendedCamera.load() != camConfirmada
            && (naGeral || ! bloqueado)
            && multiTalkSince < 0.0)
        {
            Command c;
            c.type = Command::Type::Cut; c.camera = camConfirmada; c.channel = -1;
            c.simulated = A.testMode.load (std::memory_order_relaxed);
            c.timeMs = timeMs;
            if (A.enabled.load (std::memory_order_relaxed))
            {
                commands.push (c);
                intendedCamera.store (camConfirmada);
                if (! c.simulated) liveCamera.store (camConfirmada);
                lastCutMs = timeMs;
            }
        }
        wasQuiet = quiet;

        // ninguem falando: volta para a camera geral depois do silencio pedido
        // ---- conversa cruzada: dois ou mais falando ao mesmo tempo
        //
        // Sem isto a mesa fica alternando entre quem esta mais alto a cada
        // instante, e o resultado no ar e um corta-corta que cansa. O plano
        // aberto e a resposta certa: mostra as duas pessoas discutindo.
        //
        // Exige permanencia propria porque sobreposicao curta e normal na fala
        // — alguem concorda, ri, completa a frase do outro. So vira conversa
        // cruzada quando se sustenta.
        const float multiMs = A.multiTalkEnabled.load (std::memory_order_relaxed)
                                  ? A.multiTalkMs.load (std::memory_order_relaxed) : 0.0f;
        if (multiMs > 0.0f && numFalando >= 2)
        {
            if (multiTalkSince < 0.0) multiTalkSince = timeMs;
        }
        else
        {
            // Saiu da conversa cruzada e sobrou UM falando: reassume a camera
            // dele. Sem isto a mesa fica presa no plano aberto, porque quem
            // continuou falando nunca gerou borda nova de trigger.
            if (multiTalkSince >= 0.0 && anyActive && activeCamera > 0
                && intendedCamera.load() != activeCamera && ! quiet)
            {
                Command c;
                c.type = Command::Type::Cut; c.camera = activeCamera; c.channel = -1;
                c.simulated = A.testMode.load (std::memory_order_relaxed);
                c.timeMs = timeMs;
                if (A.enabled.load (std::memory_order_relaxed) && commands.push (c))
                {
                    intendedCamera.store (activeCamera);
                    if (! c.simulated) liveCamera.store (activeCamera);
                    lastCutMs = timeMs;
                }
            }
            multiTalkSince = -1.0;
        }

        if (! quiet && multiMs > 0.0f && multiTalkSince >= 0.0
            && timeMs - multiTalkSince >= double (multiMs))
        {
            const int alvo = A.multiTalkCamera.load (std::memory_order_relaxed) > 0
                                 ? A.multiTalkCamera.load (std::memory_order_relaxed)
                                 : A.wideCamera.load (std::memory_order_relaxed);

            if (alvo > 0 && intendedCamera.load() != alvo)
            {
                Command c;
                c.type = Command::Type::Cut; c.camera = alvo; c.channel = -1;
                c.simulated = A.testMode.load (std::memory_order_relaxed);
                c.timeMs = timeMs;
                if (A.enabled.load (std::memory_order_relaxed) && commands.push (c))
                {
                    intendedCamera.store (alvo);
                    if (! c.simulated) liveCamera.store (alvo);
                    lastCutMs = timeMs;
                    ++crossTalks;
                }
            }
        }

        // Um controle so manda no retorno: o HOLD do canal que estava no ar.
        //
        // Antes havia tres prazos concorrendo — hold, "silencio antes do BG" e
        // plano minimo — e valia o maior. Na pratica isso significava mexer no
        // hold e nada mudar, porque outro prazo maior estava mandando. Um
        // controle que nao controla e pior que controle nenhum.
        //
        // O plano minimo continua existindo, mas para o que ele serve de fato:
        // impedir pingue-pongue entre CANAIS. Nao atrasa a volta ao padrao.
        if (! quiet && ! anyActive && wide > 0 && intendedCamera.load() != wide
            && timeMs > holdUntilMs)
        {
            Command c;
            c.type = Command::Type::Cut; c.camera = wide; c.channel = -1;
            c.simulated = A.testMode.load (std::memory_order_relaxed);
            c.timeMs = timeMs;
            if (A.enabled.load (std::memory_order_relaxed))
            {
                commands.push (c);
                // a intencao muda mesmo em modo de teste — senao a mesa acha que
                // nunca cortou e repete o comando a cada plano minimo, para sempre
                intendedCamera.store (wide);
                if (! c.simulated) liveCamera.store (wide);
                lastCutMs = timeMs;
            }
        }
    }

    /** DISPARO DE TESTE: manda o comando sem depender do audio.
        Serve para responder "o vMix esta recebendo?" antes de discutir threshold.
        Ignora dominancia, permanencia e plano minimo — e um teste, nao producao. */
    bool testFire (MixerEngine& mix, int channel) noexcept
    {
        if (channel < 0 || channel >= mix.numChannels()) return false;
        const int cam = mix.channel (channel).params.trigger.camera.load (std::memory_order_relaxed);
        if (cam <= 0) return false;

        Command c;
        c.type = Command::Type::Cut;
        c.camera = cam; c.channel = channel; c.manual = true;
        c.levelDb = mix.channel (channel).tapDb (TapPoint::Input);
        c.simulated = mix.automation.testMode.load (std::memory_order_relaxed);
        c.timeMs = timeMs;
        mix.channel (channel).params.trigger.command.copyTo (c.text, CommandText::kMaxLen);
        c.target = mix.channel (channel).params.trigger.target.load (std::memory_order_relaxed);
        if (! commands.push (c)) return false;

        lastCutMs = timeMs;
        intendedCamera.store (cam);
        if (! c.simulated) liveCamera.store (cam);

        // O disparo manual tem que RESPEITAR o hold do canal, senao a mesa
        // corta e volta ao plano geral no instante seguinte — que e o que
        // acontecia: sem audio, o contador de silencio ja estava vencido e o
        // retorno ao BG saia junto com o corte.
        holdSince (mix.channel (channel).params.trigger.holdMs.load (std::memory_order_relaxed));
        return true;
    }

    /** Segura o plano atual por N ms antes de qualquer retorno automatico. */
    void holdSince (float ms) noexcept
    {
        lastActiveMs = timeMs;
        holdUntilMs = timeMs + double (ms);
    }

    /** Mesma ideia, apontando direto para uma camera — util para varrer todas. */
    bool testCamera (MixerEngine& mix, int camera) noexcept
    {
        if (camera <= 0) return false;
        Command c;
        c.type = Command::Type::Cut;
        c.camera = camera; c.channel = -1; c.manual = true;
        c.simulated = mix.automation.testMode.load (std::memory_order_relaxed);
        c.timeMs = timeMs;
        if (! commands.push (c)) return false;
        lastCutMs = timeMs;
        intendedCamera.store (camera);
        if (! c.simulated) liveCamera.store (camera);
        holdSince (mix.automation.manualHoldMs.load (std::memory_order_relaxed));
        return true;
    }

    /** Envia o comando de ON/OFF do canal AGORA, mesmo que o estado nao tenha
        mudado. E o que acontece quando o operador aperta ON com o canal ja
        aberto: relanca o cartucho. A deteccao por borda continua valendo para o
        estado — isto aqui e uma acao explicita, nao um estado repetido. */
    bool fireChannelLogic (MixerEngine& mix, int channel, bool on) noexcept
    {
        if (channel < 0 || channel >= mix.numChannels()) return false;
        auto& ch = mix.channel (channel);
        if (! ch.params.logicEnabled.load (std::memory_order_relaxed)) return false;

        Command c;
        c.type = Command::Type::None;
        c.channel = channel;
        c.manual = true;
        c.target = ch.params.logicTarget.load (std::memory_order_relaxed);
        c.timeMs = timeMs;
        const int len = on ? ch.params.onCommand .copyTo (c.text, CommandText::kMaxLen)
                           : ch.params.offCommand.copyTo (c.text, CommandText::kMaxLen);
        if (len == 0) return false;

        prevOn[size_t (channel)] = on;      // evita disparo duplicado pela borda
        return commands.push (c);
    }

    TriggerState stateOf (int ch) const noexcept { return triggers[size_t (ch)]->current(); }
    /** Camera efetivamente no ar (em modo de teste nao muda). */
    int  camera() const noexcept { return liveCamera.load (std::memory_order_relaxed); }
    /** O que a mesa decidiu, mesmo em modo de teste — evita comando repetido. */
    int  intended() const noexcept { return intendedCamera.load (std::memory_order_relaxed); }
    int  rejections() const noexcept { return rejected.load (std::memory_order_relaxed); }
    int  ignoredByMinShot() const noexcept { return ignored.load (std::memory_order_relaxed); }

    CommandQueue commands;
    CommandQueue rejectedLog;   // eventos descartados, para o log da superficie

private:
    /** Fader-start: dispara na BORDA, nao a cada bloco.
        Canal abriu -> comando de play; fechou -> comando de pause. */
    void processLogic (MixerEngine& mix, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            auto& ch = mix.channel (i);
            if (! ch.params.logicEnabled.load (std::memory_order_relaxed)) continue;

            const bool on = ch.params.on.load (std::memory_order_relaxed)
                         && ! ch.params.mute.load (std::memory_order_relaxed);
            if (on == prevOn[size_t (i)]) continue;
            prevOn[size_t (i)] = on;

            Command c;
            c.type = Command::Type::None;
            c.channel = i;
            c.target = ch.params.logicTarget.load (std::memory_order_relaxed);
            c.timeMs = timeMs;
            const int len = on ? ch.params.onCommand .copyTo (c.text, CommandText::kMaxLen)
                               : ch.params.offCommand.copyTo (c.text, CommandText::kMaxLen);
            if (len > 0) commands.push (c);
        }
    }

    bool isDominant (int i, const MixerEngine& mix, float marginDb) const noexcept
    {
        const float mine = levels[size_t (i)];
        for (size_t j = 0; j < levels.size(); ++j)
        {
            if (int (j) == i) continue;
            if (! mix.channel (int (j)).params.trigger.enabled.load (std::memory_order_relaxed)) continue;
            if (levels[j] > mine - marginDb && levels[j] > mine) return false;
        }
        return true;
    }

    void requestCut (MixerEngine& mix, const TriggerEvent& ev) noexcept
    {
        const auto& A = mix.automation;
        if (! A.enabled.load (std::memory_order_relaxed)) return;
        if (ev.camera <= 0) return;

        // O plano minimo existe para evitar corta-corta ENTRE cameras de canal.
        // Sair do plano geral para quem esta falando e o caso em que menos faz
        // sentido bloquear: alguem esta no ar falando enquanto a mesa mostra a
        // geral, que e o pior resultado possivel.
        const int wide = A.wideCamera.load (std::memory_order_relaxed);
        const bool saindoDaGeral = wide > 0 && intendedCamera.load() == wide
                                && A.geralInterrompivel.load (std::memory_order_relaxed);

        if (! saindoDaGeral
            && timeMs - lastCutMs < A.minShotMs.load (std::memory_order_relaxed))
        {
            ignored.fetch_add (1, std::memory_order_relaxed);
            pushEvent (ev);                      // vira linha de log, nao vira corte
            return;
        }

        Command c;
        c.type = Command::Type::Cut;
        c.camera = ev.camera; c.channel = ev.channel; c.levelDb = ev.levelDb;
        c.simulated = A.testMode.load (std::memory_order_relaxed);
        c.timeMs = timeMs;
        mix.channel (ev.channel).params.trigger.command.copyTo (c.text, CommandText::kMaxLen);
        c.target = mix.channel (ev.channel).params.trigger.target.load (std::memory_order_relaxed);
        commands.push (c);

        lastCutMs = timeMs;
        // O hold NAO e armado aqui: ele acompanha a fala e congela quando ela
        // termina, em processBlock. Armar no corte fazia o prazo vencer no meio
        // da propria fala, e a retencao prometida na tela nunca acontecia.
        intendedCamera.store (ev.camera);
        if (! c.simulated) liveCamera.store (ev.camera);
    }

    void pushEvent (const TriggerEvent& ev) noexcept
    {
        rejected.fetch_add (1, std::memory_order_relaxed);
        Command c;
        c.type = Command::Type::None;
        c.camera = ev.camera; c.channel = ev.channel;
        c.levelDb = ev.levelDb; c.timeMs = timeMs;
        rejectedLog.push (c);
    }

    std::vector<std::unique_ptr<ChannelTrigger>> triggers;
    std::vector<float> levels;
    std::vector<bool>  prevOn;
    double timeMs = 0.0, lastCutMs = -1e9, holdUntilMs = 0.0, lastActiveMs = 0.0;
    bool wasQuiet = false;
    double multiTalkSince = -1.0;
    std::atomic<int> liveCamera { 0 }, intendedCamera { 0 }, rejected { 0 }, ignored { 0 };

};

} // namespace mesa
