#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mesa {

/** Reamostrador de razao variavel, por interpolacao cubica (Catmull-Rom).

    Nao e um conversor de taxa qualquer: a razao muda ao longo do tempo, em
    passos minusculos, para acompanhar um relogio que corre diferente do nosso.
    Por isso a fase e mantida em double e nunca reiniciada — reiniciar produz
    um salto audivel a cada bloco.

    Interpolacao cubica em vez de linear porque a linear atenua agudos de forma
    perceptivel quando a razao fica perto de 1.0, que e exatamente o nosso caso. */
class VariableResampler
{
public:
    void prepare()
    {
        h[0] = h[1] = h[2] = h[3] = 0.0f;
        phase = 0.0;
        primed = false;
    }

    /** Consome de src o que precisar e escreve n amostras em dst.
        ratio = quantas amostras de ENTRADA por amostra de SAIDA.
        Devolve quantas amostras de entrada foram consumidas. */
    template <typename PullFn>
    int process (float* dst, int n, double ratio, PullFn pullOne) noexcept
    {
        int consumed = 0;

        if (! primed)
        {
            for (int i = 0; i < 4; ++i) { h[i] = pullOne(); ++consumed; }
            primed = true;
        }

        for (int i = 0; i < n; ++i)
        {
            while (phase >= 1.0)
            {
                h[0] = h[1]; h[1] = h[2]; h[2] = h[3];
                h[3] = pullOne();
                ++consumed;
                phase -= 1.0;
            }
            dst[i] = catmullRom (float (phase));
            phase += ratio;
        }
        return consumed;
    }

private:
    /** Catmull-Rom entre h[1] e h[2], com h[0] e h[3] de contexto. */
    float catmullRom (float t) const noexcept
    {
        const float a = h[1];
        const float b = 0.5f * (h[2] - h[0]);
        const float c = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
        const float d = 0.5f * (h[3] - h[0]) + 1.5f * (h[1] - h[2]);
        return ((d * t + c) * t + b) * t + a;
    }

    float  h[4] { 0.0f, 0.0f, 0.0f, 0.0f };
    double phase = 0.0;
    bool   primed = false;
};

/** Controlador que decide a razao de reamostragem olhando so o nivel da fila.

    A ideia: nao sabemos e nao precisamos saber a taxa exata da outra placa.
    Se a fila enche, a fonte esta mais rapida que nos e precisamos consumir um
    pouco mais depressa; se esvazia, o contrario. Um PI lento resolve.

    "Lento" e requisito, nao preguica. Correcao rapida vira modulacao de altura
    audivel. O limite de +-kMaxPpm mantem o desvio bem abaixo do que o ouvido
    percebe, e o integrador leva minutos para acumular — que e a escala de tempo
    real da deriva de cristal. */
class DriftController
{
public:
    static constexpr double kMaxPpm = 400.0;   // teto de correcao

    void prepare (double targetFillRatio = 0.5)
    {
        target = targetFillRatio;
        integral = 0.0;
        ratio = 1.0;
        settled = false;
    }

    /** Chamado uma vez por bloco, com o quanto a fila esta cheia (0..1). */
    double update (double fillRatio, double blocksPerSecond) noexcept
    {
        const double err = fillRatio - target;   // >0 = fila enchendo

        // ganhos escolhidos para constante de tempo na casa de dezenas de
        // segundos: rapido o bastante para nao estourar, lento o bastante
        // para nao ser ouvido
        const double kp = 0.00035;
        const double ki = 0.000012 / std::max (1.0, blocksPerSecond);

        integral += err * ki;
        integral = std::clamp (integral, -kMaxPpm * 1e-6, kMaxPpm * 1e-6);

        double correction = err * kp + integral;
        correction = std::clamp (correction, -kMaxPpm * 1e-6, kMaxPpm * 1e-6);

        ratio = 1.0 + correction;
        if (std::abs (err) < 0.02) settled = true;
        return ratio;
    }

    double currentRatio() const noexcept { return ratio; }
    double ppm() const noexcept { return (ratio - 1.0) * 1e6; }
    bool   isSettled() const noexcept { return settled; }

private:
    double target = 0.5, integral = 0.0, ratio = 1.0;
    bool   settled = false;
};

} // namespace mesa
