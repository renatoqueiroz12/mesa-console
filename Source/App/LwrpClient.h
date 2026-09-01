#pragma once
#include <juce_core/juce_core.h>
#include <vector>

/** Cliente do LWRP, o protocolo de controle do Livewire, na porta 93.

    E texto puro, uma linha por item, entre BEGIN e END. Um QOR responde assim:

        SRC 1 PSNM:"PGM 01 - GRAVA" RTPE:1 RTPA:"239.192.12.28" NCHN:2 RTPP:12
        DST 1 NAME:"INPUT" ADDR:"239.192.0.101:5004" NCHN:2

    Preferi isto a decodificar os anuncios multicast: o formato do anuncio e
    proprietario e nao documentado, enquanto o LWRP e texto legivel, funciona
    de outra sub-rede e nao depende de IGMP no caminho.

    A consulta e feita na thread da interface, com tempo limite curto. Nao ha
    conexao mantida: um no com dezenas de fontes responde em fracao de segundo,
    e conexao aberta o dia inteiro seria mais uma coisa para cair de madrugada. */
class LwrpClient
{
public:
    struct Source
    {
        int number = 0;                 // posicao no no (1..NSRC)
        juce::String name;              // PSNM
        juce::String address;           // RTPA, ex. "239.192.12.28"
        int channels = 2;               // NCHN
        int livewireChannel = 0;        // deduzido do endereco
    };

    struct Destination
    {
        int number = 0;
        juce::String name;              // NAME
        juce::String address;           // ADDR, ex. "239.192.0.101:5004"
        int channels = 0;
        bool free() const noexcept { return address.isEmpty(); }
    };

    struct Result
    {
        bool ok = false;
        juce::String device, version, error;
        std::vector<Source> sources;
        std::vector<Destination> destinations;
    };

    /** Canal Livewire a partir do endereco: 239.192.y.z -> y*256 + z. */
    static int channelFromAddress (const juce::String& addr)
    {
        auto parts = juce::StringArray::fromTokens (addr.upToFirstOccurrenceOf (":", false, false),
                                                    ".", "");
        if (parts.size() != 4) return 0;
        if (parts[0] != "239" || parts[1] != "192") return 0;
        return parts[2].getIntValue() * 256 + parts[3].getIntValue();
    }

    static Result query (const juce::String& host, int timeoutMs = 3000)
    {
        Result r;
        juce::StreamingSocket sock;
        if (! sock.connect (host, 93, timeoutMs))
        {
            r.error = "sem resposta em " + host + ":93";
            return r;
        }

        sock.write ("VER\r\nSRC\r\nDST\r\n", 15);

        juce::String all;
        char buf[4096];
        const auto deadline = juce::Time::getMillisecondCounter() + juce::uint32 (timeoutMs);
        int ends = 0;

        while (juce::Time::getMillisecondCounter() < deadline && ends < 2)
        {
            if (sock.waitUntilReady (true, 200) <= 0) continue;
            const int n = sock.read (buf, sizeof (buf) - 1, false);
            if (n <= 0) break;
            buf[n] = 0;
            all += juce::String::fromUTF8 (buf, n);
            ends = all.upToLastOccurrenceOf ("END", true, false).isEmpty()
                     ? 0 : countEnds (all);
        }
        sock.close();

        if (all.isEmpty()) { r.error = "conectou mas nao respondeu"; return r; }

        for (const auto& line : juce::StringArray::fromLines (all))
        {
            const auto l = line.trim();
            if (l.startsWith ("VER"))
            {
                r.device  = field (l, "DEVN");
                r.version = field (l, "SYSV");
            }
            else if (l.startsWith ("SRC "))
            {
                Source s;
                s.number   = l.fromFirstOccurrenceOf ("SRC ", false, false).getIntValue();
                s.name     = field (l, "PSNM");
                s.address  = field (l, "RTPA");
                s.channels = juce::jmax (1, field (l, "NCHN").getIntValue());
                s.livewireChannel = channelFromAddress (s.address);
                if (s.address.isNotEmpty()) r.sources.push_back (s);
            }
            else if (l.startsWith ("DST "))
            {
                Destination d;
                d.number   = l.fromFirstOccurrenceOf ("DST ", false, false).getIntValue();
                d.name     = field (l, "NAME");
                d.address  = field (l, "ADDR");
                d.channels = field (l, "NCHN").getIntValue();
                r.destinations.push_back (d);
            }
        }

        r.ok = ! r.sources.empty() || ! r.destinations.empty();
        if (! r.ok) r.error = "respondeu, mas sem fontes nem destinos";
        return r;
    }

private:
    static int countEnds (const juce::String& s)
    {
        int n = 0, i = 0;
        while ((i = s.indexOf (i, "END")) >= 0) { ++n; i += 3; }
        return n;
    }

    /** Extrai CHAVE:"valor" ou CHAVE:valor de uma linha. */
    static juce::String field (const juce::String& line, const juce::String& key)
    {
        const int at = line.indexOf (key + ":");
        if (at < 0) return {};
        auto rest = line.substring (at + key.length() + 1);
        if (rest.startsWithChar ('"'))
            return rest.substring (1).upToFirstOccurrenceOf ("\"", false, false);
        return rest.upToFirstOccurrenceOf (" ", false, false);
    }
};
