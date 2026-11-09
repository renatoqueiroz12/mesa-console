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
              << " recebe "
              << juce::String (kv.second.receiver != nullptr
                                   ? int (kv.second.receiver->amostrasPorSegundo()) : 0)
              << "/s consome "
              << juce::String (int (kv.second.queue->amostrasPorSegundo())) << "/s"
              << " buracos " << juce::String (kv.second.queue->amostrasEmFalta())
              << " puxadas-ruins " << juce::String (kv.second.queue->badPulls())
              << " ppm " << juce::String (kv.second.queue->correctionPpm(), 0)
              << " fila " << juce::String (kv.second.queue->latencyMs(), 1) << " ms"
              << (kv.second.queue->driftSettled() ? "" : " NAO-ASSENTOU");
        }

        // SAIDAS NDI: quanto audio esta esperando antes de ir para a rede.
        //
        // Sem este numero, "a mesa atrasa 100 ms" nao tinha como virar conta:
        // era possivel saber o que a fila DEVIA segurar, nao o que ela
        // segurava de fato. Se a correcao de relogio deixar a fila encher, e
        // aqui que aparece.
        for (const auto& kv : ndiOut)
        {
            if (kv.second.tx == nullptr) continue;
            double pior = 0.0;
            for (const auto* q : { kv.second.left.get(), kv.second.right.get() })
                if (q != nullptr) pior = std::max (pior, q->latencyMs());
            t << "  |  NDI saida " << juce::String (kv.first).substring (0, 18)
              << (kv.second.tx->ativo() ? "" : " (parada)")
              << " " << juce::String (kv.second.tx->blocos()) << " blocos"
              << " fila " << juce::String (pior, 1) << " ms";
        }
        return t;
    }

    /** Estado dos receptores Livewire, para o batimento.

        Traz a LATENCIA DE CADA FILA em milissegundos. A barra mostra so o que
        o driver da placa declara; o atraso das filas de rede ficava invisivel,
        e sem numero a conversa sobre atraso vira palpite. Cada fila cobra o
        que esta parado dentro dela: e o preco de absorver a diferenca de
        relogio entre a rede e a placa. */
    juce::String estadoLivewire() const
    {
        juce::String t;
        for (const auto& kv : livewire)
        {
            t << "  |  LW " << kv.first << " " << juce::String (kv.second.receiver->packets())
              << " pacotes carga " << juce::String (kv.second.receiver->cargaRecebida());

            // so as filas COM LEITOR: as outras nem sao alimentadas, e
            // incluir uma delas foi o que fez o registro acusar 80 ms de
            // atraso que o audio nao tinha
            double pior = 0.0;
            const auto* rec = kv.second.receiver.get();
            if (rec != nullptr)
            {
                if (rec->usandoL()    && kv.second.left  != nullptr)
                    pior = std::max (pior, kv.second.left ->latencyMs());
                if (rec->usandoR()    && kv.second.right != nullptr)
                    pior = std::max (pior, kv.second.right->latencyMs());
                if (rec->usandoSoma() && kv.second.soma  != nullptr)
                    pior = std::max (pior, kv.second.soma ->latencyMs());
            }
            t << " fila " << juce::String (pior, 1) << " ms";
        }
        for (const auto& kv : lwOut)
        {
            t << "  |  LW saida " << kv.first << " "
              << juce::String (kv.second.sender->packets()) << " pacotes";

            double pior = 0.0;
            for (const auto* q : { kv.second.left.get(), kv.second.right.get() })
                if (q != nullptr) pior = std::max (pior, q->latencyMs());
            t << " fila " << juce::String (pior, 1) << " ms";
        }
        return t;
    }

    /** Percorre o catalogo, garante que cada fonte de rede tenha slot e fila,
        e escreve o slot de volta no SourceDef. */
    void rebind (mesa::SourceCatalog& catalog, double sampleRate, int blockSize)
    {
        // Esquece o uso anterior. Sem isto, trocar um input de "soma" para
        // "esquerdo" deixaria a soma marcada para sempre, e a fila sem leitor
        // voltaria a encher.
        for (auto& kv : livewire)
            if (kv.second.receiver != nullptr) kv.second.receiver->defineUso (false, false, false);

        std::map<std::string, int> stillUsed;
        // Qual slot ja leva cada FILA. A chave da fonte nao basta: um input
        // Livewire estereo usa a fila do lado esquerdo, e outro input so no L
        // do mesmo canal tem chave diferente e pegaria a mesma fila num
        // segundo slot. O que nao pode repetir e a fila, entao e por ela.
        std::map<mesa::AsyncSource*, int> slotDaFila;
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

            // MESMA FONTE EM DOIS INPUTS: MESMO SLOT, NAO DOIS.
            //
            // Cada input do catalogo ganhava um slot proprio. Dois inputs
            // apontando para a mesma fonte de rede pediam a fila dela, recebiam
            // a MESMA fila - e ela entrava em dois slots. O callback puxa cada
            // slot uma vez por bloco, entao a fila era drenada em dobro: cada
            // leitura levava metade do audio e a outra metade virava buraco.
            //
            // O log mostrou: a fonte NDI entregando 48 mil amostras por
            // segundo e a fila acusando 48 mil buracos por segundo. No fone,
            // audio real intercalado com zeros - um chiado com o medidor
            // mexendo normal. E acontecia mesmo com um dos dois inputs fora de
            // qualquer fader, porque o callback puxa todo slot registrado.
            //
            // Agora o segundo input reaproveita o slot do primeiro. Os dois
            // leem o MESMO bloco ja puxado, que e o que significa ouvir a
            // mesma fonte em dois lugares.
            if (auto ja = stillUsed.find (key); ja != stillUsed.end())
            {
                src.index = ja->second;
                auto jaR = stillUsed.find (key + "/R");
                src.indexR = (jaR != stillUsed.end()) ? jaR->second : -1;
                continue;
            }

            mesa::AsyncSource* q = nullptr;

            if (src.livewireChannel > 0)
            {
                // em estereo a esquerda usa o lado 0; a direita vem logo abaixo
                q = livewireQueue (src.livewireChannel,
                                   src.livewireSide == 3 ? 0 : src.livewireSide,
                                   sampleRate, blockSize);
                // depois de criar: marcar antes nao acharia o receptor e o
                // canal ficaria mudo, que e pior que a fila cheia
                marcaUso (src.livewireChannel, src.livewireSide);
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

            if (q == nullptr) { src.index = -1; src.indexR = -1; continue; }

            if (auto ja = slotDaFila.find (q); ja != slotDaFila.end())
            {
                src.index = ja->second;       // fila ja puxada: le o mesmo bloco
                src.indexR = -1;
                stillUsed[key] = ja->second;
            }
            else
            {
                engine.setNetSlot (slot, q);
                slotDaFila[q] = slot;
                src.index = slot;
                src.indexR = -1;
                stillUsed[key] = slot;
                ++slot;
            }

            // NDI EM ESTEREO: a direita ocupa o slot seguinte, como no Livewire.
            if (! src.streamName.empty() && src.livewireChannel <= 0
                && slot < AudioEngine::kMaxNetSlots)
            {
                if (auto* qr = ndiQueueR (src.streamName))
                {
                    if (auto ja = slotDaFila.find (qr); ja != slotDaFila.end())
                    {
                        src.indexR = ja->second;
                        stillUsed[key + "/R"] = ja->second;
                    }
                    else
                    {
                        engine.setNetSlot (slot, qr);
                        slotDaFila[qr] = slot;
                        src.indexR = slot;
                        stillUsed[key + "/R"] = slot;
                        ++slot;
                    }
                }
            }

            // ESTEREO: a segunda ponta ocupa um slot proprio. Nao ha tabela
            // paralela para a direita - o canal guarda os dois indices e busca
            // os dois na mesma tabela, que e a que ja existia.
            if (src.livewireChannel > 0 && src.livewireSide == 3
                && slot < AudioEngine::kMaxNetSlots)
            {
                if (auto* qr = livewireQueue (src.livewireChannel, 1, sampleRate, blockSize))
                {
                    if (auto ja = slotDaFila.find (qr); ja != slotDaFila.end())
                    {
                        src.indexR = ja->second;
                        stillUsed[key + "/R"] = ja->second;
                    }
                    else
                    {
                        engine.setNetSlot (slot, qr);
                        slotDaFila[qr] = slot;
                        src.indexR = slot;
                        stillUsed[key + "/R"] = slot;
                        ++slot;
                    }
                }
            }
        }

        for (int i = slot; i < AudioEngine::kMaxNetSlots; ++i)
            engine.setNetSlot (i, nullptr);

        // Livewire pode ser desligado com seguranca: e codigo nosso, sem DLL
        // de terceiro no caminho. Ainda assim paramos a thread antes de soltar.
        for (auto it = livewire.begin(); it != livewire.end(); )
        {
            // Por PREFIXO, nao por lista de lados. A lista escrita na mao ja
            // destruiu um receptor recem-criado - o do lado 3, estereo, que
            // ninguem lembrou de acrescentar - deixando o motor com ponteiro
            // solto e derrubando a mesa no primeiro bloco de audio.
            bool emUso = false;
            for (const auto& kv : stillUsed)
                if (mesa::chaveEhDoCanal (kv.first, it->first)) { emUso = true; break; }

            if (! emUso) { it->second.receiver->stop(); it = livewire.erase (it); }
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
    /** Nome da saida que o gerador de teste esta tomando. Vazio = nenhuma. */
    std::string destinoDoTeste;

    /** Espera por fila NDI, em ms. Vem da configuracao; vale para filas
        criadas a partir de agora. Publico porque quem decide e o
        MainComponent, na hora de religar a rede. */
    double ndiLatenciaMs = 20.0;

    void rebindOutputs (mesa::OutputCatalog& outputs, const mesa::SourceCatalog& catalog,
                        double sampleRate, int blockSize)
    {
        /** Nome da fonte -> o par (kind, index) que o motor entende.

            Tem de rodar DEPOIS do rebind das entradas, que e quem atribui o
            index das fontes de rede. Fonte que nao existe mais no catalogo
            devolve -1: o destino cai no barramento em vez de mandar o audio de
            outra fonte que por acaso ficou naquele slot. */
        auto achaFonte = [&catalog] (const std::string& nome, int& kind, int& idx)
        {
            kind = 0; idx = -1;
            if (nome.empty()) return;
            for (const auto& s : catalog.sources)
                if (s.name == nome) { kind = s.kind; idx = s.index; return; }
        };

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

            int dKind = 0, dIdx = -1;
            achaFonte (o.directSource, dKind, dIdx);
            const bool ehTeste = ! o.name.empty() && o.name == destinoDoTeste;
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
                                   o.busSource, dKind, dIdx, o.mixMinusChannel, ehTeste, o.ganhoDb);
                ++slot;
                continue;
            }

            // TRANSMISSAO NDI: a mesa vira fonte na rede de video.
            //
            // Ate aqui a linha de baixo descartava qualquer saida sem placa,
            // com o comentario "envio por NDI ainda nao ligado". O transmissor
            // existia e ninguem o ligava: a saida aparecia na lista, parecia
            // configurada, e o vMix nunca a via.
            if (! o.streamName.empty())
            {
                const std::string nome = o.name.empty() ? std::string ("MESA") : o.name;
                auto it = ndiOut.find (nome);
                if (it == ndiOut.end())
                {
                    NdiSaida saida;
                    saida.left  = std::make_unique<mesa::AsyncSource>();
                    saida.right = std::make_unique<mesa::AsyncSource>();
                    // a fila tem de comportar o bloco do NDI, independente do
                    // buffer da placa - mesmo cuidado da saida Livewire
                    const int blocoNdi = juce::jmax (blockSize, NdiTransmissor::kBloco);
                    const int blocos = mesa::blocosParaLatencia (ndiLatenciaMs, sampleRate, blocoNdi);
                    saida.left ->prepare (blocoNdi, blocos, sampleRate, 0.5);
                    saida.right->prepare (blocoNdi, blocos, sampleRate, 0.5);
                    saida.tx = std::make_unique<NdiTransmissor> (*saida.left, *saida.right);
                    if (! saida.tx->start (nome, sampleRate))
                    {
                        lastNdiError = juce::String (saida.tx->ultimoErro());
                        continue;
                    }
                    it = ndiOut.emplace (nome, std::move (saida)).first;
                }
                else if (it->second.tx != nullptr && it->second.tx->blocos() >= 0
                         && ! it->second.tx->ativo())
                {
                    it->second.tx->start (nome, sampleRate);   // voltou para a lista
                }
                engine.setNetSink (slot, it->second.left.get(), it->second.right.get(),
                                   o.busSource, dKind, dIdx, o.mixMinusChannel, ehTeste, o.ganhoDb);
                ++slot;
                continue;
            }

            if (o.deviceName.empty()) continue;

            auto* dev = secondaries.open (juce::String (o.deviceType.empty()
                                              ? std::string ("Windows Audio") : o.deviceType),
                                          juce::String (o.deviceName), sampleRate, blockSize);
            if (dev == nullptr) continue;

            const int base = o.pair * 2;
            engine.setNetSink (slot, dev->sink (base), dev->sink (base + 1), o.busSource,
                               dKind, dIdx, o.mixMinusChannel, ehTeste, o.ganhoDb);
            ++slot;
        }
        for (int i = slot; i < AudioEngine::kMaxNetSinks; ++i)
            engine.setNetSink (i, nullptr, nullptr, 0);

        // Saida NDI removida da lista para de ser ANUNCIADA - mas as filas
        // ficam vivas.
        //
        // Apagar a entrada destruiria as filas enquanto o callback de audio
        // ainda pode estar escrevendo nelas pelo ponteiro antigo: escrita em
        // memoria devolvida, que e exatamente a corrupcao de heap que ja
        // derrubou esta mesa. A saida Livewire nunca apaga as dela pelo mesmo
        // motivo. Parado o transmissor, a fonte some do vMix; as filas custam
        // alguns KB e voltam a servir se a saida for recriada com o mesmo nome.
        for (auto& kv : ndiOut)
        {
            const bool aindaExiste = std::any_of (
                outputs.outputs.begin(), outputs.outputs.end(),
                [&kv] (const mesa::OutputDef& o)
                { return ! o.streamName.empty()
                      && (o.name.empty() ? std::string ("MESA") : o.name) == kv.first; });
            if (! aindaExiste && kv.second.tx) kv.second.tx->stop();
        }
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
        // PRIMEIRO o motor esquece as filas, DEPOIS elas morrem.
        //
        // Estava na ordem contraria: os mapas eram limpos - destruindo as
        // filas - e so entao o motor largava os ponteiros. Se o callback de
        // audio ainda estivesse rodando, escrevia em memoria ja devolvida.
        // Com mais saidas de rede, mais chance de cair justo nessa janela.
        engine.clearNetSlots();
        engine.clearNetSinks();

        for (auto& kv : ndi)      if (kv.second.receiver) kv.second.receiver->stop();
        for (auto& kv : livewire) if (kv.second.receiver) kv.second.receiver->stop();
        for (auto& kv : lwOut)    if (kv.second.sender)   kv.second.sender->stop();
        for (auto& kv : ndiOut)   if (kv.second.tx)       kv.second.tx->stop();
        ndi.clear();
        livewire.clear();
        lwOut.clear();
        ndiOut.clear();
    }

    /** Estado para a barra de status: placas perdidas e streams sem sinal. */
    std::vector<juce::String> problems() const
    {
        auto v = secondaries.lostDevices();
        if (lastLivewireError.isNotEmpty()) v.push_back ("Livewire: " + lastLivewireError);
        if (lastNdiError.isNotEmpty())      v.push_back ("NDI saida: " + lastNdiError);
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

    /** Saidas NDI, pelo NOME publicado. Cada saida vira uma fonte separada
        na rede, e o vMix mostra cada uma com o nome que ela tem na lista. */
    struct NdiSaida
    {
        std::unique_ptr<mesa::AsyncSource> left, right;
        std::unique_ptr<NdiTransmissor> tx;
    };
    std::map<std::string, NdiSaida> ndiOut;

    juce::String lastNdiError;

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
        std::unique_ptr<mesa::AsyncSource> queueR;     // lado direito
        std::unique_ptr<NdiReceiver> receiver;
        bool ocioso = false;          // fora do catalogo, mas nao destruido
    };

    static std::string keyOf (const mesa::SourceDef& s) { return mesa::chaveDaFonte (s); }

    /** Um receptor por CANAL; os dois lados do estereo saem dele. */
    /** Diz ao receptor quais pontas tem leitor. Acumula: dois inputs podem
        pegar lados diferentes do mesmo canal. */
    void marcaUso (int canal, int lado)
    {
        auto it = livewire.find (canal);
        if (it == livewire.end() || it->second.receiver == nullptr) return;
        auto& r = *it->second.receiver;
        r.defineUso (r.usandoL()    || lado == 0 || lado == 3,
                     r.usandoR()    || lado == 1 || lado == 3,
                     r.usandoSoma() || lado == 2);
    }

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

    /** Fila da direita de uma fonte NDI ja aberta. */
    mesa::AsyncSource* ndiQueueR (const std::string& stream)
    {
        auto it = ndi.find (stream);
        return it != ndi.end() ? it->second.queueR.get() : nullptr;
    }

    mesa::AsyncSource* ndiQueue (const std::string& stream, double sr, int block)
    {
        auto it = ndi.find (stream);
        if (it != ndi.end()) return it->second.queue.get();

        NdiSlot s;
        s.queue = std::make_unique<mesa::AsyncSource>();
        // dimensionada por TEMPO, nao por blocos da placa
        s.queue->prepare (block, mesa::blocosParaLatencia (ndiLatenciaMs, sr, block), sr, 0.5);
        s.queue->name = stream;
        s.queue->kind = mesa::AsyncSource::Kind::Ndi;

        // a direita, com a mesma medida: os dois lados tem de andar juntos,
        // senao a imagem estereo escorrega com o tempo
        s.queueR = std::make_unique<mesa::AsyncSource>();
        s.queueR->prepare (block, mesa::blocosParaLatencia (ndiLatenciaMs, sr, block), sr, 0.5);
        s.queueR->name = stream + " (R)";
        s.queueR->kind = mesa::AsyncSource::Kind::Ndi;

        s.receiver = std::make_unique<NdiReceiver> (*s.queue, sr, block);
        s.receiver->defineDireita (s.queueR.get());
        if (! s.receiver->start (stream)) return nullptr;

        auto* q = s.queue.get();
        ndi.emplace (stream, std::move (s));
        return q;
    }

    AudioEngine& engine;
    SecondaryDevices& secondaries;
    std::map<std::string, NdiSlot> ndi;
};
