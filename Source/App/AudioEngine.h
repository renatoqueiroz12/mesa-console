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
    juce::String start (int inputs, int outputs)
    {
        deviceManager.addAudioCallback (this);

        for (auto* type : deviceManager.getAvailableDeviceTypes())
            if (type->getTypeName() == "ASIO")
                deviceManager.setCurrentAudioDeviceType ("ASIO", true);

        auto err = deviceManager.initialiseWithDefaultDevices (inputs, outputs);
        if (err.isNotEmpty())
            return err;

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

    /** Slots de fonte assincrona: NDI e placas secundarias. O canal aponta para
        um slot pelo inputIndex quando o inputKind e Network. Ponteiro atomico
        porque a UI troca a fonte com o audio rodando. */
    static constexpr int kMaxNetSlots = 32;
    std::array<std::atomic<mesa::AsyncSource*>, kMaxNetSlots> netSlot {};

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
