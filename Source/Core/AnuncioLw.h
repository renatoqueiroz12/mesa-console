#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/** Anuncio Livewire: montar e LER, sem JUCE.

    Por que sair de LivewireAdvertiser.h e vir para o core: enquanto os bytes
    do anuncio nasciam dentro do socket, nenhum teste os via. Errar um tamanho
    de bloco ou um NEST so aparecia com o QOR na frente, e a rede fica longe.
    Aqui o pacote e um vetor de bytes que a suite confere — e o mesmo leitor
    que confere o NOSSO pacote le o do driver da Axia, que e o exemplar a
    copiar.

    Formato (decifrado por captura, nao publicado):

        03 00 02 07          inicio
        4 bytes              contador, sobe a cada mensagem
        8 bytes              zeros
        frases: [4 letras][tipo][valor]

    Tipos: 00 e 07 levam 1 byte; 08 leva 2; 01 leva 4; 09 leva 8; 03 leva 2 de
    tamanho e depois o texto; 06 abre um bloco com 2 de tamanho.

    Regras descobertas: NEST conta os itens do nivel de cima depois dele
    proprio; INDI conta os itens do bloco; FSID = 0xEFC00000 + canal. */
namespace mesa {
namespace lw {

struct FonteAnuncio
{
    int canal = 0;
    std::string nome;
    /** Oito bytes. Tudo zero = livre. Vem do ADVT 3. */
    std::uint64_t uso = 0;
};

struct Identidade
{
    /** Identificador do NO. NAO derive do IP: numa maquina que tambem roda o
        IP-Driver da Axia, os dois anunciam do mesmo IP, o console ve um no so
        e fica com o ultimo anuncio — o nosso apagava as fontes do driver. */
    unsigned hwid = 0;
    unsigned ip = 0;
    /** Porta de controle que o no publica. O driver usa 4000; dividir a porta
        com ele na mesma maquina e pedir para o console falar com o vizinho. */
    unsigned udpc = 4000;
    std::string maquina;
};

// --------------------------------------------------------------- escrita

inline void poe (std::vector<std::uint8_t>& v, const char* quatro)
{
    for (int i = 0; i < 4; ++i) v.push_back (std::uint8_t (quatro[i]));
}
inline void poe8 (std::vector<std::uint8_t>& v, unsigned x)
{
    v.push_back (std::uint8_t (x & 0xff));
}
inline void poe16 (std::vector<std::uint8_t>& v, unsigned x)
{
    v.push_back (std::uint8_t ((x >> 8) & 0xff));
    v.push_back (std::uint8_t (x & 0xff));
}
inline void poe32 (std::vector<std::uint8_t>& v, unsigned x)
{
    v.push_back (std::uint8_t ((x >> 24) & 0xff));
    v.push_back (std::uint8_t ((x >> 16) & 0xff));
    v.push_back (std::uint8_t ((x >> 8) & 0xff));
    v.push_back (std::uint8_t (x & 0xff));
}

/** Texto de tamanho fixo. Guarda o ultimo byte para o zero: nome que enche o
    campo inteiro deixa o leitor do outro lado sem onde parar. */
inline void poeTexto (std::vector<std::uint8_t>& v, const std::string& t, int tam)
{
    poe16 (v, unsigned (tam));
    const int cabe = tam - 1;
    for (int i = 0; i < tam; ++i)
        v.push_back (std::uint8_t (i < cabe && i < int (t.size()) ? t[size_t (i)] : 0));
}

/** Nome do bloco de fonte: S001, S002... Tres digitos, que e o que o driver
    usa; acima de 999 fontes o formato nao comporta e nem faz sentido. */
inline std::string marcaDeFonte (size_t indice)
{
    const unsigned n = unsigned ((indice + 1) % 1000);
    std::string s = "S";
    s += char ('0' + (n / 100) % 10);
    s += char ('0' + (n / 10) % 10);
    s += char ('0' + n % 10);
    return s;
}

inline std::vector<std::uint8_t> comeco (unsigned sequencia)
{
    std::vector<std::uint8_t> v { 0x03, 0x00, 0x02, 0x07 };
    poe32 (v, sequencia);
    for (int i = 0; i < 8; ++i) v.push_back (0);
    return v;
}

/** ADVT 1: a lista das fontes, com nome e enderecos de stream. */
inline std::vector<std::uint8_t> montaLista (const Identidade& eu,
                                            const std::vector<FonteAnuncio>& fontes,
                                            unsigned sequencia)
{
    auto v = comeco (sequencia);
    poe (v, "NEST"); poe8 (v, 0x00); poe8 (v, unsigned (3 + fontes.size()));
    poe (v, "PVER"); poe8 (v, 0x08); poe16 (v, 2);
    poe (v, "ADVT"); poe8 (v, 0x07); poe8 (v, 0x01);
    poe (v, "TERM"); poe8 (v, 0x06); poe16 (v, 0x54);
    poe (v, "INDI"); poe8 (v, 0x00); poe8 (v, 0x06);
    poe (v, "ADVV"); poe8 (v, 0x01); poe32 (v, 0x3f5);
    poe (v, "HWID"); poe8 (v, 0x08); poe16 (v, eu.hwid);
    poe (v, "INIP"); poe8 (v, 0x01); poe32 (v, eu.ip);
    poe (v, "UDPC"); poe8 (v, 0x08); poe16 (v, eu.udpc);
    poe (v, "NUMS"); poe8 (v, 0x08); poe16 (v, unsigned (fontes.size()));
    poe (v, "ATRN"); poe8 (v, 0x03); poeTexto (v, eu.maquina, 32);

    for (size_t i = 0; i < fontes.size(); ++i)
    {
        const unsigned canal = unsigned (fontes[i].canal);
        poe (v, marcaDeFonte (i).c_str()); poe8 (v, 0x06); poe16 (v, 0x65);
        poe (v, "INDI"); poe8 (v, 0x00); poe8 (v, 0x0b);
        poe (v, "PSID"); poe8 (v, 0x01); poe32 (v, canal);
        poe (v, "SHAB"); poe8 (v, 0x07); poe8 (v, 0x00);
        poe (v, "FSID"); poe8 (v, 0x01); poe32 (v, 0xefc00000u + canal);
        poe (v, "FAST"); poe8 (v, 0x07); poe8 (v, 0x02);
        poe (v, "FASM"); poe8 (v, 0x07); poe8 (v, 0x01);
        poe (v, "BSID"); poe8 (v, 0x01); poe32 (v, 0xefc10000u + canal);
        poe (v, "BAST"); poe8 (v, 0x07); poe8 (v, 0x01);
        poe (v, "BASM"); poe8 (v, 0x07); poe8 (v, 0x00);
        poe (v, "LPID"); poe8 (v, 0x01); poe32 (v, canal);
        poe (v, "STPL"); poe8 (v, 0x07); poe8 (v, 0x00);
        poe (v, "PSNM"); poe8 (v, 0x03); poeTexto (v, fontes[i].nome, 16);
    }
    return v;
}

/** ADVT 2: sinal curto de vida. Diz que o no existe e quantas fontes tem. */
inline std::vector<std::uint8_t> montaVida (const Identidade& eu,
                                           size_t quantasFontes,
                                           unsigned sequencia)
{
    auto v = comeco (sequencia);
    poe (v, "NEST"); poe8 (v, 0x00); poe8 (v, 0x03);
    poe (v, "PVER"); poe8 (v, 0x08); poe16 (v, 2);
    poe (v, "ADVT"); poe8 (v, 0x07); poe8 (v, 0x02);
    poe (v, "TERM"); poe8 (v, 0x06); poe16 (v, 0x2d);
    poe (v, "INDI"); poe8 (v, 0x00); poe8 (v, 0x05);
    poe (v, "ADVV"); poe8 (v, 0x01); poe32 (v, 0x15);
    poe (v, "HWID"); poe8 (v, 0x08); poe16 (v, eu.hwid);
    poe (v, "INIP"); poe8 (v, 0x01); poe32 (v, eu.ip);
    poe (v, "UDPC"); poe8 (v, 0x08); poe16 (v, eu.udpc);
    poe (v, "NUMS"); poe8 (v, 0x08); poe16 (v, unsigned (quantasFontes));
    return v;
}

/** ADVT 3: estado de USO de cada fonte. */
inline std::vector<std::uint8_t> montaUso (const Identidade& eu,
                                          const std::vector<FonteAnuncio>& fontes,
                                          unsigned sequencia)
{
    auto v = comeco (sequencia);
    // 3 do nivel de cima (PVER, ADVT, TERM) + as fontes + as duas marcas de fim
    poe (v, "NEST"); poe8 (v, 0x00); poe8 (v, unsigned (5 + fontes.size()));
    poe (v, "PVER"); poe8 (v, 0x08); poe16 (v, 2);
    poe (v, "ADVT"); poe8 (v, 0x07); poe8 (v, 0x03);
    poe (v, "TERM"); poe8 (v, 0x06); poe16 (v, 0x0d);
    poe (v, "INDI"); poe8 (v, 0x00); poe8 (v, 0x01);
    poe (v, "HWID"); poe8 (v, 0x08); poe16 (v, eu.hwid);

    for (size_t i = 0; i < fontes.size(); ++i)
    {
        poe (v, marcaDeFonte (i).c_str()); poe8 (v, 0x06); poe16 (v, 0x1c);
        poe (v, "INDI"); poe8 (v, 0x00); poe8 (v, 0x02);
        poe (v, "PSID"); poe8 (v, 0x01); poe32 (v, unsigned (fontes[i].canal));
        poe (v, "BUSY"); poe8 (v, 0x09);
        for (int k = 7; k >= 0; --k)
            v.push_back (std::uint8_t ((fontes[i].uso >> (k * 8)) & 0xff));
    }

    for (int k = 0; k < 4; ++k) v.push_back (0xff);
    poe8 (v, 0x09); for (int k = 0; k < 8; ++k) v.push_back (0);
    v.push_back (0xff); v.push_back (0xff); v.push_back (0xff); v.push_back (0xfe);
    poe8 (v, 0x09); for (int k = 0; k < 8; ++k) v.push_back (0);
    return v;
}

// ---------------------------------------------------------------- leitura

struct Anuncio
{
    bool ok = false;
    std::string erro;
    unsigned contador = 0;
    int advt = 0;                  // 1 = lista, 2 = vida, 3 = uso
    unsigned hwid = 0, ip = 0, udpc = 0;
    int nums = -1;
    std::string maquina;
    std::vector<FonteAnuncio> fontes;
    /** Bytes lidos. Se sobrar coisa, o pacote nao foi entendido inteiro. */
    size_t consumidos = 0;
};

namespace detalhe {

struct Cursor
{
    const std::uint8_t* b = nullptr;
    size_t i = 0, fim = 0;
    bool falhou = false;
    std::string erro;

    bool cabe (size_t n) const { return i + n <= fim; }
    void para (const std::string& e) { if (! falhou) { falhou = true; erro = e; } }
};

inline unsigned leN (const std::uint8_t* b, int n)
{
    unsigned x = 0;
    for (int k = 0; k < n; ++k) x = (x << 8) | b[k];
    return x;
}

/** Percorre um nivel. Bloco por bloco, chamando de volta para cada campo. */
template <typename F>
void percorre (Cursor& c, size_t fim, F&& aoAchar, int profundidade = 0)
{
    if (profundidade > 4) { c.para ("aninhamento fundo demais"); return; }

    while (c.i < fim && ! c.falhou)
    {
        if (c.i + 5 > fim) { c.para ("frase cortada"); return; }

        char nome[5] = { char (c.b[c.i]), char (c.b[c.i + 1]),
                         char (c.b[c.i + 2]), char (c.b[c.i + 3]), 0 };
        const std::uint8_t tipo = c.b[c.i + 4];
        c.i += 5;

        int tam = -1;
        switch (tipo)
        {
            case 0x00: case 0x07: tam = 1; break;
            case 0x08:            tam = 2; break;
            case 0x01:            tam = 4; break;
            case 0x09:            tam = 8; break;
            default: break;
        }

        if (tam >= 0)
        {
            if (c.i + size_t (tam) > fim) { c.para (std::string ("valor cortado em ") + nome); return; }
            aoAchar (nome, tipo, c.b + c.i, tam, profundidade);
            c.i += size_t (tam);
        }
        else if (tipo == 0x03 || tipo == 0x06)
        {
            if (c.i + 2 > fim) { c.para (std::string ("tamanho cortado em ") + nome); return; }
            const size_t n = leN (c.b + c.i, 2);
            c.i += 2;
            if (c.i + n > fim) { c.para (std::string ("bloco maior que o pacote em ") + nome); return; }

            if (tipo == 0x03)
            {
                aoAchar (nome, tipo, c.b + c.i, int (n), profundidade);
                c.i += n;
            }
            else
            {
                aoAchar (nome, tipo, c.b + c.i, int (n), profundidade);
                const size_t ate = c.i + n;
                percorre (c, ate, aoAchar, profundidade + 1);
                if (c.falhou) return;
                c.i = ate;                    // blindagem: o bloco manda no tamanho
            }
        }
        else
        {
            char m[64];
            std::snprintf (m, sizeof (m), "tipo desconhecido 0x%02x em %s", tipo, nome);
            c.para (m);
            return;
        }
    }
}

} // namespace detalhe

/** Le um datagrama de anuncio. Nunca joga; devolve ok=false com o motivo. */
inline Anuncio le (const std::uint8_t* dados, size_t n)
{
    Anuncio a;
    if (dados == nullptr || n < 16) { a.erro = "pacote curto demais"; return a; }
    if (! (dados[0] == 0x03 && dados[1] == 0x00 && dados[2] == 0x02 && dados[3] == 0x07))
    { a.erro = "cabecalho inesperado"; return a; }

    a.contador = detalhe::leN (dados + 4, 4);

    detalhe::Cursor c { dados, 16, n, false, {} };
    FonteAnuncio emCurso;
    bool dentroDeFonte = false;

    auto guarda = [&]
    {
        if (dentroDeFonte && emCurso.canal > 0) a.fontes.push_back (emCurso);
        emCurso = FonteAnuncio();
        dentroDeFonte = false;
    };

    detalhe::percorre (c, n, [&] (const char* nome, std::uint8_t tipo,
                                  const std::uint8_t* v, int tam, int prof)
    {
        // um bloco S### no nivel de cima abre uma fonte; fechamos no seguinte
        if (tipo == 0x06 && prof == 0 && nome[0] == 'S')
        {
            guarda();
            dentroDeFonte = true;
            return;
        }
        if (tipo == 0x06) return;

        const std::string campo (nome);
        if      (campo == "ADVT") a.advt = int (detalhe::leN (v, tam));
        else if (campo == "HWID") a.hwid = detalhe::leN (v, tam);
        else if (campo == "INIP") a.ip   = detalhe::leN (v, tam);
        else if (campo == "UDPC") a.udpc = detalhe::leN (v, tam);
        else if (campo == "NUMS") a.nums = int (detalhe::leN (v, tam));
        else if (campo == "ATRN" && tipo == 0x03)
        {
            a.maquina.assign (reinterpret_cast<const char*> (v),
                              size_t (tam));
            a.maquina = a.maquina.substr (0, a.maquina.find ('\0'));
        }
        else if (dentroDeFonte && campo == "PSID") emCurso.canal = int (detalhe::leN (v, tam));
        else if (dentroDeFonte && campo == "BUSY" && tam == 8)
        {
            std::uint64_t u = 0;
            for (int k = 0; k < 8; ++k) u = (u << 8) | v[k];
            emCurso.uso = u;
        }
        else if (dentroDeFonte && campo == "PSNM" && tipo == 0x03)
        {
            emCurso.nome.assign (reinterpret_cast<const char*> (v), size_t (tam));
            emCurso.nome = emCurso.nome.substr (0, emCurso.nome.find ('\0'));
        }
    });

    guarda();

    a.consumidos = c.i;
    if (c.falhou) { a.erro = c.erro; return a; }
    a.ok = true;
    return a;
}

// -------------------------------------------------------------- identidade

/** HWID estavel a partir de um texto (nome da maquina, por exemplo).

    Precisa de tres coisas: nao mudar entre arranques, nao sair do IP — senao
    colide com o driver na mesma maquina — e evitar 0x0000 e 0xffff, que
    parecem reservados. FNV-1a de 32 bits, dobrado para 16. */
inline unsigned hwidDeTexto (const std::string& t)
{
    std::uint32_t h = 2166136261u;
    for (unsigned char c : t) { h ^= c; h *= 16777619u; }
    unsigned curto = unsigned ((h ^ (h >> 16)) & 0xffffu);
    if (curto == 0x0000u) curto = 0x4d45u;       // "ME"
    if (curto == 0xffffu) curto = 0x5341u;       // "SA"
    return curto;
}

/** Sobe de um em um ate sair da lista de ocupados. Os HWIDs vistos na rede
    vem do ouvinte de anuncios: escolher um livre e melhor que sortear. */
inline unsigned hwidLivre (unsigned desejado, const std::vector<unsigned>& ocupados)
{
    auto tomado = [&] (unsigned x)
    {
        for (unsigned o : ocupados) if (o == x) return true;
        return false;
    };
    unsigned x = desejado;
    for (int tentativas = 0; tentativas < 4096; ++tentativas)
    {
        if (x != 0x0000u && x != 0xffffu && ! tomado (x)) return x;
        x = unsigned ((x + 1) & 0xffffu);
    }
    return desejado;
}

} // namespace lw
} // namespace mesa
