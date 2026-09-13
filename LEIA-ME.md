# Mesa Console — estado em 13/09/2026, v1.7.55

Console de áudio em software para rádio, em C++17 com JUCE. Roda no ar.

## Onde está

- Máquina antiga: `C:\Users\Teste\Downloads\mesa-console-v1.6\mesa`
- Máquina nova: `C:\Users\administrator\Downloads\mesa-console` (clonada do GitHub)
- Repositório: github.com/renatoqueiroz12/mesa-console
- Configuração e registros: `%APPDATA%\MesaConsole\`
- Compilar: `cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk" -DNDI_SDK_DIR="C:/Program Files/NDI/NDI 6 SDK"`

## O anúncio — resolvido em v1.7.55

O QOR marcava a fonte da mesa como "Used EW". A causa, achada por captura:

**O console ESCREVE o estado de uso no nó, pela porta que o anúncio publica em
UDPC.** Uma vez por segundo, enquanto a fonte está assinada, ele manda:

    NEST 1
    S001  WRIN 2   PSID 3600   BUSY 00 02 | 02 0a | 00 00 | c0 a8

`0x020a` é o HWID do QOR e `0xc0a8` a outra metade do IP dele (192.168.2.10); o
terceiro par é o fader. O nó tem de devolver esse mesmo valor no BUSY do ADVT 3.
O driver da Axia faz exatamente isso — por isso as fontes dele aparecem com
cadeado e sem "Used EW". A mesa recebia e ignorava, publicava oito zeros, e o
console reescrevia para sempre sem nunca ver a confirmação.

Duas coisas impediam de enxergar isso antes:

- **anunciávamos UDPC 4000**, a porta do IP-Driver: o comando chegava ao
  vizinho. Agora a mesa publica 4002, abre e escuta.
- **o HWID saía do IP**, que mesa e driver dividem: o console via um nó só e
  ficava com o último anúncio, e o nosso apagava as fontes do driver. Agora sai
  do nome da máquina, e `hwidLivre` garante que nunca coincide.

Tudo que chega na porta de controle e não é entendido vai cru para
`%APPDATA%\MesaConsole\anuncios-recebidos.txt`. Mudança de uso vira uma linha
legível. É assim que se descobre o resto do protocolo.

### Ainda em aberto

**ADVV inconsistente.** O driver manda o mesmo valor no ADVT 1 e no ADVT 2 (30
nesta rede). Nós mandamos 1013 no ADVT 1 e 21 no ADVT 2, dois números copiados
de capturas diferentes. Não atrapalha a listagem, mas está errado — mexer só
depois, com uma variável de cada vez.

**A mesa não é servidor LWRP.** Não escuta a porta 93. Todo nó Livewire de
verdade escuta, e é por lá que se pede a lista de fontes de um nó. Numa máquina
com o IP-Driver, quem atende por aquele IP é ele (`DEVN:"lwwd"`), e ele não
conhece os canais da mesa.

## O protocolo de anúncio (decifrado por captura, não publicado)

Multicast 239.192.255.3, porta 4001.

    03 00 02 07        início
    4 bytes            contador, sobe a cada mensagem
    8 bytes            zeros
    frases: [4 letras][tipo][valor]

Tipos: `00` e `07` = 1 byte; `08` = 2 bytes; `01` = 4 bytes; `03` = 2 de
tamanho + texto; `06` = abre bloco com 2 de tamanho; `09` = 8 bytes.

Os bytes estão em `Core/AnuncioLw.h`, que monta e lê, sem JUCE, com a suíte
conferindo tamanho de bloco, NEST, corte de texto e pacote truncado. O
`LivewireAdvertiser.h` só cuida dos sockets. O mesmo leitor decifra o anúncio do
driver e o comando WRIN do console.

Três tipos de mensagem:

- **ADVT 1** — lista as fontes: PSID, FSID, nome em PSNM
- **ADVT 2** — sinal curto de vida, com NUMS
- **ADVT 3** — estado de USO por fonte, blocos BUSY

Regras descobertas: `NEST` = 3 + número de fontes (na ADVT 1);
`INDI` = número de itens do bloco; `FSID` = 0xEFC00000 + canal.

A mesa **captura anúncios** para estudo: botão OUVIR ANÚNCIOS DA REDE grava os
40 primeiros inteiros, em hexadecimal e texto, em
`%APPDATA%\MesaConsole\anuncios.txt`. É a ferramenta para comparar o nosso
anúncio com o do driver.

Para ler a captura: `python decodifica_anuncios.py anuncios.txt --resumo` lista
os nós e acusa HWID repetido; `--compara N M` põe dois pacotes lado a lado e
marca com `<<` o que difere. Foi essa comparação que revelou o BUSY.

## O que já funciona

- Livewire nativo nos dois sentidos, sem driver e sem PTP
- NDI, placas ASIO e Windows, mix-minus, automação de câmera por vMix
- Gravador por canal, CUE com DIM e destino escolhível
- Suíte com 238 testes: `build\Release\test_core.exe`

## Armadilhas já pagas — não repetir

**Placa de rede decide tudo.** Numa máquina com várias placas, o Windows
escolhe a interface errada para multicast. Receptor, transmissor e ouvinte de
anúncios precisam pedir a interface explicitamente (`IP_ADD_MEMBERSHIP` e
`IP_MULTICAST_IF`); o `joinMulticast` do JUCE não aceita interface.

**Trocar a placa exige refazer os sockets.** Eles pedem a interface ao abrir.

**Nunca abrir ASIO no escuro.** Driver de placa ausente quebra dentro do
próprio driver, e a mesa morre no arranque sem escrever no log. Sem escolha
salva, abrir o áudio do Windows.

**O rastro de arranque** (`arranque.log`) marca cada etapa e sobrevive à morte
do processo — foi ele que achou a queda acima.

**A macro `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` remove o construtor
padrão.** Declarar `Classe() = default`.

**Medir antes de ajustar.** O áudio picotado do NDI era uma fração de
milissegundo perdida no arredondamento da dormida (50469 amostras/s onde se
consome 48000). O "nível baixo" era o monitor disputando o par de saída do PGM.
Em ambos, o número apontou; o palpite não teria achado.

**Verificar que a alteração pegou.** Substituições de texto falham em silêncio
quando o trecho procurado não bate — isso já custou várias versões empacotadas
sem a correção dentro.

## Ambiente de teste

- QOR Axia em 192.168.2.10; a máquina nova em 192.168.2.116 (Ethernet 2)
- A máquina tem seis placas de rede — daí a importância de escolher a certa
- Canal que a mesa transmite: 3600. Canais do QOR: 3100, 3101, 3105, 3106
- O IP-Driver da Axia roda na mesma máquina e é o modelo a copiar
