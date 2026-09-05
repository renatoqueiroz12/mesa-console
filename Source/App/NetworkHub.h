#pragma once
#include "AudioEngine.h"
#include "NdiEngine.h"
#include "SecondaryDevices.h"
#include "LivewireReceiver.h"
#include "LivewireSender.h"
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

            if (src.livewireChannel > 0)
            {
                q = livewireQueue (src.livewireChannel, src.livewireSide, sampleRate, blockSize);
            }
            else if (! src.streamName.empty())
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

        // Livewire pode ser desligado com seguranca: e codigo nosso, sem DLL
        // de terceiro no caminho. Ainda assim paramos a thread antes de soltar.
        for (auto it = livewire.begin(); it != livewire.end(); )
        {
            const std::string k0 = "lw:" + std::to_string (it->first) + ":0";
            const std::string k1 = "lw:" + std::to_string (it->first) + ":1";
            const std::string k2 = "lw:" + std::to_string (it->first) + ":2";
            if (stillUsed.find (k0) == stillUsed.end()
                && stillUsed.find (k1) == stillUsed.end()
                && stillUsed.find (k2) == stillUsed.end())
            { it->second.receiver->stop(); it = livewire.erase (it); }
            else ++it;
        }

        // Receptores NDI que sairam do catalogo NAO sao destruidos agora.
        //
        // Destruir objeto do NDI durante a operacao — que era o que acontecia
        // ao fechar as configuracoes — derrubou a mesa varias vezes. Eles
        // ficam parados, sem slot apontando para eles, gastando quase nada, e
        // sao desmontados de uma vez no encerramento, quando ninguem mais usa
        // a biblioteca.
        for (auto& kv : ndi)
            if (stillUsed.find ("ndi:" + kv.first) == stillUsed.end() && ! kv.second.ocioso)
            {
                kv.second.ocioso = true;   // so marca; o objeto continua vivo
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
            // transmissao Livewire: a mesa vira fonte na rede Axia
            if (o.livewireChannel > 0)
            {
                auto it = lwOut.find (o.livewireChannel);
                if (it == lwOut.end())
                {
                    LwSaida saida;
                    saida.left  = std::make_unique<mesa::AsyncSource>();
                    saida.right = std::make_unique<mesa::AsyncSource>();
                    // fila rasa: latencia aqui e latencia no ar da outra mesa
                    // O pacote Livewire leva 240 quadros. A fila tem que
                    // comportar isso INDEPENDENTE do buffer da placa: com a
                    // UMC em 8 amostras, dimensionar pelo bloco escreveria
                    // fora do buffer.
                    const int blocoLw = juce::jmax (blockSize, 256);
                    saida.left ->prepare (blocoLw, 8, sampleRate, 0.35);
                    saida.right->prepare (blocoLw, 8, sampleRate, 0.35);
                    saida.sender = std::make_unique<LivewireSender> (*saida.left, *saida.right);
                    if (! saida.sender->start (o.livewireChannel, sampleRate))
                    {
                        lastLivewireError = saida.sender->error();
                        continue;
                    }
                    it = lwOut.emplace (o.livewireChannel, std::move (saida)).first;
                }
                engine.setNetSink (slot, it->second.left.get(), it->second.right.get(),
                                   o.busSource);
                ++slot;
                continue;
            }

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

    /** Para tudo que abrimos: receptores, transmissores e filas.

        Ordem importa no encerramento — thread viva com a biblioteca ja
        descarregada derruba o processo. Fechar aqui, cedo e explicitamente,
        tira essa corrida do caminho. */
    void shutdown()
    {
        for (auto& kv : ndi)      if (kv.second.receiver) kv.second.receiver->stop();
        for (auto& kv : livewire) if (kv.second.receiver) kv.second.receiver->stop();
        for (auto& kv : lwOut)    if (kv.second.sender)   kv.second.sender->stop();
        ndi.clear();
        livewire.clear();
        lwOut.clear();
        engine.clearNetSlots();
        engine.clearNetSinks();
    }

    /** Estado para a barra de status: placas perdidas e streams sem sinal. */
    std::vector<juce::String> problems() const
    {
        auto v = secondaries.lostDevices();
        if (lastLivewireError.isNotEmpty()) v.push_back ("Livewire: " + lastLivewireError);
        for (auto& kv : livewire)
            if (kv.second.receiver != nullptr && kv.second.receiver->packets() == 0)
                v.push_back ("Livewire canal " + juce::String (kv.first)
                             + ": nenhum pacote recebido (IGMP no switch? VLAN certa?)");
        for (auto& kv : lwOut)
            if (kv.second.sender->error().isNotEmpty())
                v.push_back ("Livewire saida " + juce::String (kv.first) + ": "
                             + kv.second.sender->error());
        // So acusa perda de verdade: sem quadro por mais de 3 s, e ja passada a
        // carencia de abertura. Antes bastava o contador estar zerado, o que
        // acusava perda ate com a fonte na propria maquina.
        for (auto& kv : ndi)
            if (kv.second.receiver != nullptr && ! kv.second.ocioso
                && ! kv.second.receiver->emCarencia()
                && kv.second.receiver->segundosSemAudio() > 3.0)
                v.push_back ("NDI sem audio ha "
                             + juce::String (kv.second.receiver->segundosSemAudio(), 0)
                             + "s: " + juce::String (kv.first));
        return v;
    }

private:
    struct LwSaida
    {
        std::unique_ptr<mesa::AsyncSource> left, right;
        std::unique_ptr<LivewireSender> sender;
    };
    std::map<int, LwSaida> lwOut;

    struct LwSlot
    {
        std::unique_ptr<mesa::AsyncSource> left, right, soma;
        std::unique_ptr<LivewireReceiver> receiver;
    };
    std::map<int, LwSlot> livewire;
    juce::String lastLivewireError;

    struct NdiSlot
    {
        std::unique_ptr<mesa::AsyncSource> queue;
        std::unique_ptr<NdiReceiver> receiver;
        bool ocioso = false;          // fora do catalogo, mas nao destruido
    };

    static std::string keyOf (const mesa::SourceDef& s)
    {
        if (s.livewireChannel > 0)
            return "lw:" + std::to_string (s.livewireChannel) + ":"
                 + std::to_string (s.livewireSide);
        if (! s.streamName.empty()) return "ndi:" + s.streamName;
        if (! s.deviceName.empty()) return "dev:" + s.deviceName + ":"
                                         + std::to_string (s.deviceChannel);
        return {};
    }

    /** Um receptor por CANAL; os dois lados do estereo saem dele. */
    mesa::AsyncSource* livewireQueue (int channel, int side, double sr, int block)
    {
        auto it = livewire.find (channel);
        if (it == livewire.end())
        {
            LwSlot slot;
            slot.left  = std::make_unique<mesa::AsyncSource>();
            slot.right = std::make_unique<mesa::AsyncSource>();
            slot.soma  = std::make_unique<mesa::AsyncSource>();
            // fila rasa: o Livewire chega em cadencia regular, e latencia
            // acumulada aqui e latencia no ar
            // idem na recepcao: o pacote traz ate 240 quadros de uma vez
            const int blocoLw = juce::jmax (block, 256);
            slot.left ->prepare (blocoLw, 8, sr, 0.35);
            slot.right->prepare (blocoLw, 8, sr, 0.35);
            slot.soma ->prepare (blocoLw, 8, sr, 0.35);
            slot.left ->name = "LW " + std::to_string (channel) + " L";
            slot.right->name = "LW " + std::to_string (channel) + " R";
            slot.left ->kind = mesa::AsyncSource::Kind::Ndi;
            slot.right->kind = mesa::AsyncSource::Kind::Ndi;

            slot.receiver = std::make_unique<LivewireReceiver> (*slot.left, *slot.right,
                                                                slot.soma.get());
            if (! slot.receiver->start (channel))
            {
                lastLivewireError = slot.receiver->error();
                return nullptr;
            }
            it = livewire.emplace (channel, std::move (slot)).first;
        }
        // 0 = esquerdo, 1 = direito, 2 = soma dos dois
        if (side == 1) return it->second.right.get();
        if (side == 2) return it->second.soma.get();
        return it->second.left.get();
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
