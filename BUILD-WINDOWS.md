# Gerar o .exe no Windows

## O que instalar (uma vez)

1. **Visual Studio 2022 Community** — na instalacao marque
   *"Desenvolvimento para desktop com C++"*. E o compilador; a IDE voce nem precisa abrir.
2. **CMake** — https://cmake.org/download, marque *Add CMake to PATH*.
3. **Git** — https://git-scm.com (o CMake baixa o JUCE por ele).
4. **ASIO SDK** — https://www.steinberg.net/developers/ (exige aceitar a licenca,
   por isso nao pode vir junto do projeto). Descompacte em `C:\SDKs\asiosdk`.

## Primeira compilacao — SEM ASIO

Comece assim para separar problema de compilacao de problema de driver:

```
cd caminho\para\mesa
cmake -B build
cmake --build build --config Release
```

O executavel sai em `build\MesaConsole_artefacts\Release\Mesa Console.exe`.
Ele abre com WASAPI. Se rodar, o codigo esta bom.

## Segunda compilacao — COM ASIO

```
cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk"
cmake --build build --config Release
```

Agora a lista de dispositivos mostra o driver ASIO da placa.

## O que olhar na primeira execucao

Na barra de cima: dispositivo, sample rate, buffer, **latencia real**, **carga do callback**,
camera no ar e a contagem de comandos enviados. Sao esses numeros que decidem o resto.

Sequencia sugerida:

1. Rodar o **LatencyMon** por 15 min antes de tudo. Se a maquina nao passar, nenhum
   ajuste de buffer resolve.
2. Abrir o app, escolher a placa, buffer 256. Confirmar que passa audio.
3. Descer para 128 e depois 64, olhando a carga do callback. Parar antes de estalar.
4. Botao *Disparar teste no canal 1* — o painel da direita mostra o que saiu pela rede.

## settings.json

Criado ao lado do .exe na primeira execucao. E onde ficam dispositivo, roteamento,
destinos de comando (vMix TCP 8099, cartucheira UDP 8889), perfis e usuarios.
Editar com o app fechado.

## Se a compilacao falhar

Provavel na primeira vez — o codigo do app nunca foi compilado, so o core (158 testes).
Mande a primeira mensagem de erro do log: as seguintes costumam ser consequencia dela.
