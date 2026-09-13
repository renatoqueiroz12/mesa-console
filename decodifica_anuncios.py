#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Decodifica os anuncios Livewire capturados em anuncios.txt.

Uso:
    python decodifica_anuncios.py %APPDATA%\\MesaConsole\\anuncios.txt
    python decodifica_anuncios.py anuncios.txt --no 2          # so o pacote 2
    python decodifica_anuncios.py anuncios.txt --resumo        # so a tabela de nos
    python decodifica_anuncios.py anuncios.txt --compara 1 4   # dois pacotes lado a lado

Por que existe: o anuncios.txt e hexadecimal cru. Para saber o que o driver da
Axia manda e nos nao, o olho nao basta — precisa da arvore de campos. O formato
lido aqui e o mesmo que LivewireAdvertiser.h escreve:

    03 00 02 07 | contador (4) | zeros (8) | frases [4 letras][tipo][valor]

Tipos: 00 e 07 = 1 byte; 08 = 2; 01 = 4; 09 = 8; 03 = texto (2 de tamanho);
       06 = bloco aninhado (2 de tamanho).
"""

import re
import sys
from collections import OrderedDict

CABECALHO = bytes([0x03, 0x00, 0x02, 0x07])

TAMANHO_FIXO = {0x00: 1, 0x07: 1, 0x08: 2, 0x01: 4, 0x09: 8}


class Erro(Exception):
    pass


# --------------------------------------------------------------------- leitura

def le_pacotes(texto):
    """Extrai os pacotes do despejo. Aceita o formato do anuncios.txt
    ("[n] N bytes" + linhas de hex + linha de texto) e tambem hex solto."""
    pacotes = []
    atual = None

    for linha in texto.splitlines():
        cabeca = re.match(r"^\[(\d+)\]\s+(\d+)\s+bytes", linha.strip())
        if cabeca:
            if atual is not None:
                pacotes.append(atual)
            atual = {"n": int(cabeca.group(1)),
                     "tamanho": int(cabeca.group(2)),
                     "bytes": bytearray()}
            continue

        if atual is None:
            continue

        # so aceita linhas que sejam pares hexadecimais separados por espaco;
        # a linha de texto legivel do despejo cai fora por causa dos pontos
        pedacos = linha.split()
        if pedacos and all(re.fullmatch(r"[0-9a-fA-F]{2}", p) for p in pedacos):
            if len(atual["bytes"]) < atual["tamanho"]:
                atual["bytes"].extend(int(p, 16) for p in pedacos)

    if atual is not None:
        pacotes.append(atual)

    for p in pacotes:
        p["bytes"] = bytes(p["bytes"][: p["tamanho"]])
    return pacotes


# ------------------------------------------------------------------ decodifica

def nome_legivel(quatro):
    if all(32 <= c < 127 for c in quatro):
        return quatro.decode("ascii")
    return "0x" + quatro.hex()


def le_frases(b, ini, fim, profundidade=0):
    """Devolve lista de (nome, tipo, valor, filhos)."""
    itens = []
    i = ini
    while i < fim:
        if i + 5 > fim:
            itens.append(("<sobra>", None, b[i:fim], []))
            break

        nome = nome_legivel(b[i:i + 4])
        tipo = b[i + 4]
        i += 5

        if tipo in TAMANHO_FIXO:
            n = TAMANHO_FIXO[tipo]
            if i + n > fim:
                raise Erro("valor cortado em %s" % nome)
            itens.append((nome, tipo, b[i:i + n], []))
            i += n

        elif tipo == 0x03:                       # texto
            if i + 2 > fim:
                raise Erro("texto sem tamanho em %s" % nome)
            n = int.from_bytes(b[i:i + 2], "big")
            i += 2
            if i + n > fim:
                raise Erro("texto cortado em %s" % nome)
            itens.append((nome, tipo, b[i:i + n], []))
            i += n

        elif tipo == 0x06:                       # bloco
            if i + 2 > fim:
                raise Erro("bloco sem tamanho em %s" % nome)
            n = int.from_bytes(b[i:i + 2], "big")
            i += 2
            if i + n > fim:
                raise Erro("bloco %s promete %d bytes e so ha %d"
                           % (nome, n, fim - i))
            itens.append((nome, tipo, None,
                          le_frases(b, i, i + n, profundidade + 1)))
            i += n

        else:
            raise Erro("tipo desconhecido 0x%02x depois de %s (posicao %d)"
                       % (tipo, nome, i - 1))

    return itens


def decodifica(b):
    if len(b) < 16:
        raise Erro("pacote curto demais (%d bytes)" % len(b))
    if b[:4] != CABECALHO:
        raise Erro("cabecalho inesperado %s" % b[:4].hex())
    contador = int.from_bytes(b[4:8], "big")
    zeros = b[8:16]
    return {"contador": contador,
            "zeros_ok": zeros == bytes(8),
            "itens": le_frases(b, 16, len(b))}


# ----------------------------------------------------------------- apresentar

def mostra_valor(nome, tipo, v):
    if tipo == 0x03:
        texto = v.split(b"\x00")[0].decode("latin-1", "replace")
        return '"%s" (%d bytes, %d de texto)' % (texto, len(v), len(texto))
    n = int.from_bytes(v, "big")
    if nome == "INIP":
        return "%d.%d.%d.%d" % tuple(v)
    if nome in ("FSID", "BSID"):
        return "0x%08x  (canal %d)" % (n, n & 0xFFFF)
    if nome == "HWID":
        return "0x%04x  (%d)" % (n, n)
    if nome == "UDPC":
        return "%d" % n
    if len(v) <= 4:
        return "%d  (0x%0*x)" % (n, len(v) * 2, n)
    return "0x%s" % v.hex()


def imprime(itens, recuo=0):
    for nome, tipo, valor, filhos in itens:
        if tipo == 0x06:
            print("%s%s:" % ("  " * recuo, nome))
            imprime(filhos, recuo + 1)
        elif tipo is None:
            print("%s%s %s" % ("  " * recuo, nome, valor.hex()))
        else:
            print("%s%-6s %s" % ("  " * recuo, nome, mostra_valor(nome, tipo, valor)))


# ------------------------------------------------------------------- resumo

def acha(itens, nome):
    """Primeira ocorrencia de um campo, em qualquer profundidade."""
    for n, tipo, valor, filhos in itens:
        if n == nome and tipo != 0x06:
            return valor
        achado = acha(filhos, nome)
        if achado is not None:
            return achado
    return None


def campos_planos(itens, prefixo=""):
    """Todos os campos folha como caminho -> valor, para comparar pacotes."""
    saida = OrderedDict()
    for nome, tipo, valor, filhos in itens:
        if tipo == 0x06:
            saida.update(campos_planos(filhos, prefixo + nome + "/"))
        elif tipo is not None:
            saida[prefixo + nome] = (tipo, valor)
    return saida


def resume(pacotes):
    linhas = []
    for p in pacotes:
        try:
            d = decodifica(p["bytes"])
        except Erro as e:
            linhas.append((p["n"], "?", "?", "?", "ERRO: %s" % e))
            continue

        advt = acha(d["itens"], "ADVT")
        hwid = acha(d["itens"], "HWID")
        inip = acha(d["itens"], "INIP")
        atrn = acha(d["itens"], "ATRN")
        nums = acha(d["itens"], "NUMS")

        fontes = []
        for nome, tipo, valor, filhos in d["itens"]:
            if tipo == 0x06 and nome.startswith("S"):
                psid = acha(filhos, "PSID")
                psnm = acha(filhos, "PSNM")
                if psid is not None:
                    rotulo = str(int.from_bytes(psid, "big"))
                    if psnm is not None:
                        rotulo += ":" + psnm.split(b"\x00")[0].decode("latin-1", "replace")
                    fontes.append(rotulo)

        linhas.append((
            p["n"],
            int.from_bytes(advt, "big") if advt else "-",
            "0x%04x" % int.from_bytes(hwid, "big") if hwid else "-",
            "%d.%d.%d.%d" % tuple(inip) if inip else "-",
            "%s  n=%s  %s" % (
                atrn.split(b"\x00")[0].decode("latin-1", "replace") if atrn else "",
                int.from_bytes(nums, "big") if nums else "-",
                ", ".join(fontes[:6]) + (" ..." if len(fontes) > 6 else "")),
        ))

    print("%-4s %-5s %-8s %-15s %s" % ("pkt", "ADVT", "HWID", "INIP", "no / fontes"))
    print("-" * 100)
    for l in linhas:
        print("%-4s %-5s %-8s %-15s %s" % l)

    nos = {}
    for p in pacotes:
        try:
            d = decodifica(p["bytes"])
        except Erro:
            continue
        hwid = acha(d["itens"], "HWID")
        inip = acha(d["itens"], "INIP")
        chave = ("0x%04x" % int.from_bytes(hwid, "big") if hwid else "-",
                 "%d.%d.%d.%d" % tuple(inip) if inip else "-")
        advt = acha(d["itens"], "ADVT")
        nos.setdefault(chave, set()).add(int.from_bytes(advt, "big") if advt else 0)

    print("\nNOS VISTOS (HWID + INIP e o que identifica o no para o console)")
    for (hwid, inip), tipos in sorted(nos.items()):
        print("  HWID %s  INIP %-15s  ADVT %s" % (hwid, inip, sorted(tipos)))
    if len(nos) != len({k[0] for k in nos}):
        print("  !! dois nos com o mesmo HWID: o console vai juntar os dois")


def compara(pacotes, numeros):
    """Poe dois pacotes lado a lado, campo a campo.

    Driver e mesa compartilham HWID nesta rede, entao nao da para separa-los
    sozinho: diga os numeros. Rode --resumo antes para escolher quais."""
    if len(numeros) != 2:
        print("uso: --compara N M   (dois numeros de pacote; veja --resumo)")
        return

    escolhidos = []
    for n in numeros:
        achado = next((p for p in pacotes if p["n"] == n), None)
        if achado is None:
            print("pacote [%d] nao esta no arquivo" % n)
            return
        escolhidos.append(achado)

    lados = []
    for p in escolhidos:
        try:
            lados.append((p["n"], campos_planos(decodifica(p["bytes"])["itens"])))
        except Erro as e:
            print("pacote [%d] nao decodificou: %s" % (p["n"], e))
            return

    todos = OrderedDict()
    for _, campos in lados:
        for k in campos:
            todos[k] = True

    largura = max(len(k) for k in todos) + 2
    print("%-*s%s" % (largura, "campo",
                      "".join("%-28s" % ("pacote [%d]" % n) for n, _ in lados)))
    print("-" * (largura + 28 * len(lados)))
    for k in todos:
        celulas, vistos = [], []
        for _, campos in lados:
            if k not in campos:
                celulas.append("%-28s" % "-- ausente --")
                vistos.append(None)
            else:
                tipo, v = campos[k]
                celulas.append("%-28s" % mostra_valor(k.split("/")[-1], tipo, v)[:27])
                vistos.append(v)
        marca = "" if len(set(vistos)) == 1 else "  <<"
        print("%-*s%s%s" % (largura, k, "".join(celulas), marca))
    print("\n<< marca os campos que diferem")


# --------------------------------------------------------------------- entrada

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    caminho = argv[1]
    with open(caminho, "r", encoding="latin-1") as f:
        pacotes = le_pacotes(f.read())

    if not pacotes:
        print("nenhum pacote encontrado em %s" % caminho)
        return 1

    if "--resumo" in argv:
        resume(pacotes)
        return 0
    if "--compara" in argv:
        resto = argv[argv.index("--compara") + 1:]
        compara(pacotes, [int(x) for x in resto if x.isdigit()])
        return 0

    escolhido = None
    if "--no" in argv:
        escolhido = int(argv[argv.index("--no") + 1])

    for p in pacotes:
        if escolhido is not None and p["n"] != escolhido:
            continue
        print("=" * 72)
        print("pacote [%d] — %d bytes" % (p["n"], p["tamanho"]))
        try:
            d = decodifica(p["bytes"])
        except Erro as e:
            print("  NAO DECODIFICOU: %s" % e)
            print("  %s" % p["bytes"][:64].hex())
            continue
        print("contador %d   zeros %s" % (d["contador"],
                                          "ok" if d["zeros_ok"] else "DIFERENTES"))
        imprime(d["itens"], 0)
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
