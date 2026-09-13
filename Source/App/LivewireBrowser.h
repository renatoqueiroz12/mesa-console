#pragma once
#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
#endif
#include "../Core/NomeDaThread.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

/** Escuta os anuncios do Livewire e monta a lista de fontes da rede.

    E assim que a lista "Select Source" do QOR se enche: cada equipamento
    anuncia periodicamente o que oferece, no grupo 239.192.255.3, porta 4001.
    Por isso ela mostra tambem o "PC 1@DESKTOP" do driver da Axia, que o LWRP
    do no nao conhece — o LWRP so sabe das fontes DAQUELE no.

    Ressalva importante: o formato do anuncio e proprietario e nao publicado.
    O que fazemos aqui e reconhecer o que da para reconhecer com seguranca —
    textos legiveis e numeros de canal plausiveis — e guardar os bytes crus dos
    primeiros pacotes para afinar depois. Preferi isso a fingir que entendo o
    formato inteiro: leitura errada de nome de fonte no ar e pior que lista
    incompleta. */
class LivewireBrowser
{
public:
    struct Fonte
    {
        int canal = 0;
        juce::String nome;
        juce::String origem;        // IP de quem anunciou
        double vistoMs = 0.0;
    };

    /** A macro de nao-copiavel do JUCE tambem some com o construtor padrao. */
    LivewireBrowser() = default;
    ~LivewireBrowser() { stop(); }

    void start (const juce::String& placaIp)
    {
        stop();
        placa = placaIp;
        sair.store (false);
        worker = std::thread ([this] { loop(); });
    }

    void stop()
    {
        sair.store (true);
        if (socket != nullptr) socket->shutdown();
        if (worker.joinable()) worker.join();
        socket = nullptr;
    }

    bool ativo() const noexcept { return worker.joinable(); }
    int  pacotes() const noexcept { return recebidos.load(); }
    juce::String erro() const { std::lock_guard<std::mutex> g (mutex); return ultimoErro; }

    std::vector<Fonte> fontes() const
    {
        std::lock_guard<std::mutex> g (mutex);
        std::vector<Fonte> v;
        v.reserve (achadas.size());
        for (const auto& kv : achadas) v.push_back (kv.second);
        return v;
    }

    /** Bytes dos primeiros pacotes, em hexadecimal, para afinar o leitor.
        Sem isso, ajustar o parser vira tentativa e erro as cegas. */
    juce::StringArray amostrasCruas() const
    {
        std::lock_guard<std::mutex> g (mutex);
        return cruas;
    }

private:
    static constexpr int kPorta = 4001;
    static constexpr const char* kGrupo = "239.192.255.3";

    bool entraNoGrupoPelaPlaca()
    {
        if (placa.isEmpty() || socket == nullptr) return false;
       #if JUCE_WINDOWS
        struct ip_mreq req {};
        req.imr_multiaddr.s_addr = ::inet_addr (kGrupo);
        req.imr_interface.s_addr = ::inet_addr (placa.toRawUTF8());
        const int fd = socket->getRawSocketHandle();
        if (fd < 0) return false;
        return ::setsockopt (SOCKET (fd), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             reinterpret_cast<const char*> (&req), sizeof (req)) == 0;
       #else
        return false;
       #endif
    }

    void loop()
    {
        mesa::batizaThread ("lw-anuncios");
        socket = std::make_unique<juce::DatagramSocket> (false);
        socket->setEnablePortReuse (true);

        // mesma razao da recepcao de audio: preso ao endereco da placa, o
        // socket perde o que nasce na propria maquina
        if (! socket->bindToPort (kPorta)) { anota ("nao consegui abrir a porta 4001"); return; }

        // pela placa escolhida, como no receptor de audio: sem isto o ouvinte
        // entrava na rede que o Windows escolhesse, e capturava anuncios de
        // outra rede enquanto os da rede certa passavam despercebidos
        if (! entraNoGrupoPelaPlaca() && ! socket->joinMulticast (kGrupo))
        {
            anota (juce::String ("nao consegui entrar no grupo de anuncios ") + kGrupo
                   + (placa.isNotEmpty() ? " pela placa " + placa : juce::String())
                   + " — VLAN do Livewire? IGMP no switch?");
            return;
        }
        // idem para os anuncios: equipamento na propria maquina tambem anuncia
        socket->setMulticastLoopbackEnabled (true);
        anota ({});

        std::vector<char> buf (4096);
        while (! sair.load())
        {
            if (socket->waitUntilReady (true, 250) <= 0) continue;

            juce::String de; int porta = 0;
            const int n = socket->read (buf.data(), int (buf.size()), false, de, porta);
            if (n <= 0) continue;

            recebidos.fetch_add (1);
            guardaAmostra (buf.data(), n);
            interpreta (buf.data(), n, de);
        }
    }

    /** Extrai nomes legiveis e numeros de canal plausiveis.

        Heuristica, e assumida como tal: procura sequencias de texto imprimivel
        e, perto delas, inteiros que caibam na faixa de canal do Livewire
        (1..32767). Nomes de fonte no Livewire costumam vir como texto puro no
        pacote — foi o que permitiu montar a lista sem a especificacao. */
    void interpreta (const char* dados, int n, const juce::String& de)
    {
        const auto* b = reinterpret_cast<const unsigned char*> (dados);

        // 1) todos os trechos de texto imprimivel com 3 ou mais caracteres
        std::vector<std::pair<int, juce::String>> textos;
        juce::String atual;
        int inicio = 0;
        for (int i = 0; i < n; ++i)
        {
            const unsigned char c = b[i];
            const bool imprimivel = (c >= 32 && c < 127);
            if (imprimivel)
            {
                if (atual.isEmpty()) inicio = i;
                atual += juce::String::charToString (juce::juce_wchar (c));
            }
            else
            {
                if (atual.length() >= 3) textos.push_back ({ inicio, atual });
                atual.clear();
            }
        }
        if (atual.length() >= 3) textos.push_back ({ inicio, atual });

        // 2) inteiros de 32 e 16 bits que caibam na faixa de canal
        auto canalPerto = [&] (int posTexto) -> int
        {
            for (int i = juce::jmax (0, posTexto - 12); i + 4 <= n && i < posTexto + 4; ++i)
            {
                const int v32 = (int (b[i]) << 24) | (int (b[i+1]) << 16)
                              | (int (b[i+2]) << 8) | int (b[i+3]);
                if (v32 > 0 && v32 <= 32767) return v32;
                const int v16 = (int (b[i]) << 8) | int (b[i+1]);
                if (v16 > 0 && v16 <= 32767) return v16;
            }
            return 0;
        };

        const double agora = juce::Time::getMillisecondCounterHiRes();
        std::lock_guard<std::mutex> g (mutex);

        for (const auto& t : textos)
        {
            // descarta o que claramente nao e nome de fonte
            if (t.second.startsWithIgnoreCase ("http")) continue;
            if (t.second.containsOnly ("0123456789.")) continue;

            const int canal = canalPerto (t.first);
            if (canal <= 0) continue;

            auto& f = achadas[canal];
            f.canal = canal;
            if (f.nome.isEmpty() || t.second.length() > f.nome.length())
                f.nome = t.second.trim();
            f.origem = de;
            f.vistoMs = agora;
        }
    }

    void guardaAmostra (const char* dados, int n)
    {
        std::lock_guard<std::mutex> g (mutex);

        // Grava os anuncios INTEIROS num arquivo.
        //
        // Tres amostras cortadas em 96 bytes serviam para conferir se o
        // formato batia. Para ESCREVER um anuncio que o QOR aceite, precisamos
        // do exemplar completo de quem ele ja aceita — o driver da Axia nesta
        // mesma rede. Copiar algo comprovado vale mais que interpretar
        // documentacao de terceiro.
        gravaParaEstudo (dados, n);

        if (cruas.size() >= 3) return;

        juce::String linha;
        linha << n << " bytes: ";
        const auto* b = reinterpret_cast<const unsigned char*> (dados);
        for (int i = 0; i < juce::jmin (n, 96); ++i)
            linha << juce::String::toHexString (int (b[i])).paddedLeft ('0', 2) << " ";
        cruas.add (linha);
    }

    /** Guarda os primeiros anuncios completos, em hexadecimal, para analise. */
    void gravaParaEstudo (const char* dados, int n)
    {
        if (gravados >= 40) return;            // o bastante para ver o padrao
        ++gravados;

        auto pasta = juce::File::getSpecialLocation (
                         juce::File::userApplicationDataDirectory)
                     .getChildFile ("MesaConsole");
        pasta.createDirectory();

        juce::String linha;
        linha << "[" << gravados << "] " << n << " bytes\n";

        const auto* b = reinterpret_cast<const unsigned char*> (dados);
        for (int i = 0; i < n; ++i)
        {
            linha << juce::String::toHexString (int (b[i])).paddedLeft ('0', 2) << " ";
            if ((i + 1) % 16 == 0) linha << "\n";
        }
        linha << "\n";

        // e o mesmo trecho em texto legivel, onde os nomes aparecem
        for (int i = 0; i < n; ++i)
            linha << ((b[i] >= 32 && b[i] < 127) ? juce::String::charToString (
                          juce::juce_wchar (b[i])) : juce::String ("."));
        linha << "\n\n";

        pasta.getChildFile ("anuncios.txt").appendText (linha, false, false, "\n");
    }

    void anota (const juce::String& e)
    {
        std::lock_guard<std::mutex> g (mutex);
        ultimoErro = e;
    }

    juce::String placa;
    std::unique_ptr<juce::DatagramSocket> socket;
    std::thread worker;
    std::atomic<bool> sair { true };
    std::atomic<int> recebidos { 0 };
    mutable std::mutex mutex;
    std::map<int, Fonte> achadas;
    juce::StringArray cruas;
    int gravados = 0;
    juce::String ultimoErro;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivewireBrowser)
};
