#pragma once
#include "Dsp.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mesa {

/** GERADOR DE SINAL DE TESTE, como o de qualquer mesa de bancada.

    Serve para calibrar saida sem depender de fonte externa: manda um sinal
    conhecido para um destino e confere o que chega do outro lado - no medidor
    do QOR, no processador, no gravador. Sem isso, ajustar nivel de saida vira
    tentativa e erro com o programa no ar.

    Tres sinais, e cada um responde a uma pergunta diferente:

    - 1 kHz: nivel. E o tom de referencia de toda a cadeia de radio, e o que se
      usa para acertar "o que sai daqui em 0 chega la em 0".
    - ruido rosa: resposta de frequencia e equalizacao de sala. Tem a mesma
      energia por oitava, que e como o ouvido divide o espectro - por isso soa
      plano, ao contrario do branco.
    - ruido branco: teste de cadeia e de ruido de fundo. Mesma energia por
      HERTZ, entao soa agudo demais para julgar sala.

    O NIVEL DO TOM E PICO; o dos ruidos e RMS. Nao e inconsistencia, e a
    norma: a EBU R68 define o tom de alinhamento pela AMPLITUDE da senoide, e e
    assim que o outro lado da linha espera receber. Com pico, -18 no gerador da
    -18 no medidor de pico e cai no zero da regua - que e a leitura que se quer
    ao calibrar.

    Ja ruido nao tem pico util: ele varia sem parar e o valor instantaneo nao
    diz nada. Por isso os dois ruidos sao referenciados em RMS, e seus picos
    ficam naturalmente acima do numero pedido. */
class GeradorTeste
{
public:
    enum class Tipo { Tom1k = 0, RuidoRosa, RuidoBranco };

    std::atomic<bool>  ativo    { false };
    std::atomic<int>   tipo     { int (Tipo::Tom1k) };
    std::atomic<float> nivelDb  { -18.0f };
    std::atomic<float> freqHz   { 1000.0f };

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        buffer.assign (size_t (maxBlockSize > 0 ? maxBlockSize : 512), 0.0f);
        fase = 0.0;
        for (auto& e : rosa) e = 0.0f;
        semente = 22222u;
    }

    /** O bloco gerado agora, para quem consome depois do mixer no mesmo
        ciclo - saidas de rede e de placa secundaria. */
    const float* ultimoBloco() const noexcept { return buffer.data(); }

    /** Gera o bloco e devolve o buffer. Fora do ar devolve silencio, sem
        parar de existir: quem consome nao precisa perguntar se esta ligado. */
    const float* processa (int n) noexcept
    {
        if (n > int (buffer.size())) n = int (buffer.size());
        float* d = buffer.data();

        if (! ativo.load (std::memory_order_relaxed))
        {
            for (int i = 0; i < n; ++i) d[i] = 0.0f;
            return d;
        }

        const float g = dbToGain (nivelDb.load (std::memory_order_relaxed));

        switch (Tipo (tipo.load (std::memory_order_relaxed)))
        {
            case Tipo::Tom1k:
            {
                // AMPLITUDE = o nivel pedido, sem correcao.
                //
                // Antes eu multiplicava por raiz de 2 para o RMS bater no
                // numero, e o medidor - que e de PICO - mostrava 3 dB a mais:
                // -18 no campo virava -15 na barra. Calibrar com regua que nao
                // fecha e pior que nao calibrar.
                const double f = double (freqHz.load (std::memory_order_relaxed));
                const double passo = 2.0 * 3.14159265358979323846 * f / sr;
                const float amp = g;
                for (int i = 0; i < n; ++i)
                {
                    d[i] = amp * float (std::sin (fase));
                    fase += passo;
                    if (fase > 2.0 * 3.14159265358979323846) fase -= 2.0 * 3.14159265358979323846;
                }
                break;
            }

            case Tipo::RuidoRosa:
                // Filtro de Voss-McCartney reduzido: soma de fontes brancas que
                // se renovam em ritmos diferentes, dando os 3 dB por oitava de
                // queda. Barato e estavel o bastante para calibracao.
                for (int i = 0; i < n; ++i)
                {
                    const float b = branco();
                    rosa[0] = 0.99886f * rosa[0] + b * 0.0555179f;
                    rosa[1] = 0.99332f * rosa[1] + b * 0.0750759f;
                    rosa[2] = 0.96900f * rosa[2] + b * 0.1538520f;
                    rosa[3] = 0.86650f * rosa[3] + b * 0.3104856f;
                    rosa[4] = 0.55000f * rosa[4] + b * 0.5329522f;
                    rosa[5] = -0.7616f * rosa[5] - b * 0.0168980f;
                    const float soma = rosa[0] + rosa[1] + rosa[2] + rosa[3]
                                     + rosa[4] + rosa[5] + rosa[6] + b * 0.5362f;
                    rosa[6] = b * 0.115926f;
                    // 0,325 e medido, nao estimado: com ganho 1 a soma do
                    // filtro da RMS 3,08, entao este fator leva o resultado
                    // para 1 e o nivel pedido passa a ser o nivel que sai.
                    d[i] = g * soma * 0.325f;
                }
                break;

            default:
                for (int i = 0; i < n; ++i) d[i] = g * branco();
                break;
        }

        return d;
    }

private:
    /** Ruido branco uniforme, com RMS proximo de 1. Gerador proprio para nao
        depender de rand(), que nao e seguro na thread de audio. */
    float branco() noexcept
    {
        semente = semente * 1664525u + 1013904223u;
        const float u = float (int32_t (semente >> 8)) * (1.0f / 8388608.0f) - 1.0f;
        return u * 1.732f;            // uniforme -1..1 tem RMS 1/raiz(3)
    }

    std::vector<float> buffer;
    double sr = 48000.0;
    double fase = 0.0;
    float  rosa[7] {};
    std::uint32_t semente = 22222u;
};

} // namespace mesa
