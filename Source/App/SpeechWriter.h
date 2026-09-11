#pragma once
#include "../Core/NomeDaThread.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Core/SpeechSegmenter.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/** Grava os trechos de fala em WAV, numa pasta, para o transcritor consumir.

    Por que arquivo e processo separado, e nao a transcricao dentro da mesa:

    Ja levamos duas quedas por causa de biblioteca de terceiro rodando dentro
    do processo do audio. Reconhecimento de fala e pesado, usa modelo grande,
    e as implementacoes travam com mais frequencia que uma biblioteca de rede.
    Se ela morrer aqui dentro, a emissora sai do ar.

    Com pasta no meio: a mesa so escreve arquivo — operacao barata, feita em
    thread propria — e quem transcreve e outro programa. Se o transcritor
    travar, encher de trabalho ou for reiniciado, a mesa nem fica sabendo. O
    texto volta pela porta 8890, que ja existe e ja e testada.

    Converte para 16 kHz mono porque e o que os reconhecedores esperam; mandar
    48 kHz so gastaria disco e tempo de conversao do outro lado. */
class SpeechWriter
{
public:
    /** A macro de nao-copiavel do JUCE tambem some com o construtor padrao. */
    SpeechWriter() = default;
    ~SpeechWriter() { stop(); }

    void start (const juce::File& pasta, double sampleRateEntrada)
    {
        stop();
        destino = pasta;
        destino.createDirectory();
        srEntrada = sampleRateEntrada;
        sair.store (false);
        worker = std::thread ([this] { loop(); });
    }

    void stop()
    {
        sair.store (true);
        if (worker.joinable()) worker.join();
    }

    /** Chamado da thread da interface com o trecho recolhido do segmentador. */
    void enfileira (const std::vector<float>& amostras)
    {
        if (amostras.empty()) return;
        std::lock_guard<std::mutex> g (mutex);
        // teto de fila: se o transcritor nao acompanha, e melhor perder trecho
        // velho do que crescer sem limite ate a memoria acabar
        if (fila.size() >= 20) { fila.erase (fila.begin()); descartados.fetch_add (1); }
        fila.push_back (amostras);
    }

    int gravados() const noexcept { return escritos.load(); }
    int descartadosPorFila() const noexcept { return descartados.load(); }
    juce::File pasta() const { return destino; }

private:
    void loop()
    {
        // se cair, o log da queda diz o nome em vez de "(sem nome)"
        mesa::batizaThread ("fala-arquivo");

        while (! sair.load())
        {
            std::vector<float> trecho;
            {
                std::lock_guard<std::mutex> g (mutex);
                if (! fila.empty()) { trecho.swap (fila.front()); fila.erase (fila.begin()); }
            }
            if (trecho.empty())
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
                continue;
            }
            grava (trecho);
        }
    }

    void grava (const std::vector<float>& entrada)
    {
        const auto agora = juce::Time::getCurrentTime();
        const auto nome = agora.formatted ("%Y%m%d-%H%M%S") + "-"
                        + juce::String (escritos.load()).paddedLeft ('0', 4) + ".wav";
        auto arquivo = destino.getChildFile (nome);

        // 48000 -> 16000 com FILTRO DE VERDADE antes de decimar.
        //
        // A versao anterior fazia media de tres amostras. Isso e um filtro
        // fraquissimo: quase tudo que estava acima de 8 kHz voltava rebatido
        // para dentro da banda da voz. O resultado era um audio distorcido em
        // que o reconhecedor nao achava fala — e, nao achando, inventava. Os
        // textos com a mesma frase repetida cinco vezes vinham dai.
        //
        // Agora passa por um passa-baixa de quarta ordem em 7 kHz, aplicado
        // nos dois sentidos para nao torcer a fase, e so entao decima.
        std::vector<float> filtrado (entrada);
        passaBaixa (filtrado, srEntrada, 7000.0f);

        const int fator = juce::jmax (1, int (std::round (srEntrada / 16000.0)));
        std::vector<float> saida;
        saida.reserve (filtrado.size() / size_t (fator) + 1);
        for (size_t i = 0; i < filtrado.size(); i += size_t (fator))
            saida.push_back (filtrado[i]);

        // Normaliza para -3 dBFS.
        //
        // O reconhecedor trabalha muito melhor com sinal em nivel cheio, e o
        // que chega da mesa pode estar 20 ou 30 dB abaixo dependendo da fonte.
        // Normalizar por trecho e seguro aqui: este audio NAO vai ao ar, serve
        // so para transcrever.
        float pico = 0.0f;
        for (float v : saida) pico = juce::jmax (pico, std::abs (v));
        if (pico > 1.0e-5f)
        {
            const float ganho = juce::jmin (32.0f, 0.708f / pico);   // -3 dBFS
            for (auto& v : saida) v *= ganho;
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> fluxo (arquivo.createOutputStream());
        if (fluxo == nullptr) return;

        std::unique_ptr<juce::AudioFormatWriter> escritor (
            wav.createWriterFor (fluxo.get(), 16000.0, 1, 16, {}, 0));
        if (escritor == nullptr) return;
        fluxo.release();   // o escritor assume o fluxo

        const float* canais[1] = { saida.data() };
        escritor->writeFromFloatArrays (canais, 1, int (saida.size()));
        escritor.reset();

        escritos.fetch_add (1);
    }

    /** Passa-baixa de 4a ordem, aplicado ida e volta.

        Duas passagens de um biquad de 2a ordem, a segunda de tras para frente:
        o corte fica mais firme e a fase nao e torcida. Fase importa pouco para
        o reconhecedor, mas nao custa nada aqui e evita o "sotaque" metalico
        que filtro assimetrico deixa. */
    static void passaBaixa (std::vector<float>& x, double sr, float corte)
    {
        if (x.empty()) return;

        const double w = 2.0 * juce::MathConstants<double>::pi * double (corte) / sr;
        const double cosw = std::cos (w), sinw = std::sin (w);
        const double alpha = sinw / (2.0 * 0.7071);          // Q de Butterworth

        const double b0 = (1.0 - cosw) * 0.5, b1 = 1.0 - cosw, b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cosw, a2 = 1.0 - alpha;

        const float B0 = float (b0 / a0), B1 = float (b1 / a0), B2 = float (b2 / a0);
        const float A1 = float (a1 / a0), A2 = float (a2 / a0);

        auto passa = [&] (bool tras)
        {
            float z1 = 0.0f, z2 = 0.0f;
            const int n = int (x.size());
            for (int k = 0; k < n; ++k)
            {
                const int i = tras ? (n - 1 - k) : k;
                const float ent = x[size_t (i)];
                const float sai = B0 * ent + z1;
                z1 = B1 * ent - A1 * sai + z2;
                z2 = B2 * ent - A2 * sai;
                x[size_t (i)] = sai;
            }
        };
        passa (false);
        passa (true);
    }

    juce::File destino;
    double srEntrada = 48000.0;
    std::thread worker;
    std::atomic<bool> sair { true };
    std::atomic<int> escritos { 0 }, descartados { 0 };
    std::mutex mutex;
    std::vector<std::vector<float>> fila;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeechWriter)
};
