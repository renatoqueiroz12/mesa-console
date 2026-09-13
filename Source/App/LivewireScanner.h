#pragma once
#include "../Core/NomeDaThread.h"
#include <juce_core/juce_core.h>
#include "LwrpClient.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <set>
#include <algorithm>

/** Encontra os equipamentos Livewire da rede sozinho.

    Por que varredura e nao cadastro de IP: endereco muda — DHCP, troca de
    equipamento, reconfiguracao de rede — e nome nao. Uma lista de IPs
    cadastrados se quebra sozinha meses depois, e sempre no pior momento.
    Guardando o NOME e reencontrando por varredura, a mesa se conserta.

    Por que LWRP e nao os anuncios multicast: o LWRP e texto documentado, e
    responde sempre. Os anuncios sao formato proprietario, e podem nem chegar
    se o switch bloquear multicast — que pode ser exatamente o caso aqui, ja
    que nem audio nem anuncio chegaram nesta instalacao.

    A varredura e paralela porque serial seria inviavel: 254 enderecos vezes
    o tempo limite de cada um daria minutos. Com dezesseis por vez e limite
    curto, termina em poucos segundos. */
class LivewireScanner
{
public:
    struct Achado
    {
        juce::String ip;
        juce::String equipamento;      // DEVN do VER
        juce::String versao;
        std::vector<LwrpClient::Source> fontes;
    };

    LivewireScanner() = default;
    ~LivewireScanner() { cancela(); }

    /** Varre a sub-rede da placa informada. Vazio = todas as placas locais. */
    void varre (const juce::String& placaIp)
    {
        cancela();

        std::vector<juce::String> bases;
        for (const auto& ip : juce::IPAddress::getAllAddresses (false))
        {
            const auto txt = ip.toString();
            if (txt == "127.0.0.1") continue;
            if (placaIp.isNotEmpty() && txt != placaIp) continue;

            // So redes locais de verdade. Uma maquina de estudio costuma ter
            // uma duzia de adaptadores virtuais — VPN, maquina virtual,
            // loopback de audio — e varrer todos levava 5 mil enderecos e
            // minutos. Equipamento Livewire vive em rede privada.
            if (! ehRedePrivada (txt)) continue;

            const auto base = txt.upToLastOccurrenceOf (".", true, false);
            if (std::find (bases.begin(), bases.end(), base) == bases.end())
                bases.push_back (base);
        }
        if (bases.empty()) return;

        {
            std::lock_guard<std::mutex> g (mutex);
            achados.clear();
            testados.store (0);
            total.store (int (bases.size()) * 254);
        }

        parar.store (false);
        rodando.store (true);

        worker = std::thread ([this, bases]
        {
            mesa::batizaThread ("lw-varredura");
            std::vector<std::thread> equipe;
            std::atomic<int> proximo { 0 };
            const int quantos = int (bases.size()) * 254;

            for (int t = 0; t < 16; ++t)
                equipe.emplace_back ([this, &bases, &proximo, quantos]
                {
                    mesa::batizaThread ("lw-varredura-op");
                    for (;;)
                    {
                        const int i = proximo.fetch_add (1);
                        if (i >= quantos || parar.load()) return;

                        const auto base = bases[size_t (i / 254)];
                        const auto ip = base + juce::String ((i % 254) + 1);

                        // limite curto: quem nao responde rapido nao e Livewire
                        const auto r = LwrpClient::query (ip, 400);
                        testados.fetch_add (1);
                        if (! r.ok) continue;

                        Achado a;
                        a.ip = ip;
                        a.equipamento = r.device;
                        a.versao = r.version;
                        a.fontes = r.sources;

                        std::lock_guard<std::mutex> g (mutex);
                        achados.push_back (std::move (a));
                    }
                });

            for (auto& e : equipe) e.join();
            rodando.store (false);
        });
    }

    void cancela()
    {
        parar.store (true);
        if (worker.joinable()) worker.join();
        rodando.store (false);
    }

    bool emAndamento() const noexcept { return rodando.load(); }
    int  progresso() const noexcept { return testados.load(); }
    int  totalEnderecos() const noexcept { return total.load(); }

    std::vector<Achado> resultado() const
    {
        std::lock_guard<std::mutex> g (mutex);
        return achados;
    }

    /** Placa local que enxerga os equipamentos encontrados.

        Deduzida em vez de perguntada: se achamos um QOR em 192.168.2.10, a
        placa da rede Livewire e a que esta em 192.168.2.x. Perguntar isso ao
        operador era transferir para ele um trabalho que a mesa sabe fazer —
        e, pior, um trabalho em que errar produz silencio sem mensagem. */
    juce::String placaDeduzida() const
    {
        std::lock_guard<std::mutex> g (mutex);
        if (achados.empty()) return {};

        const auto redeDoAchado = achados.front().ip.upToLastOccurrenceOf (".", true, false);
        for (const auto& ip : juce::IPAddress::getAllAddresses (false))
        {
            const auto txt = ip.toString();
            if (txt == "127.0.0.1") continue;
            if (txt.upToLastOccurrenceOf (".", true, false) == redeDoAchado) return txt;
        }
        return {};
    }

    /** Todas as fontes de todos os equipamentos, numa lista so. */
    std::vector<LwrpClient::Source> todasAsFontes() const
    {
        std::lock_guard<std::mutex> g (mutex);
        std::vector<LwrpClient::Source> v;
        std::set<std::pair<int, juce::String>> jaVistas;

        for (const auto& a : achados)
            for (const auto& f : a.fontes)
            {
                // Canal invalido nao entra.
                //
                // Equipamento com fonte configurada mas sem canal devolve zero,
                // e zero nao e endereco de coisa nenhuma. Oito linhas de "FM
                // Pre-final" com canal 0 so atrapalhavam quem procurava.
                if (f.livewireChannel <= 0) continue;

                auto copia = f;
                // o nome ganha a origem: numa rede com varios equipamentos,
                // "PGM 01" sozinho nao diz de quem e
                if (a.equipamento.isNotEmpty())
                    copia.name = f.name + " @" + a.equipamento;

                // mesma fonte anunciada duas vezes entra uma so
                const auto chave = std::make_pair (copia.livewireChannel, copia.name);
                if (! jaVistas.insert (chave).second) continue;

                v.push_back (copia);
            }
        return v;
    }

    /** Quantos equipamentos ja apareceram. A interface acompanha por aqui, em
        vez de por retorno de chamada: aviso disparado de outra thread para uma
        janela que pode ter sido fechada no meio da varredura derrubou a mesa. */
    int quantosAchados() const
    {
        std::lock_guard<std::mutex> g (mutex);
        return int (achados.size());
    }

private:
    /** 10.x, 172.16-31.x e 192.168.x — onde equipamento de estudio vive. */
    static bool ehRedePrivada (const juce::String& ip)
    {
        auto p = juce::StringArray::fromTokens (ip, ".", "");
        if (p.size() != 4) return false;
        const int a = p[0].getIntValue(), b = p[1].getIntValue();
        if (a == 10) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 192 && b == 168) return true;
        return false;
    }

    std::thread worker;
    std::atomic<bool> parar { true }, rodando { false };
    std::atomic<int> testados { 0 }, total { 0 };
    mutable std::mutex mutex;
    std::vector<Achado> achados;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivewireScanner)
};
