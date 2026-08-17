#pragma once
#include "AudioEngine.h"
#include "NdiEngine.h"
#include "SecondaryDevices.h"
#include "../Core/SourceCatalog.h"
#include <map>
#include <memory>

/** Liga cada fonte assincrona do catalogo a um slot do motor.

    Um "slot" e so um numero: o canal guarda esse numero em inputIndex quando
    o inputKind e Network, e o callback puxa dali. Quem resolve o que ha do
    outro lado — um receptor NDI ou uma entrada de placa secundaria — e este
    hub, sempre na thread da interface.

    Rebind e idempotente: chamar de novo depois de mexer no catalogo reaproveita
    o que ja esta aberto e fecha so o que ninguem usa mais. Isso importa porque
    reabrir um receptor NDI a cada salvamento produziria um corte no ar. */
class NetworkHub
{
public:
    NetworkHub (AudioEngine& e, SecondaryDevices& sec) : engine (e), secondaries (sec) {}

    /** Percorre o catalogo, garante que cada fonte de rede tenha slot e fila,
        e escreve o slot de volta no SourceDef. */
    void rebind (mesa::SourceCatalog& catalog, double sampleRate, int blockSize)
    {
        std::map<std::string, int> stillUsed;
        int slot = 0;

        for (auto& src : catalog.sources)
        {
            if (src.kind != int (mesa::InputKind::Network)) continue;
            if (slot >= AudioEngine::kMaxNetSlots) break;

            const std::string key = keyOf (src);
            if (key.empty()) { src.index = -1; continue; }

            mesa::AsyncSource* q = nullptr;

            if (! src.streamName.empty())
            {
                q = ndiQueue (src.streamName, sampleRate, blockSize);
            }
            else if (! src.deviceName.empty())
            {
                auto* dev = secondaries.open (juce::String (src.deviceType.empty()
                                                  ? std::string ("Windows Audio") : src.deviceType),
                                              juce::String (src.deviceName),
                                              sampleRate, blockSize);
                q = (dev != nullptr) ? dev->source (src.deviceChannel) : nullptr;
            }

            if (q == nullptr) { src.index = -1; continue; }

            engine.setNetSlot (slot, q);
            src.index = slot;
            stillUsed[key] = slot;
            ++slot;
        }

        for (int i = slot; i < AudioEngine::kMaxNetSlots; ++i)
            engine.setNetSlot (i, nullptr);

        // fecha receptores NDI que sairam do catalogo
        for (auto it = ndi.begin(); it != ndi.end(); )
        {
            if (stillUsed.find ("ndi:" + it->first) == stillUsed.end())
            {
                it->second.receiver->stop();
                it = ndi.erase (it);
            }
            else ++it;
        }
    }

    /** Liga cada OutputDef de rede a um destino do motor. Mesma logica dos
        inputs, do outro lado: par de filas por destino. */
    void rebindOutputs (mesa::OutputCatalog& outputs, double sampleRate, int blockSize)
    {
        int slot = 0;
        for (auto& o : outputs.outputs)
        {
            if (o.kind != int (mesa::InputKind::Network)) continue;
            if (slot >= AudioEngine::kMaxNetSinks) break;
            if (o.deviceName.empty()) continue;   // envio por NDI ainda nao ligado

            auto* dev = secondaries.open (juce::String (o.deviceType.empty()
                                              ? std::string ("Windows Audio") : o.deviceType),
                                          juce::String (o.deviceName), sampleRate, blockSize);
            if (dev == nullptr) continue;

            const int base = o.pair * 2;
            engine.setNetSink (slot, dev->sink (base), dev->sink (base + 1), o.busSource);
            ++slot;
        }
        for (int i = slot; i < AudioEngine::kMaxNetSinks; ++i)
            engine.setNetSink (i, nullptr, nullptr, 0);
    }

    /** Estado para a barra de status: placas perdidas e streams sem sinal. */
    std::vector<juce::String> problems() const
    {
        auto v = secondaries.lostDevices();
        for (auto& kv : ndi)
            if (! kv.second.queue->isConnected())
                v.push_back ("NDI sem sinal: " + juce::String (kv.first));
        return v;
    }

private:
    struct NdiSlot
    {
        std::unique_ptr<mesa::AsyncSource> queue;
        std::unique_ptr<NdiReceiver> receiver;
    };

    static std::string keyOf (const mesa::SourceDef& s)
    {
        if (! s.streamName.empty()) return "ndi:" + s.streamName;
        if (! s.deviceName.empty()) return "dev:" + s.deviceName + ":"
                                         + std::to_string (s.deviceChannel);
        return {};
    }

    mesa::AsyncSource* ndiQueue (const std::string& stream, double sr, int block)
    {
        auto it = ndi.find (stream);
        if (it != ndi.end()) return it->second.queue.get();

        NdiSlot s;
        s.queue = std::make_unique<mesa::AsyncSource>();
        s.queue->prepare (block, 8, sr);
        s.queue->name = stream;
        s.queue->kind = mesa::AsyncSource::Kind::Ndi;

        s.receiver = std::make_unique<NdiReceiver> (*s.queue, sr, block);
        if (! s.receiver->start (stream)) return nullptr;

        auto* q = s.queue.get();
        ndi.emplace (stream, std::move (s));
        return q;
    }

    AudioEngine& engine;
    SecondaryDevices& secondaries;
    std::map<std::string, NdiSlot> ndi;
};
