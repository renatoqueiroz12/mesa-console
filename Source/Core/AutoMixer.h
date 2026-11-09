#pragma once
#include "DspUtil.h"
#include <atomic>
#include <cmath>

namespace mesa {

/** AUTO FADER: um operador automatico no fader.

    Move o FADER — nao um ganho escondido — para manter o canal num alvo de
    nivel. O operador ve o fader andando, que e metade do valor da funcao:
    ele entende o que a mesa esta fazendo e pode assumir a qualquer momento.

    Por que medir ANTES do fader: medir depois fecharia uma malha (sobe o
    fader, le mais alto, abaixa o fader) que oscila. Medindo o sinal pre-fader
    e calculando o ganho necessario, o calculo e direto e nao realimenta.

    Por que o piso: sem ele, na pausa da fala o nivelador persegue o ruido de
    sala ate o teto, e a primeira silaba seguinte estoura. Abaixo do piso ele
    congela onde esta.

    Nao confundir com automix por compartilhamento de ganho (Dugan), que abaixa
    microfones que nao estao falando. Sao funcoes diferentes. */
struct AutoMixParams
{
    std::atomic<bool>  enabled     { false };
    std::atomic<float> targetDb    { -18.0f };  // alvo de RMS pos-fader
    /** Travas de SEGURANCA do fader automatico, nao ajuste de operacao.

        Eram -30 e +10 e apareciam na tela como se fossem regulagem. So
        escondiam o problema: fader encostado no teto nunca foi questao de
        teto, e sim de entrada fora do lugar. Agora sao largas o bastante para
        nao atrapalhar e servem so para o fader nao fugir se algo der errado. */
    std::atomic<float> maxFaderDb  {  20.0f };
    std::atomic<float> minFaderDb  { -40.0f };
    std::atomic<float> floorDb     { -45.0f };  // abaixo disso, congela
    /** Velocidade para SUBIR o fader, quando a saida esta baixa. Devagar e
        discreto: ninguem repara num nivel que cresce. */
    std::atomic<float> speedDbPerSec { 6.0f };

    /** Velocidade para BAIXAR o fader, quando a saida passou do alvo.

        Separada, e bem mais rapida, de proposito. Subir devagar e elegante;
        descer devagar deixa o pico passar inteiro antes de a correcao chegar -
        e pico que passa e o unico erro que se ouve. E a mesma razao pela qual
        um compressor tem attack curto e release longo. */
    std::atomic<float> descidaDbPerSec { 40.0f };

};

class AutoMixer
{
public:
    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        block = maxBlockSize;
        rms.prepare (sampleRate, 300.0f);   // janela longa: nivel de fala, nao silaba
        current = 0.0f;
        estimate = -120.0f;
        primed = false;
    }

    void reset() noexcept { primed = false; }

    /** Roda uma vez por bloco.

        MALHA FECHADA NA SAIDA. Antes ele fazia a conta "fader = alvo - nivel de
        entrada" e confiava que daria certo depois do fader. E malha aberta: se
        qualquer coisa no meio mudar o nivel - o DSP, o limiter, o trim
        automatico - a saida nao chega no alvo e ele nao fica sabendo.

        Agora ele olha a SAIDA e corrige ate ela bater no alvo, que e como um
        operador trabalha: ninguem calcula o fader, todo mundo olha a barra.

        O sinal de ENTRADA continua sendo usado, mas so para decidir SE deve
        mexer - se ha som, se a frase nao esta acabando. Sem isso, com o fader
        fechado a saida seria silencio e ele subiria para sempre atras dela.

        Devolve a posicao de fader em dB que o canal deve assumir. Se estiver
        desligado ou o canal fechado, devolve o valor do operador.

        @param saidaDb  nivel medido DEPOIS do fader, do bloco anterior. */
    /** @param entradaDb  nivel da entrada COMO A BARRA IN MOSTRA. -120 = usar
                           a medida propria (compatibilidade). */
    float process (const AutoMixParams& p, const float* preFader, int n,
                   float operatorFaderDb, bool channelOn,
                   float saidaDb = -120.0f, float entradaDb = -120.0f) noexcept
    {
        if (! p.enabled.load (std::memory_order_relaxed) || ! channelOn)
        {
            current = operatorFaderDb;   // acompanha, para nao dar salto ao ligar
            primed = true;
            return operatorFaderDb;
        }

        const float rmsDb = rms.processBlock (preFader, n);

        // A ENTRADA e medida como a barra IN mostra: media rapida.
        //
        // Assim o limiar que voce ajusta e o mesmo numero que voce le no
        // medidor - se o IN marca -30 com a sala em silencio, o limiar de -30
        // e o ponto exato. Medir por pico aqui obrigava a traduzir de cabeca
        // os 12 dB de crista da fala, toda vez.
        const float medidoDb = (entradaDb > -119.0f) ? entradaDb : rmsDb;

        // O PORTAO OLHA O NIVEL DE AGORA, nao a media de 300 ms.
        //
        // Aqui estava o defeito que levava o fader ao teto. Quando a fala para,
        // a SAIDA cai na hora - e silencio de verdade - mas a media da entrada
        // leva uns 300 ms para descer abaixo do limiar. Nessa janela o laco via
        // "saida em -100, alvo -18" e empurrava o fader para cima. Cada frase
        // ganhava um pouco, e em algumas dezenas de frases ele encostava no
        // teto sem o audio ter pedido nada.
        //
        // Medindo o bloco e soltando rapido, o portao fecha junto com o som.
        // Subida instantanea para nao perder o comeco da palavra.
        // O NUMERO QUE O PORTAO COMPARA E O NUMERO DA BARRA.
        //
        // Ele calculava um RMS proprio, e dava outra coisa: com a barra IN
        // marcando -20,8 este calculo dava -46,6. Quem punha o limiar em -19,5
        // olhando o medidor estava comparando com um numero 26 dB diferente, e
        // o auto fader simplesmente nunca ligava. Medida que so existe por
        // dentro nao serve para ajustar nada.
        float agoraDb;
        if (entradaDb > -119.0f)
        {
            agoraDb = entradaDb;
        }
        else
        {
            double soma = 0.0;
            for (int i = 0; i < n; ++i) soma += double (preFader[i]) * double (preFader[i]);
            agoraDb = gainToDb (float (std::sqrt (soma / n)));
        }

        if (agoraDb > portao) portao = agoraDb;
        else portao -= 120.0f * float (n) / float (sr);      // fecha em poucos ms

        // A SAIDA E LIDA COMO CHEGA, sem seguidor proprio.
        //
        // Havia um aqui, de queda lenta, de quando os dois lados mediam coisas
        // diferentes. Agora os dois vem da mesma media rapida, e o seguidor
        // virou um problema: ele fazia a leitura da saida DESPENCAR sozinha
        // enquanto a entrada ainda estava acima do limiar. O erro crescia sem
        // o audio ter mudado, e o fader subia atras de um numero que nao
        // existia - foi assim que ele chegou ao teto.
        saidaSeguida = saidaDb;

        // estimativa so para a tela e para a primeira volta; nao decide nada
        estimate = medidoDb;
        const float levelDb = medidoDb;

        if (! primed) { current = operatorFaderDb; primed = true; }

        // UMA REGRA SO: a ENTRADA passou do limiar?
        //
        // Antes havia mais duas travas por cima desta - uma "margem de fim de
        // frase" e um seguidor de queda lenta - herdadas de quando isto era um
        // nivelador de fala em malha aberta. Elas faziam o fader comecar e
        // parar em momentos sem relacao com o limiar ajustado, e o efeito era
        // parecer que ele atuava na hora errada, ou que estava olhando a saida
        // para decidir. Nao estava; so havia coisa demais decidindo.
        //
        // Agora e literalmente o que se espera: acima do limiar ele pilota,
        // abaixo congela. Quem evita perseguir ruido de sala e o limiar, que e
        // ajustavel e visivel - nao uma regra escondida.
        const bool speaking = portao > p.floorDb.load (std::memory_order_relaxed);

        if (speaking)
        {
            const float target = p.targetDb.load (std::memory_order_relaxed);
            // ERRO NA SAIDA: quanto falta para a barra chegar no alvo.
            //
            // Enquanto houver erro o fader anda; quando a saida bate no alvo,
            // ele para sozinho - sem precisar saber quanto vale o ganho da
            // cadeia. E por isso que isto sobrevive a DSP, limiter e trim
            // automatico mexendo no nivel no meio do caminho.
            //
            // Na primeira volta ainda nao ha saida medida: parte da conta de
            // malha aberta para nao levar segundos subindo do zero.
            const float erro = (saidaSeguida > -119.0f) ? (target - saidaSeguida)
                                                        : (target - levelDb - current);
            float alvoFader = current + erro;

            // Limites sao TRAVA DE SEGURANCA, nao ajuste. Encostar neles quer
            // dizer que a ENTRADA esta fora do lugar - falta ou sobra trim - e
            // nenhum fader resolve isso. Guardado para a tela poder dizer.
            const float lo = p.minFaderDb.load (std::memory_order_relaxed);
            const float hi = p.maxFaderDb.load (std::memory_order_relaxed);
            noLimite = (alvoFader > hi + 0.5f) ? 1 : (alvoFader < lo - 0.5f ? -1 : 0);
            desejado = alvoFader;
            if (alvoFader < lo) alvoFader = lo;
            if (alvoFader > hi) alvoFader = hi;

            // caminhada limitada: correcao instantanea seria bombeamento audivel
            // passo diferente para cada lado: rapido para tirar excesso,
            // devagar para repor
            const float diff = alvoFader - current;
            const float porSegundo = (diff < 0.0f)
                                       ? p.descidaDbPerSec.load (std::memory_order_relaxed)
                                       : p.speedDbPerSec  .load (std::memory_order_relaxed);
            const float step = porSegundo * float (n) / float (sr);

            if (diff >  step) current += step;
            else if (diff < -step) current -= step;
            else current = alvoFader;
        }
        return current;
    }

    float currentFaderDb() const noexcept { return current; }

    /** Recoloca o fader sem saltar: usado quando algo de fora o move, como o
        ON levando para 0 dB. */
    void reposiciona (float db) noexcept { current = db; primed = true; }

    /** Para a tela mostrar o que ele esta enxergando e decidindo. */
    float nivelMedidoDb()   const noexcept { return estimate; }
    /** Saida como o laco a enxerga, ja com a balistica do seguidor. */
    float saidaMedidaDb()   const noexcept { return saidaSeguida; }
    float faderDesejadoDb() const noexcept { return desejado; }
    /** 0 = dentro da faixa, +1 = pedindo mais que o teto (falta trim),
        -1 = pedindo menos que o piso (sobra trim). */
    int   limitado()        const noexcept { return noLimite; }

private:
    /** RMS com janela longa, em dB. */
    struct RmsDetector
    {
        void prepare (double sampleRate, float windowMs)
        {
            coeff = std::exp (-1.0f / (float (sampleRate) * windowMs * 0.001f));
            acc = 0.0f;
        }
        float processBlock (const float* x, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float s = x[i] * x[i];
                acc = coeff * acc + (1.0f - coeff) * s;
            }
            return gainToDb (std::sqrt (acc < 1.0e-12f ? 1.0e-12f : acc));
        }
        float coeff = 0.0f, acc = 0.0f;
    };

    RmsDetector rms;
    double sr = 48000.0;
    int block = 0;
    float current = 0.0f;
    float desejado = 0.0f, portao = -120.0f;
    float saidaSeguida = -120.0f;
    int   noLimite = 0;
    float estimate = -120.0f;
    bool primed = false;
};

} // namespace mesa
