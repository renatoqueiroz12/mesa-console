#pragma once
#include "Atraso.h"
#include "Meter.h"
#include "AutoTrim.h"
#include "DspRack.h"
#include "Diagnostics.h"
#include "AutoMixer.h"
#include "SpeechSegmenter.h"
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

/** OS QUATRO USOS QUE A MESA REALMENTE DISTINGUE.

    Havia dez na lista e so quatro comportamentos: "MIC Externo" e "Player de
    PC" eram identicos a "Linha", e "MIC Produtor" e "MIC Convidado (controle)"
    identicos a "MIC Operador". Escolha que nao muda nada e pior que escolha
    que falta - manda o operador decidir uma coisa que nao existe.

    Nao da para reduzir a "microfone" so, como seria bom: microfone no controle
    e microfone no estudio cortam CAIXAS DIFERENTES.

    Os numeros sao os antigos de proposito. Renumerar trocaria o uso de toda
    configuracao ja gravada - um telefone viraria linha e perderia o retorno,
    sem ninguem perceber ate alguem se ouvir no ar. */
enum class Uso
{
    MicControle = int (SourceType::Operator),      // abrir muta o monitor do controle
    MicEstudio  = int (SourceType::StudioGuest),   // abrir muta o monitor do estudio
    TelefoneOuExterna = int (SourceType::Phone),   // recebe mix-minus
    Linha       = int (SourceType::Line)           // nao faz nada de especial
};

/** Leva qualquer tipo antigo para um dos quatro, SEM mudar comportamento. */
inline int usoCanonico (int t) noexcept
{
    if (t == int (SourceType::Operator) || t == int (SourceType::Producer)
        || t == int (SourceType::CrGuest))        return int (Uso::MicControle);
    if (t == int (SourceType::StudioGuest))       return int (Uso::MicEstudio);
    if (t == int (SourceType::Phone) || t == int (SourceType::Codec)
        || t == int (SourceType::StudioFeed))     return int (Uso::TelefoneOuExterna);
    return int (Uso::Linha);
}

/** O nome diz a SALA e da um exemplo.

    "Microfone no controle" e "Microfone no estudio" descreviam a regra certa e
    ainda assim faziam parar para pensar: quem opera chama de MIC do operador e
    MIC do locutor, nao pelo comodo. O exemplo entre travessoes resolve sem
    mentir - o de controle nao e so do operador, serve para o produtor e para
    convidado sentado do lado das caixas do controle. */
inline const char* nomeDoUso (int t) noexcept
{
    switch (usoCanonico (t))
    {
        case int (Uso::MicControle):       return "MIC controle - operador, produtor";
        case int (Uso::MicEstudio):        return "MIC estudio - locutor, convidado";
        case int (Uso::TelefoneOuExterna): return "Telefone ou externa - recebe retorno";
        default:                           return "Linha - playout, vinheta, musica";
    }
}

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
    std::atomic<float> triggerMs    { 300.0f };
    std::atomic<float> holdMs       { 2500.0f };
    std::atomic<float> releaseMs    { 400.0f };
    std::atomic<float> cooldownMs   { 1000.0f };
    std::atomic<float> hysteresisDb {   6.0f };
    std::atomic<int>   camera       { 0 };     // 0 = sem camera

    /** Comando literal enviado quando este canal dispara. Vazio = a mesa monta
        um CUT a partir do numero da camera. Preenchido, manda exatamente isto —
        serve para overlay, transicao, atalho do vMix ou outro sistema. */
    CommandText command;
    CommandText releaseCommand;   // opcional: enviado quando o canal solta
    std::atomic<int> target { 0 };   // indice em Settings::targets
};

/** De onde vem o sinal do canal. */
/** De onde o canal tira o audio.

    Gerador e uma fonte de verdade, nao um atalho: por um fader dentro dele,
    o tom passa por trim, DSP, fader, pan e barramento como qualquer outra
    fonte - e e assim que se calibra o fader contra o medidor sem depender de
    nada externo. */
enum class InputKind { Device = 0, Network, Gerador };

struct ChannelParams
{
    std::atomic<int>      inputKind  { int (InputKind::Device) };
    std::atomic<int>      inputIndex { -1 };     // indice na lista da origem; -1 = sem fonte
    std::atomic<int>      sourceType { int (SourceType::Line) };
    std::atomic<float>    trimDb     { 0.0f };   // trim manual (-25..+25)
    std::atomic<float>    faderDb    { -100.0f };
    std::atomic<float>    panPos     { 0.0f };   // -1 esq .. +1 dir
    /** Input carregado, mas sem fonte escolhida.

        Diferente de canal vazio: aqui existe input, com nome e ajustes, e
        ninguem apontou de onde vem o audio. A tira precisa dizer isso — antes
        mostrava o nome normalmente e simplesmente nao saia som, o que parece
        defeito da mesa. */
    std::atomic<bool>     semFonte   { false };
    std::atomic<bool>     on         { false };
    std::atomic<bool>     mute       { false };
    std::atomic<bool>     cue        { false };
    std::atomic<unsigned> busMask    { 1u };     // bit 0 = PGM1 ... bit 3 = PGM4

    // --- mix-minus (feed to source)
    /** DUAS fontes que se alternam sozinhas com o ON/OFF do canal, como na
        Axia. E o que faz um telefone funcionar sem ninguem operar nada:

          canal ON  -> feedBusOn  (o programa: PGM 1)
          canal OFF -> feedBusOff (a comunicacao: PGM 4)

        Fora do ar o interlocutor ouve o barramento de comunicacao, onde o
        operador e o apresentador falam com ele; entrando no ar, o retorno vira
        o programa. Nos DOIS casos sem a propria voz — e isso nao e opcao, e a
        razao de existir do mix-minus. */
    /** Segunda ponta de uma fonte ESTEREO: -1 = canal mono, que e o normal.
        Para rede e o outro slot; para placa, a outra entrada. */
    std::atomic<int>   inputIndexR { -1 };

    /** Atraso deste canal, em ms, para casar com o video. 0 = sem atraso. */
    std::atomic<float> atrasoMs { 0.0f };

    /** ON leva o fader para 0 dB.

        Liga o canal, o fader vai para a unidade - o ponto de referencia da
        mesa. Serve para operacao rapida: o operador aperta ON e ja esta no
        nivel, sem procurar o lugar do fader. Desligado de fabrica, porque
        mexer no fader de quem nao pediu e o tipo de surpresa que ninguem quer
        no ar. */
    std::atomic<bool> onVaiParaZero { false };

    std::atomic<int>   feedBusOn  { 0 };   // 0..3 = PGM 1..4
    std::atomic<int>   feedBusOff { 3 };
    /** Par de saida da placa PRINCIPAL. Para mandar por Livewire ou placa
        secundaria, aponte uma saida com "Mix-minus" em Configuracoes. */
    std::atomic<int>   feedOutputPair { -1 };
    std::atomic<float> feedDimDb { -10.0f };   // quanto abaixa o programa durante o talkback
    std::atomic<bool>  talkTo { false };       // operador falando com esta fonte
    AutoTrimParams        autoTrim;
    AutoMixParams         autoMix;
    SpeechSegmenter::Params fala;
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
        comunic .assign (size_t (maxBlockSize), 0.0f);
        scratchR .assign (size_t (maxBlockSize), 0.0f);
        preFaderR.assign (size_t (maxBlockSize), 0.0f);
        comunicR .assign (size_t (maxBlockSize), 0.0f);
        soma     .assign (size_t (maxBlockSize), 0.0f);
        bruto.assign (size_t (maxBlockSize), 0.0f);
        meterIn .prepare (sr);
        meterInR.prepare (sr);
        meterDsp.prepare (sr);
        meterOutR.prepare (sr);
        meterOut.prepare (sr);
        rack.prepare (sr, maxBlockSize);
        autoTrim.prepare (sr);
        autoMixer.prepare (sr, maxBlockSize);
        segmentador.prepare (sr, maxBlockSize);
        trimGain .prepare (sr, 25.0f);
        faderGain.prepare (sr, 20.0f);
        comGain  .prepare (sr, 20.0f);
        atraso .prepare (sr, maxBlockSize);
        atrasoR.prepare (sr, maxBlockSize);
        presence.reset();
        trimGain .setTargetDb (0.0f);  trimGain .snap();
        faderGain.setTargetDb (kMinusInfDb); faderGain.snap();
    }

    /** in pode ser nullptr (canal sem fonte). Retorna o buffer processado da
        esquerda; em canal estereo, a direita sai por processedDataR(). */
    const float* process (const float* in, int n) noexcept { return process (in, nullptr, n); }

    /** @param inR  segunda ponta de uma fonte ESTEREO. nullptr = canal mono,
                    e entao tudo se comporta exatamente como sempre se
                    comportou - os acessores da direita devolvem a esquerda. */
    const float* process (const float* in, const float* inR, int n) noexcept
    {
        float* dst = scratch.data();
        float* dstR = scratchR.data();
        estereo = (inR != nullptr);

        if (in == nullptr)
        {
            estereo = false;
            for (int i = 0; i < n; ++i)
            { dst[i] = 0.0f; preFader[size_t (i)] = 0.0f; comunic[size_t (i)] = 0.0f; }
            meterIn.silence(); meterDsp.silence(); meterOut.silence();
            for (int t = 0; t < int (TapPoint::NumTaps); ++t)
                taps[t].store (kMinusInfDb, std::memory_order_relaxed);
            return dst;
        }

        for (int i = 0; i < n; ++i) dst[i] = in[i];
        if (estereo) for (int i = 0; i < n; ++i) dstR[i] = inR[i];

        // ATRASO NA ENTRADA, antes de tudo.
        //
        // Aqui e nao depois porque assim TUDO que sai deste canal anda junto:
        // barramento, direct out, CUE, mix-minus, medidores e o gravador. Se
        // atrasasse so na saida, o medidor mostraria uma coisa e o ar levaria
        // outra, e o operador perderia a referencia.
        //
        // As duas pontas do estereo passam por linhas separadas, mas com o
        // MESMO ajuste - atraso diferente por lado desmancharia a imagem.
        atraso.defineMs (params.atrasoMs.load (std::memory_order_relaxed));
        atraso.processa (dst, n);
        if (estereo)
        {
            atrasoR.defineMs (params.atrasoMs.load (std::memory_order_relaxed));
            atrasoR.processa (dstR, n);
        }

        // O QUE OS MEDIDORES OLHAM.
        //
        // Em estereo, a mistura das duas pontas. Uma fonte com conteudo so na
        // direita mediria silencio se olhassemos so a esquerda - e dai sairiam
        // erradas tres coisas de uma vez: o medidor da tira, o auto-trim e o
        // nivelador automatico.
        // Copia crua da ENTRADA, antes de trim, DSP e fader.
        //
        // Serve ao gravador de diagnostico: para saber ONDE o audio se
        // estraga, precisamos de um ponto de escuta que nao passou por
        // nenhum processamento nosso.
        for (int i = 0; i < n; ++i) bruto[size_t (i)] = in[i];

        // 1. medicao de entrada: CADA LADO O SEU.
        //
        // A esquerda media a mistura das duas pontas, de quando a barra era
        // uma so. Agora que ha uma coluna por lado, medir a mistura na
        // esquerda faria as duas mentirem: com a direita muda, a esquerda
        // marcaria 6 dB abaixo do que realmente chega nela.
        //
        // O auto-trim passa a olhar a esquerda tambem, e nao a mistura. Ele
        // corrige o nivel da FONTE inteira, e em material normal os dois lados
        // andam juntos; a diferenca so apareceria num estereo muito desigual,
        // que e caso de conserto e nao de correcao automatica.
        meterIn.process (in, n);
        // TODOS OS MEDIDORES DA MESA MEDEM PICO.
        //
        // As tiras mostravam media rapida e as barras de programa, pico. O
        // mesmo audio dava numeros diferentes em duas reguas lado a lado - com
        // o tom de 1 kHz, 0 no programa e -3 na tira. Regua que discorda de
        // outra regua nao calibra nada.
        //
        // Pico e a escolha certa para as duas: e o que se usa em radio para
        // controlar nivel, e e o unico que responde a pergunta "isto vai
        // estourar?". O tom de alinhamento tambem e referenciado em pico (EBU
        // R68), entao os tres - gerador, tira e programa - passam a falar a
        // mesma lingua.
        // MEDICAO POR LADO.
        //
        // Antes tudo era medido na mistura das duas pontas, e barra mono nao
        // denuncia o que mais importa em estereo: um lado mudo. Playout com um
        // canal caido, cabo solto numa entrada de linha, fonte que chegou mono
        // sem ninguem perceber - nada disso aparecia.
        //
        // Em canal mono as duas medidas sao a mesma e as duas metades da barra
        // andam juntas: nao ha caso especial para tratar.
        meterInR.process (estereo ? inR : in, n);

        taps [int (TapPoint::Input)].store (meterIn .peakDb(), std::memory_order_relaxed);
        tapsR[int (TapPoint::Input)].store (meterInR.peakDb(), std::memory_order_relaxed);

        // 2. auto trim + trim manual, aplicados como um unico ganho suavizado
        const float atDb = autoTrim.update (meterIn.rmsDb(), n, params.autoTrim);
        trimGain.setTargetDb (params.trimDb.load (std::memory_order_relaxed) + atDb);
        // as duas pontas andam com o MESMO ganho: ganho diferente por lado
        // moveria a imagem estereo a cada ajuste de trim
        for (int i = 0; i < n; ++i)
        {
            const float g = trimGain.next();
            dst[i] *= g;
            if (estereo) dstR[i] *= g;
        }
        tapsR[int (TapPoint::PostAutoTrim)].store (meterInR.peakDb()
                                                   + gainToDb (trimGain.currentGain()),
                                                   std::memory_order_relaxed);
        taps[int (TapPoint::PostAutoTrim)].store (meterIn.peakDb()
                                                  + gainToDb (trimGain.currentGain()),
                                                  std::memory_order_relaxed);

        // 3. rack de DSP: gate, EQ, compressor, de-esser, limiter e, futuramente, VST3
        //
        // SO EM CANAL MONO, por enquanto. O rack guarda estado - biquads do EQ,
        // envelope do compressor - e passar duas pontas pelo mesmo objeto
        // embaralharia esse estado a cada bloco. Dois racks resolveriam o
        // estado, mas cada um teria seus proprios parametros e a deteccao das
        // dinamicas ficaria solta: compressor agindo diferente em cada lado
        // move a imagem. O certo e deteccao ligada, e isso pede mexer nos cinco
        // processadores - fica para depois.
        //
        // Na pratica atinge pouco: canal estereo aqui e playout, onde o rack
        // costuma estar desligado. Mas o menu do canal avisa, porque DSP que
        // nao age sem ninguem dizer nada e pior do que DSP que falta.
        if (! estereo) rack.process (dst, n);

        // guarda o sinal pre-fader: e ele que alimenta o CUE (PFL) e o Audio Trigger
        for (int i = 0; i < n; ++i) preFader[size_t (i)] = dst[i];
        if (estereo) for (int i = 0; i < n; ++i) preFaderR[size_t (i)] = dstR[i];

        // O NIVEL QUE O AUTO FADER OLHA: pre-fader, ja com trim e DSB.
        //
        // Em canal estereo isto estava errado desde que o estereo entrou: o
        // auto fader e o medidor de DSP recebiam a mistura CRUA da entrada,
        // antes do trim. Mediam um nivel que nao era o que iam corrigir, e
        // ajustar o trim nao mudava a decisao deles.
        const float* nivelPre = preFader.data();
        if (estereo)
        {
            for (int i = 0; i < n; ++i)
                soma[size_t (i)] = 0.5f * (preFader[size_t (i)] + preFaderR[size_t (i)]);
            nivelPre = soma.data();
        }

        meterDsp.process (nivelPre, n);
        taps[int (TapPoint::PostDsp)].store (meterDsp.peakDb(), std::memory_order_relaxed);

        // Transcricao: escuta a ENTRADA, nao o pos-fader. O texto do que foi
        // dito nao deve depender de o operador ter aberto o canal ou de onde
        // deixou o fader.
        // TRAVA o aviso ate alguem recolher.
        //
        // Antes era atribuicao direta: ficava verdadeiro no bloco em que o
        // trecho fechava e voltava a falso no bloco seguinte, 0,167 ms depois
        // com buffer de 8. A interface olha 25 vezes por segundo — nunca
        // pegava. O trecho era montado e descartado sem ninguem ver.
        if (segmentador.process (params.fala, in, n))
            falaPronta.store (true, std::memory_order_release);

        // 4. nivelador: mede o sinal PRE-fader e decide onde o fader deve estar.
        // Medir depois do fader fecharia uma malha que oscila.
        const bool ligado = params.on.load (std::memory_order_relaxed);

        // ON -> 0 dB, no MOTOR e nao no botao.
        //
        // Aqui pega todo caminho que liga o canal: a tela, o GPIO, o comando
        // remoto e a automacao. Posto no botao, valeria so para quem clica - e
        // o canal ligado por fader-start ficaria onde estava, o que e
        // exatamente o caso em que o operador nao esta olhando.
        if (ligado && ! ligadoAntes
            && params.onVaiParaZero.load (std::memory_order_relaxed))
        {
            params.faderDb.store (0.0f, std::memory_order_relaxed);
            autoMixer.reposiciona (0.0f);     // o nivelador parte dali, sem salto
        }
        ligadoAntes = ligado;

        const bool open = ligado && ! params.mute.load (std::memory_order_relaxed);

        const float operatorDb = params.faderDb.load (std::memory_order_relaxed);
        // A saida do bloco ANTERIOR fecha a malha. Um bloco de atraso num laco
        // que anda 6 dB por segundo nao muda nada - e e o unico jeito de olhar
        // a saida antes de decidir o fader deste bloco.
        // O laco olha o MESMO numero que a barra desenha - agora pico, como
        // todos os medidores da mesa. Pediu -18, a barra para em -18.
        const float saidaDb = meterOut.peakDb();

        // a MESMA leitura que a barra IN desenha, para o limiar ser o numero
        // que o operador ve
        const float entradaDb = taps[int (TapPoint::PostAutoTrim)].load (std::memory_order_relaxed);

        const float rideDb = autoMixer.process (params.autoMix, nivelPre, n,
                                                operatorDb, open, saidaDb, entradaDb);
        if (params.autoMix.enabled.load (std::memory_order_relaxed) && open)
        {
            // escreve de volta: o fader ANDA na tela, e o operador ve o que a
            // mesa esta fazendo em vez de adivinhar
            params.faderDb.store (rideDb, std::memory_order_relaxed);
        }

        // 5b. SAIDA DE COMUNICACAO: o canal no nivel do fader, IGNORANDO o
        //     ON/OFF.
        //
        //     E o que faz falar com um telefone antes de coloca-lo no ar. O
        //     barramento de comunicacao so recebia canal aberto, entao para
        //     falar com o ouvinte era preciso entrar no ar primeiro — que e
        //     exatamente o que ninguem quer fazer.
        //
        //     Usa o nivel do OPERADOR e nao o do nivelador automatico: o
        //     nivelador existe para a mistura no ar, e aqui nao ha ar. Respeita
        //     o MUTE, que e ordem explicita de silencio, ao contrario do OFF,
        //     que so quer dizer "fora do ar".
        comGain.setTargetDb (params.mute.load (std::memory_order_relaxed)
                                 ? kMinusInfDb : operatorDb);
        for (int i = 0; i < n; ++i)
        {
            const float g = comGain.next();
            comunic[size_t (i)] = preFader[size_t (i)] * g;
            if (estereo) comunicR[size_t (i)] = preFaderR[size_t (i)] * g;
        }

        // 5. fader + on/off + mute
        faderGain.setTargetDb (open ? rideDb : kMinusInfDb);
        for (int i = 0; i < n; ++i)
        {
            const float g = faderGain.next();
            dst[i] *= g;
            if (estereo) dstR[i] *= g;
        }

        meterOut .process (dst, n);
        meterOutR.process (estereo ? dstR : dst, n);
        taps [int (TapPoint::PostFader)].store (meterOut .peakDb(), std::memory_order_relaxed);
        tapsR[int (TapPoint::PostFader)].store (meterOutR.peakDb(), std::memory_order_relaxed);

        // diagnostico: ha sinal chegando? esta clipando? o calibrador precisa medir?
        const float blockMs = float (n) / float (sampleRate) * 1000.0f;
        presence.update (meterIn.fastDb(), blockMs, meterIn.clipped (true));
        calibrator.update (meterIn.fastDb(), blockMs);

        return dst;
    }

    /** Nivel para o GATILHO DE CAMERA: media rapida, nao pico.

        Os medidores da mesa passaram a mostrar pico, que e o certo para
        controlar nivel. O gatilho pergunta outra coisa - "alguem esta falando
        aqui?" - e para isso a media e melhor: um estalo de teclado tem pico
        alto e media baixa, e nao deve trocar a camera. Alem disso os
        thresholds ja ajustados foram calibrados na media; mudar a medida por
        baixo os deslocaria todos de uma vez, sem ninguem pedir. */
    float nivelParaGatilho (TapPoint t) const noexcept
    {
        switch (t)
        {
            case TapPoint::Input:        return meterIn.fastDb();
            case TapPoint::PostAutoTrim: return meterIn.fastDb() + gainToDb (trimGain.currentGain());
            case TapPoint::PostDsp:      return meterDsp.fastDb();
            default:                     return meterOut.fastDb();
        }
    }

    /** O mesmo ponto, na ponta DIREITA. Em canal mono devolve a esquerda. */
    float tapDbR (TapPoint t) const noexcept
    {
        return tapsR[int (t)].load (std::memory_order_relaxed);
    }

    float tapDb (TapPoint t) const noexcept
    {
        return taps[int (t)].load (std::memory_order_relaxed);
    }

    float autoTrimDb() const noexcept { return autoTrim.currentDb(); }
    /** Para a tela mostrar o que o nivelador esta enxergando e decidindo. */
    const AutoMixer& nivelador() const noexcept { return autoMixer; }

    /** Onde o nivelador colocou o fader. Igual ao fader quando esta desligado. */
    float autoMixFaderDb() const noexcept { return autoMixer.currentFaderDb(); }

    /** Entrada crua do bloco atual, sem nada aplicado. */
    const float* entradaBruta() const noexcept { return bruto.data(); }

    /** Ha trecho de fala pronto para recolher. Zerado por quem recolher. */
    bool temFalaPronta() const noexcept { return falaPronta.load (std::memory_order_acquire); }
    bool falandoAgora() const noexcept { return segmentador.estaFalando(); }
    double duracaoFalaMs() const noexcept { return segmentador.duracaoAtualMs(); }
    void marcaFalaRecolhida() noexcept { falaPronta.store (false, std::memory_order_release); }
    const std::vector<float>& trechoDeFala() const noexcept { return segmentador.trecho(); }
    void limpaTrechoDeFala() noexcept { segmentador.limpaTrecho(); }

    /** Sinal pos-trim e pos-DSP, antes do fader e do ON/OFF. */
    const float* preFaderData() const noexcept { return preFader.data(); }
    /** O canal no nivel do fader, sem o portao de ON/OFF. E o que alimenta o
        barramento de comunicacao. */
    const float* comunicData() const noexcept { return comunic.data(); }
    /** Quanto este canal esta atrasando agora, em ms. */
    float atrasoAtualMs() const noexcept { return atraso.ms(); }
    void  limpaAtraso() noexcept { atraso.limpa(); atrasoR.limpa(); }

    /** A ponta DIREITA. Em canal mono devolve a esquerda de proposito: quem
        soma nos barramentos escreve uma linha so e vale para os dois casos. */
    bool  ehEstereo()  const noexcept { return estereo; }
    const float* processedDataR() const noexcept { return estereo ? scratchR .data() : scratch .data(); }
    const float* preFaderDataR()  const noexcept { return estereo ? preFaderR.data() : preFader.data(); }
    const float* comunicDataR()   const noexcept { return estereo ? comunicR .data() : comunic .data(); }

    /** Sinal pos-fader do ultimo bloco — usado pelo mix-minus. */
    const float* processedData() const noexcept { return scratch.data(); }

    bool isMicOpen() const noexcept
    {
        return params.on.load (std::memory_order_relaxed)
            && ! params.mute.load (std::memory_order_relaxed);
    }

    ChannelParams params;
    Meter meterIn, meterDsp, meterOut;
    /** Ponta direita. Em canal mono recebe o mesmo sinal da esquerda. */
    Meter meterInR, meterOutR;
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
    std::vector<float> scratch, preFader, comunic, bruto;
    /** Caminho da direita. So tem conteudo proprio em canal ESTEREO; em mono
        os acessores devolvem o buffer da esquerda, e quem consome nao precisa
        perguntar de que tipo o canal e. */
    std::vector<float> scratchR, preFaderR, comunicR;
    /** Mistura das duas pontas, so para MEDIR. Medidor, auto-trim, nivelador e
        segmentador de fala olham o canal inteiro: fonte com conteudo so na
        direita mediria silencio se olhassemos so a esquerda. */
    std::vector<float> soma;
    bool estereo = false;
    double sampleRate = 48000.0;
    AutoTrim      autoTrim;
    AutoMixer     autoMixer;
    SpeechSegmenter segmentador;
    std::atomic<bool> falaPronta { false };
    SmoothedGain  trimGain, faderGain, comGain;
    Atraso        atraso, atrasoR;
    bool          ligadoAntes = false;
    /** Ponta direita de cada ponto de medicao. */
    std::atomic<float> tapsR[int (TapPoint::NumTaps)] {
        { -120.0f }, { -120.0f }, { -120.0f }, { -120.0f } };
    std::atomic<float> taps[int (TapPoint::NumTaps)] {
        { kMinusInfDb }, { kMinusInfDb }, { kMinusInfDb }, { kMinusInfDb } };
};

} // namespace mesa
