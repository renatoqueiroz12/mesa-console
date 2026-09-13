#pragma once
#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
#endif
#include <juce_core/juce_core.h>
#include "../Core/AnuncioLw.h"
#include "../Core/NomeDaThread.h"
#include <atomic>
#include <map>
#include <thread>
#include <vector>

/** Anuncia as fontes da mesa na rede Livewire.

    Por que isto existe: o QOR marca um canal como "Used EW" quando recebe o
    audio mas nao encontra anuncio correspondente. Sem anuncio ele nao completa
    a inscricao e nao sabe distinguir "ninguem anunciou" de "outro motor ja
    pegou". O audio funciona e o rotulo fica errado.

    Os BYTES do anuncio nao moram mais aqui: estao em Core/AnuncioLw.h, onde a
    suite os confere sem rede e sem JUCE. Aqui ficam so os sockets, que e o que
    nao da para testar em bancada.

    DUAS MUDANCAS, e a razao de cada uma:

    1. O HWID NAO SAI MAIS DO IP. O anuncio identifica o no pelo par HWID/INIP.
       Numa maquina que tambem roda o IP-Driver da Axia os dois dividem o IP;
       tirando o HWID do IP, dividiam tambem o HWID, o console via um no so e
       ficava com o ultimo anuncio — o nosso, com uma fonte, apagava as quatro
       do driver. Agora o HWID sai do NOME da maquina e foge, por construcao,
       do valor que o IP daria.

    2. A MESA ESCUTA A PORTA QUE ANUNCIA. O campo UDPC diz ao console em que
       porta falar com o no. Anunciavamos 4000 — a porta do driver — e nao
       abriamos porta nenhuma. Se o console diz alguma coisa antes de confirmar
       a fonte, ia para o vizinho e nunca viamos. Agora anunciamos uma porta
       nossa, abrimos, e gravamos tudo que chega em
       %APPDATA%\MesaConsole\anuncios-recebidos.txt. Enquanto esse arquivo
       ficar vazio, esta provado que o console nao fala com o no — e isso e
       resposta, nao palpite. */
class LivewireAdvertiser
{
public:
    LivewireAdvertiser() = default;
    ~LivewireAdvertiser() { stop(); }

    using Fonte = mesa::lw::FonteAnuncio;

    void setFontes (std::vector<Fonte> f)
    {
        const std::lock_guard<std::mutex> g (mutex);
        fontes = std::move (f);
    }

    /** @param hwidPedido  0 = deduzir do nome da maquina (o recomendado).
        @param udpcPedido  porta de controle publicada. 4000 e a do driver: em
                           maquina que roda o driver, tem de ser outra. */
    bool start (const juce::String& placaIp, const juce::String& nomeDaMaquina,
                int hwidPedido = 0, int udpcPedido = 4002)
    {
        stop();

        placa = placaIp;
        maquina = nomeDaMaquina.isNotEmpty() ? nomeDaMaquina
                                             : juce::SystemStats::getComputerName();

        eu.ip      = numeroDoIp (placa);
        eu.maquina = maquina.toStdString();
        eu.udpc    = unsigned (udpcPedido > 0 ? udpcPedido : 4002);

        const unsigned doDriver = eu.ip & 0xffffu;
        const unsigned desejado = hwidPedido > 0
                                      ? unsigned (hwidPedido)
                                      : mesa::lw::hwidDeTexto (eu.maquina + "/MesaConsole");
        eu.hwid = mesa::lw::hwidLivre (desejado, { doDriver });

        socket = std::make_unique<juce::DatagramSocket> (true);
        if (! socket->bindToPort (0, placa)) { socket = nullptr; return false; }
        socket->setMulticastLoopbackEnabled (true);
        pedePlacaDeSaida (*socket);

        // A porta que o anuncio promete. Se nao abrir, seguimos anunciando —
        // anunciar com a porta ocupada e melhor que nao anunciar — mas fica
        // registrado, porque e a diferenca entre "o console nao falou" e "o
        // console falou e nao tinha ninguem ouvindo".
        controle = std::make_unique<juce::DatagramSocket> (true);
        controle->setEnablePortReuse (true);
        if (! controle->bindToPort (int (eu.udpc)))
        {
            anota ("NAO consegui abrir a porta de controle " + juce::String (int (eu.udpc))
                   + " — outro programa ja a tem. Escolha outra nas configuracoes.");
            controle = nullptr;
        }

        sair.store (false);
        worker = std::thread ([this] { loop(); });
        if (controle != nullptr)
            ouvinte = std::thread ([this] { loopControle(); });

        anota ("anunciando pela placa " + placa
               + "  HWID 0x" + juce::String::toHexString (int (eu.hwid))
               + "  UDPC " + juce::String (int (eu.udpc))
               + "  (do IP sairia 0x" + juce::String::toHexString (int (doDriver))
               + ", que e o do driver nesta maquina)");
        return true;
    }

    void stop()
    {
        sair.store (true);
        if (worker.joinable())  worker.join();
        if (ouvinte.joinable()) ouvinte.join();
        socket = nullptr;
        controle = nullptr;
    }

    int enviados()  const noexcept { return contaEnviados.load(); }
    /** Datagramas que o console mandou de volta. Zero tambem e diagnostico. */
    int recebidos() const noexcept { return contaRecebidos.load(); }
    unsigned hwid() const noexcept { return eu.hwid; }
    unsigned udpc() const noexcept { return eu.udpc; }

private:
    static constexpr const char* kGrupo = "239.192.255.3";
    static constexpr int kPorta = 4001;

    static unsigned numeroDoIp (const juce::String& s)
    {
        const auto ip = juce::IPAddress (s);
        return (unsigned (ip.address[0]) << 24) | (unsigned (ip.address[1]) << 16)
             | (unsigned (ip.address[2]) << 8)  |  unsigned (ip.address[3]);
    }

    void loop()
    {
        mesa::batizaThread ("lw-anuncio");

        while (! sair.load())
        {
            std::vector<Fonte> copia;
            { const std::lock_guard<std::mutex> g (mutex); copia = fontes; }

            if (! copia.empty())
            {
                // O ADVT 3 nao inventa o estado de uso: devolve o que o console
                // ESCREVEU pela porta de controle. Sem isso ele reescrevia o
                // mesmo comando uma vez por segundo, nunca via a confirmacao, e
                // marcava a fonte como usada em outro lugar.
                {
                    const std::lock_guard<std::mutex> g (mutex);
                    for (auto& f : copia)
                    {
                        const auto it = usoPorCanal.find (f.canal);
                        f.uso = it != usoPorCanal.end() ? it->second : 0;
                    }
                }

                envia (mesa::lw::montaLista (eu, copia, ++sequencia));
                envia (mesa::lw::montaVida  (eu, copia.size(), ++sequencia));
                envia (mesa::lw::montaUso   (eu, copia, ++sequencia));
            }

            // um por segundo: o ritmo do driver da Axia nesta rede. Mais rapido
            // so gera trafego; mais devagar e o console pode dar a fonte por
            // desligada. No meio, atende quem responder no socket do anuncio —
            // se a resposta vier por unicast, e ali que ela cai.
            for (int i = 0; i < 10 && ! sair.load(); ++i)
            {
                drena (socket.get(), "resposta ao anuncio");
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
            }
        }
    }

    void loopControle()
    {
        mesa::batizaThread ("lw-controle");
        while (! sair.load())
        {
            if (controle == nullptr) return;
            if (controle->waitUntilReady (true, 250) <= 0) continue;
            drena (controle.get(), "porta de controle");
        }
    }

    void envia (const std::vector<std::uint8_t>& p)
    {
        if (socket == nullptr || p.empty()) return;
        if (socket->write (kGrupo, kPorta, p.data(), int (p.size())) > 0)
            contaEnviados.fetch_add (1);
    }

    /** Le o que houver e guarda. Nao interpreta: ainda nao sabemos o que o
        console manda, e o primeiro passo e ter o exemplar na mao. */
    void drena (juce::DatagramSocket* s, const char* de)
    {
        if (s == nullptr) return;
        char buf[2048];
        while (! sair.load() && s->waitUntilReady (true, 0) > 0)
        {
            juce::String origem; int porta = 0;
            const int n = s->read (buf, int (sizeof (buf)), false, origem, porta);
            if (n <= 0) break;
            contaRecebidos.fetch_add (1);
            if (! aplicaEscrita (buf, n, origem))
                gravaRecebido (de, origem, porta, buf, n);
        }
    }

    /** O console escreve o estado de uso: NEST + S### com WRIN, PSID e BUSY.

        Guardamos por canal e devolvemos no proximo ADVT 3. Devolve true quando
        entendeu o pacote — o que nao entendemos vai cru para o arquivo, porque
        e o unico jeito de descobrir o resto do protocolo.

        So registra MUDANCA: o comando chega uma vez por segundo enquanto a
        fonte fica assinada, e gravar todos encheria o arquivo em minutos. */
    bool aplicaEscrita (const char* dados, int n, const juce::String& origem)
    {
        const auto a = mesa::lw::le (reinterpret_cast<const std::uint8_t*> (dados),
                                     size_t (n));
        if (! a.ok || a.advt != 0 || a.fontes.empty()) return false;

        for (const auto& f : a.fontes)
        {
            if (f.canal <= 0) continue;

            bool mudou = false;
            {
                const std::lock_guard<std::mutex> g (mutex);
                const auto it = usoPorCanal.find (f.canal);
                mudou = it == usoPorCanal.end() || it->second != f.uso;
                usoPorCanal[f.canal] = f.uso;
            }

            if (mudou)
            {
                const std::lock_guard<std::mutex> g (mutexArquivo);
                escreve (juce::Time::getCurrentTime().toString (true, true)
                         + "  canal " + juce::String (f.canal)
                         + (f.uso == 0 ? juce::String (" liberado por ")
                                       : juce::String (" assumido por "))
                         + origem + "  BUSY 0x"
                         + juce::String::toHexString (juce::int64 (f.uso))
                                .paddedLeft ('0', 16) + "\n\n");
            }
        }
        return true;
    }

    void gravaRecebido (const char* de, const juce::String& origem, int porta,
                        const char* dados, int n)
    {
        const std::lock_guard<std::mutex> g (mutexArquivo);
        if (gravados >= 60) return;          // o bastante para ver o padrao
        ++gravados;

        juce::String linha;
        linha << "[" << gravados << "] " << de << " — de " << origem << ":" << porta
              << ", " << n << " bytes\n";

        const auto* b = reinterpret_cast<const unsigned char*> (dados);
        for (int i = 0; i < n; ++i)
        {
            linha << juce::String::toHexString (int (b[i])).paddedLeft ('0', 2) << " ";
            if ((i + 1) % 16 == 0) linha << "\n";
        }
        linha << "\n";
        for (int i = 0; i < n; ++i)
            linha << ((b[i] >= 32 && b[i] < 127)
                          ? juce::String::charToString (juce::juce_wchar (b[i]))
                          : juce::String ("."));
        linha << "\n\n";

        escreve (linha);
    }

    void anota (const juce::String& t)
    {
        const std::lock_guard<std::mutex> g (mutexArquivo);
        escreve (juce::Time::getCurrentTime().toString (true, true) + "  " + t + "\n\n");
    }

    static void escreve (const juce::String& texto)
    {
        auto pasta = juce::File::getSpecialLocation (
                         juce::File::userApplicationDataDirectory)
                     .getChildFile ("MesaConsole");
        pasta.createDirectory();
        pasta.getChildFile ("anuncios-recebidos.txt").appendText (texto, false, false, "\n");
    }

    /** Multicast pela placa escolhida. Numa maquina com seis placas, deixar o
        Windows decidir e sortear a rede — ja custou silencio antes. */
    void pedePlacaDeSaida (juce::DatagramSocket& s)
    {
       #if JUCE_WINDOWS
        if (placa.isEmpty()) return;
        const int fd = s.getRawSocketHandle();
        if (fd < 0) return;
        in_addr ifaddr {};
        ifaddr.s_addr = ::inet_addr (placa.toRawUTF8());
        ::setsockopt (SOCKET (fd), IPPROTO_IP, IP_MULTICAST_IF,
                      reinterpret_cast<const char*> (&ifaddr), sizeof (ifaddr));
       #else
        juce::ignoreUnused (s);
       #endif
    }

    std::unique_ptr<juce::DatagramSocket> socket, controle;
    std::thread worker, ouvinte;
    std::atomic<bool> sair { true };
    std::atomic<int> contaEnviados { 0 }, contaRecebidos { 0 };
    std::mutex mutex, mutexArquivo;
    std::vector<Fonte> fontes;
    /** Estado de uso que o console escreveu, por canal. */
    std::map<int, std::uint64_t> usoPorCanal;
    juce::String placa, maquina;
    mesa::lw::Identidade eu;
    unsigned sequencia = 0;
    int gravados = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivewireAdvertiser)
};
