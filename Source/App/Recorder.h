#pragma once
#include "../Core/NomeDaThread.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/** Gravador de diagnostico.

    Grava o que chega a um canal EXATAMENTE como chega: taxa da placa, sem
    filtro, sem conversao, sem normalizacao. E de proposito — este gravador
    existe para responder "onde o audio se estraga?", e qualquer processamento
    nosso no caminho estragaria a prova.

    E diferente do SpeechWriter, que converte para 16 kHz, filtra e normaliza
    porque quem le aquilo e um reconhecedor de fala. Aqui quem le e o ouvido,
    e o ouvido precisa do original.

    O callback de audio so escreve na fila; a thread grava. Nada de disco
    dentro do callback. */
class Recorder
{
public:
    Recorder() = default;
    ~Recorder() { stop(); }

    /** Comeca a gravar. Devolve o arquivo criado, ou nulo em caso de falha. */
    juce::File start (const juce::File& pasta, double sampleRate, const juce::String& etiqueta,
                      int bits = 24)
    {
        stop();

        pasta.createDirectory();
        const auto agora = juce::Time::getCurrentTime();
        arquivo = pasta.getChildFile (agora.formatted ("%Y%m%d-%H%M%S") + "-"
                                      + etiqueta.retainCharacters (
                                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                            "abcdefghijklmnopqrstuvwxyz0123456789-_")
                                      + ".wav");
        sr = sampleRate;

        std::unique_ptr<juce::FileOutputStream> fluxo (arquivo.createOutputStream());
        if (fluxo == nullptr) return {};

        juce::WavAudioFormat wav;
        // 24 bits: o suficiente para nao acrescentar ruido proprio a prova
        const int b = (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
        escritor.reset (wav.createWriterFor (fluxo.get(), sampleRate, 1, b, {}, 0));
        if (escritor == nullptr) return {};
        fluxo.release();

        fila.assign (size_t (sampleRate * 4.0), 0.0f);   // 4 s de folga
        escrita.store (0);
        leitura.store (0);
        perdidas.store (0);
        gravadas.store (0);

        pausado.store (false);
        sair.store (false);
        worker = std::thread ([this] { loop(); });
        gravando.store (true);
        return arquivo;
    }

    void stop()
    {
        if (! gravando.exchange (false)) { sair.store (true); if (worker.joinable()) worker.join(); return; }
        sair.store (true);
        if (worker.joinable()) worker.join();

        std::lock_guard<std::mutex> g (mutex);
        escritor.reset();      // fecha o arquivo e grava o cabecalho
    }

    /** Chamado do callback de audio. Nao aloca, nao bloqueia. */
    void alimenta (const float* x, int n) noexcept
    {
        if (! gravando.load (std::memory_order_relaxed)
            || pausado.load (std::memory_order_relaxed) || fila.empty()) return;

        const int cap = int (fila.size());
        const int w = escrita.load (std::memory_order_relaxed);
        const int r = leitura.load (std::memory_order_acquire);
        const int livre = cap - (w - r) - 1;

        if (livre < n) { perdidas.fetch_add (n, std::memory_order_relaxed); return; }

        for (int i = 0; i < n; ++i) fila[size_t ((w + i) % cap)] = x[i];
        escrita.store (w + n, std::memory_order_release);
    }

    bool estaGravando() const noexcept { return gravando.load(); }

    /** Pausa mantem o arquivo aberto e para de aceitar audio: retomar continua
        no mesmo arquivo, sem emenda. Parar fecha e o proximo comeca outro. */
    void pausa (bool p) noexcept { pausado.store (p); }
    bool estaPausado() const noexcept { return pausado.load(); }
    juce::File arquivoAtual() const { return arquivo; }
    double segundos() const noexcept { return sr > 0.0 ? double (gravadas.load()) / sr : 0.0; }
    int amostrasPerdidas() const noexcept { return perdidas.load(); }

private:
    void loop()
    {
        // se cair, o log da queda diz o nome em vez de "(sem nome)"
        mesa::batizaThread ("gravador");

        std::vector<float> bloco (4096, 0.0f);

        while (! sair.load())
        {
            const int cap = int (fila.size());
            const int w = escrita.load (std::memory_order_acquire);
            int r = leitura.load (std::memory_order_relaxed);
            int disponivel = w - r;

            if (disponivel <= 0)
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (20));
                continue;
            }

            const int quantas = juce::jmin (disponivel, int (bloco.size()));
            for (int i = 0; i < quantas; ++i) bloco[size_t (i)] = fila[size_t ((r + i) % cap)];
            leitura.store (r + quantas, std::memory_order_release);

            std::lock_guard<std::mutex> g (mutex);
            if (escritor != nullptr)
            {
                const float* canais[1] = { bloco.data() };
                escritor->writeFromFloatArrays (canais, 1, quantas);
                gravadas.fetch_add (quantas);
            }
        }
    }

    juce::File arquivo;
    double sr = 48000.0;
    std::unique_ptr<juce::AudioFormatWriter> escritor;
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> gravando { false }, sair { true }, pausado { false };
    std::vector<float> fila;
    std::atomic<int> escrita { 0 }, leitura { 0 }, perdidas { 0 }, gravadas { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
