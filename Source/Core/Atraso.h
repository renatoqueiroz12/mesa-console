#pragma once
#include <algorithm>
#include <vector>

namespace mesa {

/** ATRASO DE CANAL, para casar audio com video.

    Video passa por captura, conversao, mistura e codificacao; audio chega
    quase na hora. Sem atrasar o audio, a boca anda na frente da voz. O ajuste
    e por canal porque cada fonte tem o seu: uma camera com conversor e outra
    ligada direto nao atrasam igual.

    So atrasa; nunca adianta. Quem precisa de audio ANTES do video tem de
    atrasar o video, que e coisa de mixer de video e nao de mesa de som.

    O buffer e alocado uma vez, no prepare, com o maximo permitido. Trocar o
    atraso em operacao nao aloca e nao trava - so muda de onde se le. Alocar na
    thread de audio e a forma classica de furar o som ao mexer num controle. */
class Atraso
{
public:
    /** Teto de 2 segundos.

        Comecou em 500 ms, pensando em cadeia de estudio. Mas video que vai
        para stream ou passa por encoder acumula bem mais que isso, e ficar
        sem margem no ajuste e pior que gastar memoria.

        TETO TEM DE EXISTIR, e por isso: o buffer e alocado uma vez no arranque
        e nunca mais, justamente para mexer no controle durante a operacao nao
        alocar na thread de audio - que e a forma classica de furar o som. Sem
        limite nao ha quanto alocar.

        Custa 384 KB por linha a 48 kHz. Um canal estereo usa duas, e dezesseis
        canais dao uns 12 MB - pouco para uma maquina que trabalha com 60 MB no
        total. */
    static constexpr float kMaxMs = 2000.0f;

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const int maxAmostras = int (kMaxMs * 0.001 * sr) + maxBlockSize + 4;
        linha.assign (size_t (maxAmostras), 0.0f);
        escrita = 0;
        atrasoAmostras = 0;
    }

    /** Milissegundos de atraso. Fora da faixa e preso nos limites: pedido
        absurdo vira o teto, nao um indice invalido. */
    void defineMs (float ms) noexcept
    {
        const float limpo = std::max (0.0f, std::min (ms, kMaxMs));
        const int n = int (limpo * 0.001f * float (sr) + 0.5f);
        atrasoAmostras = std::max (0, std::min (n, int (linha.size()) - 1));
    }

    float ms() const noexcept
    {
        return sr > 0.0 ? float (atrasoAmostras) * 1000.0f / float (sr) : 0.0f;
    }

    int amostras() const noexcept { return atrasoAmostras; }

    /** Atrasa o bloco no proprio lugar. Com atraso zero nao toca em nada -
        nem gasta: canal sem atraso e o caso comum e nao pode pagar por um
        recurso que nao usa. */
    void processa (float* x, int n) noexcept
    {
        if (atrasoAmostras <= 0 || linha.empty()) return;

        const int cap = int (linha.size());
        for (int i = 0; i < n; ++i)
        {
            linha[size_t (escrita)] = x[i];

            int leitura = escrita - atrasoAmostras;
            if (leitura < 0) leitura += cap;
            x[i] = linha[size_t (leitura)];

            if (++escrita >= cap) escrita = 0;
        }
    }

    /** Zera o conteudo sem mexer no ajuste. Usado ao trocar a fonte do fader:
        senao o rabo da fonte anterior sai depois da troca. */
    void limpa() noexcept
    {
        std::fill (linha.begin(), linha.end(), 0.0f);
    }

private:
    std::vector<float> linha;
    double sr = 48000.0;
    int escrita = 0;
    int atrasoAmostras = 0;
};

} // namespace mesa
