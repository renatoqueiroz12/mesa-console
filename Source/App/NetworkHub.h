#pragma once
#include "AudioEngine.h"
#include "NdiEngine.h"
#include "SecondaryDevices.h"
#include "LivewireReceiver.h"
#include "LivewireSender.h"
#include "LivewireAdvertiser.h"
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

    /** IP da placa por onde falar Livewire. Vazio = escolha do Windows. */
    void setPlacaLivewire (const juce::String& ip)
    {
        if (ip == placaLw) return;

        // A PLACA MUDOU: refaz os dois lados.
        //
        // Receptor e transmissor pedem a interface ao ABRIR. Trocar a escolha
        // depois nao alcanca quem ja esta de pe — o novo valor ficava guardado
        // e o socket seguia na placa antiga. O sintoma engana: a tela mostra a
        // placa certa e o audio continua indo para a errada.
        placaLw = ip;
        fechaLivewire();
        anunciante.stop();
        anuncioLigado = false;
    }

    /** Anuncia na rede os canais que a mesa transmite.

        Sem isto o QOR recebe o audio e marca "Used EW": ele quer alterar o
        anuncio para registrar que assumiu a fonte, nao acha anuncio nenhum, e
        nao sabe distinguir isso de outro motor ja ter pegado o stream. */
    void permiteAnuncio (bool v)
    {
        if (v == anuncioPermitido) return;
        anuncioPermitido = v;
        if (! v) { anunciante.stop(); anuncioLigado = false; }
    }

    /** HWID e porta de controle do anuncio. Mudar exige refazer os sockets —
        o anunciante pede a porta ao abrir, como o receptor pede a placa. */
    void setIdentidadeAnuncio (int hwid, int udpc)
    {
        if (hwid == hwidAnuncio && udpc == udpcAnuncio) return;
        hwidAnuncio = hwid;
        udpcAnuncio = udpc;
        anunciante.stop();
        anuncioLigado = false;
    }

    void atualizaAnuncio (const juce::String& nomeDaMaquina)
    {
        if (! anuncioPermitido)
        {
            anunciante.stop();
            anuncioLigado = false;
            return;
        }

        std::vector<LivewireAdvertiser::Fonte> lista;
        for (auto& kv : lwOut)
            if (kv.second.sender != nullptr)
            {
                const auto nome = nomesDeSaida.count (kv.first) ? nomesDeSaida[kv.first]
                                                                : juce::String ("MESA");
                lista.push_back ({ kv.first, nome.toStdString(), 0 });
            }

        if (lista.empty())
        {
            registra ("anuncio: nenhum canal para anunciar ("
                      + juce::String (int (lwOut.size())) + " saidas Livewire)");
            anunciante.stop();
            anuncioLigado = false;
            return;
        }

        anunciante.setFontes (lista);

        // Estado proprio, e nao o contador de enviados.
        //
        // Antes a condicao era "enviados == 0", que se contradiz: o contador
        // zera a cada start, entao ou ele tentava religar para sempre, ou —
        // dando certo uma vez — nunca mais religava ao trocar de placa. Um
        // sinalizador explicito diz o que se quer saber.
        if (placaLw.isEmpty())
            registra ("anuncio: sem placa de rede escolhida — nao da para anunciar");

        if (! anuncioLigado && placaLw.isNotEmpty())
        {
            anuncioLigado = anunciante.start (placaLw, nomeDaMaquina,
                                              hwidAnuncio, udpcAnuncio);
            registra (anuncioLigado
                          ? "anunciando " + juce::String (int (lista.size()))
                                + " canal(is) na rede pela placa " + placaLw
                          : "NAO consegui anunciar na rede pela placa " + placaLw);
        }
    }

    int anunciosEnviados()  const noexcept { return anunciante.enviados(); }
    /** Quanto o console ja respondeu ao nosso anuncio. Zero e diagnostico. */
    int anunciosRecebidos() const noexcept { return anunciante.recebidos(); }
    unsigned hwidDoAnuncio() const noexcept { return anunciante.hwid(); }

    /** Tipo de carga RTP usado ao transmitir. */
    void setTipoDeCarga (int v)
    {
        cargaTx = v;
        for (auto& kv : lwOut)
            if (kv.second.sender != nullptr)
                kv.second.sender->tipoDeCarga.store (v);
    }

    /** Para a superficie registrar no log o que a rede esta fazendo.

        Sem isto, "nao entra audio" nao tem investigacao: nao da para saber se
        o receptor nem foi criado, se foi criado e nada chega, ou se chega e
        para em outro lugar. Cada um desses tem conserto diferente. */
    std::function<void (const juce::String&)> aoRegistrar;

    /** Estado das fontes NDI, para o batimento.

        Mesma razao do Livewire e das secundarias: "o NDI esta ruim" nao diz se
        o emissor entrega mal, se a fila seca ou se o relogio nao casa. Buraco
        e fila vazia tem conserto diferente. */
    juce::String estadoNdi()
    {
        juce::String t;
        for (auto& kv : ndi)
        {
            if (kv.second.queue == nullptr) continue;
            t << "  |  NDI " << juce::String (kv.first).substring (0, 18)
              << (kv.second.ocioso ? " (ocioso)" : "")
              << " " << juce::String (int (kv.second.queue->amostrasPorSegundo())) << "/s"
              << " buracos " << juce::String (kv.second.queue->amostrasEmFalta())
              << " puxadas-ruins " << juce::String (kv.second.queue->badPulls())
              << " ppm " << juce::String (kv.second.queue->correctionPpm(), 0)
              << (kv.second.queue->driftSettled() ? "" : " NAO-ASSENTOU");
        }
        return t;
    }

    /** Estado dos receptores Livewire, para o batimento. */
    juce::String estadoLivewire() const
    {
        juce::String t;
        for (const auto& kv : livewire)
            t << "  |  LW " << kv.first << " " << juce::String (kv.second.receiver->packets())
              << " pacotes carga " << juce::String (kv.second.receiver->cargaRecebida());
        for (const auto& kv : lwOut)
            t << "  |  LW saida " << kv.first << " "
              << juce::String (kv.second.sender->packets()) << " pacotes";
        return t;
    }

    /** Percorre o catalogo, garante que cada fonte de rede tenha slot e fila,
        e escreve o slot de volta no SourceDef. */
    void rebind (mesa::SourceCatalog& catalog, double sampleRate, int blockSize)
    {
        std::map<std::string, int> stillUsed;
        int slot = 0;

        for (auto& src : catalog.sources)
        {
            // Livewire e NDI valem por si.
            //
            // Antes esta linha exigia que a fonte estivesse MARCADA como de
            // rede. Um input criado como Windows e depois apontado para um
            // canal Livewire mantinha a marca antiga e nunca chegava aqui: a
            // lista mostrava a fonte escolhida, o endereco aparecia certo na
            // tela, e o receptor simplesmente nao existia. Quem manda e o
            // campo preenchido, nao a marca.
            const bool ehDeRede = src.kind == int (mesa::InputKind::Network)
                               || src.livewireChannel > 0
                               || ! src.streamName.empty();
            if (! ehDeRede) continue;
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
            // Quem manda e o campo preenchido, nao a marca.
            //
            // Mesmo defeito que os inputs tinham: um output criado como placa
            // e depois apontado para um canal Livewire mantinha a marca antiga
            // e era descartado aqui. O transmissor nunca nascia — e sem
            // transmissor, o anunciante nao tem o que anunciar.
            const bool ehDeRede = o.kind == int (mesa::InputKind::Network)
                               || o.livewireChannel > 0
                               || ! o.streamName.empty();
            if (! ehDeRede) continue;
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
                    saida.sender->tipoDeCarga.store (cargaTx);
                    nomesDeSaida[o.livewireChannel] = juce::String (o.name);
                    if (! saida.sender->start (o.livewireChannel, sampleRate, placaLw))
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

    /** Fecha so os receptores Livewire, para reabrirem na placa certa.

        Trocar a placa nao adianta com receptor ja aberto: ele continua preso
        ao grupo pela placa antiga ate ser refeito. */
    void fechaLivewire()
    {
        for (auto& kv : livewire) if (kv.second.receiver) kv.second.receiver->stop();
        livewire.clear();
        for (auto& kv : lwOut)    if (kv.second.sender)   kv.second.sender->stop();
        lwOut.clear();
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
    void registra (const juce::String& m) { if (aoRegistrar) aoRegistrar (m); }

    juce::String lastLivewireError, placaLw;
    int cargaTx = 96;
    LivewireAdvertiser anunciante;
    bool anuncioLigado = false;
    bool anuncioPermitido = false;
    int  hwidAnuncio = 0;
    int  udpcAnuncio = 4002;
    std::map<int, juce::String> nomesDeSaida;

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
            if (! slot.receiver->start (channel, placaLw))
            {
                lastLivewireError = slot.receiver->error();
                registra ("Livewire canal " + juce::String (channel) + ": FALHOU — "
                          + slot.receiver->error());
                return nullptr;
            }
            registra ("Livewire canal " + juce::String (channel) + ": receptor aberto em "
                      + slot.receiver->address() + ":5004  placa "
                      + slot.receiver->placa());
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
