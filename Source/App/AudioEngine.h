#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Core/MixerEngine.h"
#include "../Core/AsyncSource.h"
#include <array>
#include "../Core/AutomationEngine.h"

/** Ponte entre o dispositivo ASIO e o MixerEngine.
    Nada aqui aloca, trava ou faz I/O dentro do callback de audio. */
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine (int numChannels) : desiredChannels (numChannels) {}

    ~AudioEngine() override { shutdown(); }

    /** Tenta abrir o driver ASIO. Retorna string vazia em caso de sucesso. */
    /** estadoSalvo e o XML devolvido por estadoAtual() numa sessao anterior.

        Antes o arranque chamava initialiseWithDefaultDevices, que IGNORA
        qualquer escolha anterior e abre o dispositivo padrao do Windows. Era
        por isso que so a placa nao persistia: todo o resto ia para o JSON, mas
        o dispositivo era redecidido do zero a cada abertura. */
    /** Nome da placa que a configuracao pedia e nao existe aqui. Vazio = tudo
        certo. A superficie mostra isso, porque silencio inexplicado e pior
        que aviso. */
    juce::String placaSalvaAusente;

    /** A placa nomeada no estado salvo esta disponivel nesta maquina? */
    bool placaSalvaExiste (const juce::XmlElement& xml)
    {
        const auto nome = xml.getStringAttribute ("audioDeviceName");
        if (nome.isEmpty()) return true;          // sem nome, deixa tentar

        for (auto* tipo : deviceManager.getAvailableDeviceTypes())
        {
            tipo->scanForDevices();
            if (tipo->getDeviceNames (false).contains (nome)) return true;
            if (tipo->getDeviceNames (true) .contains (nome)) return true;
        }
        return false;
    }

    juce::String start (int inputs, int outputs, const juce::String& estadoSalvo = {})
    {
        deviceManager.addAudioCallback (this);

        if (estadoSalvo.isNotEmpty())
        {
            if (auto xml = juce::XmlDocument::parse (estadoSalvo))
            {
                // A placa salva EXISTE nesta maquina?
                //
                // Confiar no erro devolvido nao basta: o driver de uma placa
                // ausente pode quebrar dentro da abertura, antes de devolver
                // qualquer coisa — e a mesa morre no arranque sem chegar a
                // escrever no log. Foi o que aconteceu ao trazer a
                // configuracao de outra maquina, que nomeava uma placa que
                // nao esta aqui.
                //
                // Perguntar antes custa uma varredura de nomes e transforma
                // "nao abre" em "abriu na placa padrao".
                if (placaSalvaExiste (*xml))
                {
                    const auto err = deviceManager.initialise (inputs, outputs, xml.get(), true);
                    if (err.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr)
                        return {};
                }
                else
                {
                    placaSalvaAusente = xml->getStringAttribute ("audioDeviceName");
                }
            }
        }

        // NAO ABRIMOS ASIO NO ESCURO.
        //
        // Antes a mesa forcava o tipo ASIO e abria o primeiro driver da lista.
        // Parece sensato — ASIO e o caminho de menor latencia, que e o que uma
        // mesa quer. Mas driver ASIO de placa AUSENTE quebra ao ser aberto, e
        // quebra dentro do proprio driver, onde nenhum tratamento nosso
        // alcanca: a mesa morre no arranque sem chegar a escrever no log.
        // Basta alguem ter instalado uma interface e levado embora.
        //
        // Sem escolha salva, abrimos o audio do Windows, que sempre abre. O
        // operador escolhe ASIO uma vez nas configuracoes e a escolha fica
        // salva — a partir dai o caminho de baixa latencia e usado sempre, so
        // que por decisao de alguem, nao por sorte da ordem da lista.
        auto err = deviceManager.initialiseWithDefaultDevices (inputs, outputs);
        if (err.isNotEmpty())
            return err;

        return {};
    }

    /** Estado do dispositivo em XML, para gravar junto das configuracoes. */
    juce::String estadoAtual() const
    {
        if (auto xml = deviceManager.createStateXml())
            return xml->toString();
        return {};
    }

    void shutdown()
    {
        deviceManager.removeAudioCallback (this);
        deviceManager.closeAudioDevice();
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const double sr    = device->getCurrentSampleRate();
        const int    block = device->getCurrentBufferSizeSamples();

        mixer.prepare (sr, block, desiredChannels);   // fora do callback: pode alocar
        automation.prepare (desiredChannels);
        blockMs.store (float (block) / float (sr) * 1000.0f);

        sampleRate.store (sr);
        blockSize .store (block);
        latencyMs .store (float ((device->getInputLatencyInSamples()
                                + device->getOutputLatencyInSamples()) / sr * 1000.0));
        deviceName = device->getName();
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();

        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

        // fontes assincronas (NDI, placas secundarias). pull() nunca bloqueia:
        // se a fonte sumiu, devolve silencio e o canal acusa ausencia de sinal.
        int numNet = 0;
        for (int i = 0; i < kMaxNetSlots; ++i)
        {
            auto* q = netSlot[size_t (i)].load (std::memory_order_relaxed);
            netPtr[size_t (i)] = (q != nullptr) ? q->pull (numSamples) : nullptr;
            if (q != nullptr) numNet = i + 1;
        }

        mixer.process (inputChannelData, numInputChannels,
                       outputChannelData, numOutputChannels, numSamples,
                       numNet > 0 ? netPtr.data() : nullptr, numNet);

        // gravador de diagnostico: entrada crua, antes de qualquer coisa nossa
        {
            const int cg = canalGravado.load (std::memory_order_relaxed);
            const int pt = pontoGravacao.load (std::memory_order_relaxed);

            if (aoGravar)
            {
                if (pt == 2)                       // programa, independe de canal
                    aoGravar (mixer.busLeft (0), numSamples);
                else if (cg >= 0 && cg < mixer.numChannels())
                    aoGravar (pt == 1 ? mixer.channel (cg).processedData()
                                      : mixer.channel (cg).entradaBruta(), numSamples);
            }
        }

        // transcricao em tempo real: entrada crua, o texto do que foi dito nao
        // deve depender de fader nem de DSP
        {
            const int ct = canalTranscricao.load (std::memory_order_relaxed);
            if (ct >= 0 && ct < mixer.numChannels() && aoTranscrever)
                aoTranscrever (mixer.channel (ct).entradaBruta(), numSamples);
        }

        // saidas assincronas: copia o barramento escolhido para a fila do
        // destino. Escrita em fila SPSC — nao bloqueia, nao aloca.
        for (int i = 0; i < kMaxNetSinks; ++i)
        {
            auto& sk = netSink[size_t (i)];
            auto* l = sk.left .load (std::memory_order_relaxed);
            if (l == nullptr) continue;
            auto* r = sk.right.load (std::memory_order_relaxed);

            const int b = sk.busSource.load (std::memory_order_relaxed);
            const float* srcL = nullptr; const float* srcR = nullptr;
            switch (b)
            {
                case 0: case 1: case 2: case 3:
                    srcL = mixer.busLeft (b);      srcR = mixer.busRight (b);      break;
                case 4: srcL = mixer.cueLeft();    srcR = mixer.cueRight();        break;
                case 5: srcL = mixer.monitorLeft(); srcR = mixer.monitorRight();   break;
                case 6: srcL = mixer.phonesLeft(); srcR = mixer.phonesRight();     break;
                case 7: srcL = mixer.studioLeft(); srcR = mixer.studioRight();     break;
                default: break;
            }
            if (srcL != nullptr) l->push (srcL, numSamples);
            if (r != nullptr && srcR != nullptr) r->push (srcR, numSamples);
        }

        // decide cortes e enfileira comandos. Nao abre socket, nao aloca, nao trava.
        automation.processBlock (mixer, blockMs.load (std::memory_order_relaxed));

        // PICO DO QUE SAI DE FATO PARA A PLACA.
        //
        // Medido depois de tudo — mix, pan, ganho de barramento — no proprio
        // buffer que o driver recebe. E o unico numero que separa "a mesa
        // entrega baixo" de "a mesa entrega certo e o driver atenua". Sem ele,
        // ajustar nivel vira adivinhacao.
        {
            // POR CANAL, e nao o maior de todos.
            //
            // O pico geral nao serve para achar roteamento errado: se o som
            // sai cheio na saida 3 e a caixa esta na 1, ele marca cheio do
            // mesmo jeito e nada parece errado. Por canal, o silencio aparece
            // exatamente onde esta.
            float geral = 0.0f;
            for (int ch = 0; ch < numOutputChannels && ch < kMaxCanaisMedidos; ++ch)
            {
                float pico = 0.0f;
                if (outputChannelData[ch] != nullptr)
                    for (int i = 0; i < numSamples; ++i)
                        pico = juce::jmax (pico, std::abs (outputChannelData[ch][i]));

                const float anterior = picoCanal[size_t (ch)].load (std::memory_order_relaxed);
                picoCanal[size_t (ch)].store (juce::jmax (pico, anterior * 0.9995f),
                                              std::memory_order_relaxed);
                geral = juce::jmax (geral, pico);
            }

            canaisSaida.store (numOutputChannels, std::memory_order_relaxed);
            const float anterior = picoSaida.load (std::memory_order_relaxed);
            picoSaida.store (juce::jmax (geral, anterior * 0.9995f),
                             std::memory_order_relaxed);
        }

        const double elapsed = juce::Time::highResolutionTicksToSeconds (
                                   juce::Time::getHighResolutionTicks() - t0);
        const double budget  = numSamples / sampleRate.load();
        cpuLoad.store (float (elapsed / budget * 100.0));
    }

    void audioDeviceStopped() override { cpuLoad.store (0.0f); }

    void audioDeviceError (const juce::String& message) override
    {
        lastError = message;   // chamado fora do callback de audio
    }

    mesa::MixerEngine      mixer;
    mesa::AutomationEngine automation;
    juce::AudioDeviceManager deviceManager;

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int>    blockSize  { 0 };
    std::atomic<float>  latencyMs  { 0.0f };
    std::atomic<float>  cpuLoad    { 0.0f };
    /** Pico das amostras entregues a placa, em escala linear. */
    std::atomic<float>  picoSaida  { 0.0f };
    static constexpr int kMaxCanaisMedidos = 16;
    std::array<std::atomic<float>, kMaxCanaisMedidos> picoCanal {};
    std::atomic<int>    canaisSaida { 0 };

    /** Slots de fonte assincrona: NDI e placas secundarias. O canal aponta para
        um slot pelo inputIndex quando o inputKind e Network. Ponteiro atomico
        porque a UI troca a fonte com o audio rodando. */
    static constexpr int kMaxNetSlots = 32;
    std::array<std::atomic<mesa::AsyncSource*>, kMaxNetSlots> netSlot {};

    /** Canal cuja ENTRADA CRUA vai para o gravador. -1 desliga. */
    std::atomic<int> canalGravado { -1 };
    /** 0 = entrada crua, 1 = pos-fader, 2 = PGM 1. */
    std::atomic<int> pontoGravacao { 0 };
    std::function<void (const float*, int)> aoGravar;

    /** Canal cuja entrada vai para a transcricao continua. -1 desliga. */
    std::atomic<int> canalTranscricao { -1 };
    std::function<void (const float*, int)> aoTranscrever;

    void setNetSlot (int i, mesa::AsyncSource* q) noexcept
    {
        if (i >= 0 && i < kMaxNetSlots) netSlot[size_t (i)].store (q, std::memory_order_relaxed);
    }
    void clearNetSlots() noexcept
    {
        for (auto& sl : netSlot) sl.store (nullptr, std::memory_order_relaxed);
    }

    /** Destino assincrono: um par de filas alimentado pelos barramentos.
        busSource segue a mesma numeracao do OutputDef. */
    struct NetSink
    {
        std::atomic<mesa::AsyncSource*> left  { nullptr };
        std::atomic<mesa::AsyncSource*> right { nullptr };
        std::atomic<int> busSource { 0 };
    };

    static constexpr int kMaxNetSinks = 16;
    std::array<NetSink, kMaxNetSinks> netSink {};

    void setNetSink (int i, mesa::AsyncSource* l, mesa::AsyncSource* r, int bus) noexcept
    {
        if (i < 0 || i >= kMaxNetSinks) return;
        netSink[size_t (i)].busSource.store (bus, std::memory_order_relaxed);
        netSink[size_t (i)].left .store (l, std::memory_order_relaxed);
        netSink[size_t (i)].right.store (r, std::memory_order_relaxed);
    }
    void clearNetSinks() noexcept
    {
        for (auto& s : netSink)
        { s.left.store (nullptr); s.right.store (nullptr); }
    }
    std::atomic<float>  blockMs    { 2.67f };
    juce::String deviceName, lastError;

private:
    std::array<const float*, kMaxNetSlots> netPtr {};   // sem alocar no callback

    int desiredChannels;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
