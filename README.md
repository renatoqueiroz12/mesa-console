# Mesa Console — motor de audio (v0.1)

Primeira engrenagem: **ASIO in -> canal (meter, auto trim, trim, fader, on/off, cue) ->
buses PGM 1-4 -> master -> ASIO out**, com 12 canais.

## Estrutura

```
Source/Core/    <- sem JUCE, C++17 puro, compila e testa em qualquer lugar
  DspUtil.h       conversao dB, ganho suavizado
  Meter.h         pico e RMS com balistica, publicados por atomic
  AutoTrim.h      ganho automatico estrutural (nao e plugin)
  Channel.h       o strip; expoe os 4 taps para o Audio Trigger (fase 7)
  MixerEngine.h   soma nos buses, pan de potencia constante, master, cue
  Dsp.h           EQ 3 bandas, compressor, gate, de-esser, limiter
  DspRack.h       cadeia por canal, ordem trocada por swap atomico (sem lock)
  TriggerEngine.h maquina de estados do Audio Trigger por canal
  AutomationEngine.h  dominancia, plano minimo, hold, camera geral, fila de comandos
  AsyncSource.h   fonte de rede (NDI/AES67) com fila SPSC e contagem de underrun
  Json.h          leitor/escritor JSON minimo, sem dependencias
  Scene.h         CENA (show profile): capturar, aplicar, salvar e carregar
  Settings.h      configuracoes da instalacao, incluindo perfis de acesso

Source/App/     <- JUCE, so aqui
  AudioEngine.h     AudioIODeviceCallback + AudioDeviceManager (ASIO)
  StripComponent.h  strip da UI: fader, ON sobre OFF, CUE, meter
  MainComponent.h   janela + seletor de dispositivo + estatisticas
  Main.cpp

tests/test_core.cpp   14 testes do core
```

## Compilar e rodar os testes do core (qualquer SO, sem JUCE)

```
g++ -std=c++17 -O2 tests/test_core.cpp -o test_core && ./test_core
```

## Compilar o app com ASIO (Windows)

1. Baixe o ASIO SDK no site da Steinberg (exige aceitar o contrato de licenca —
   por isso ele nao pode ser versionado junto com o projeto).
2. Descompacte, por exemplo, em `C:\SDKs\asiosdk`.
3. Configure e compile:

```
cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk"
cmake --build build --config Release
```

Sem `ASIO_SDK_DIR` o app ainda compila, mas so enxerga WASAPI/DirectSound —
util para desenvolver a UI no Mac/Linux.

## O que ja funciona (94 testes)

- Abertura do dispositivo, escolha de driver, sample rate e buffer pela propria janela
- 12 canais mono roteados dos 12 primeiros canais de entrada
- Fader (-60 a +10 dB) com suavizacao, ON/OFF com rampa, CUE pre-fader
- Meter de pico e RMS por canal e no master
- Auto trim por canal (desligado por padrao; ligar por
  `channel(i).params.autoTrim.enabled.store(true)`)
- PGM 1 -> saidas 0/1, PGM 2 -> 2/3, PGM 3 -> 4/5, PGM 4 -> 6/7
- Leitura de latencia real do driver e carga de CPU do callback
- Rack de DSP por canal: gate, EQ, compressor, de-esser e limiter, ordem configuravel
- Mix-minus por fader: telefone e codec ouvem o programa menos eles mesmos
- Audio Trigger com threshold, permanencia, histerese, hold, release e cooldown
- Automation Engine com dominancia entre canais, plano minimo e camera geral
- Fila de comandos sem lock: o audio enfileira, a rede consome
- Perfis de acesso (operador, produtor, engenheiro) com area por area
- **Cenas**: captura completa do console em JSON legivel (canais, buses, pan, auto trim,
  parametros de trigger e camera, automacao global), com regra de seguranca — canal no ar
  nao troca de fonte ao carregar a cena, fica pendente

## Regras que o codigo respeita (e que precisam continuar valendo)

- `process()` nao aloca, nao trava, nao registra log e nao chama nada do SO
- Toda memoria e alocada em `prepare()`, chamado em `audioDeviceAboutToStart`
- Parametros atravessam a fronteira UI -> audio so por `std::atomic`
- Ganhos sempre suavizados: nunca um degrau de ganho no meio do bloco

## O que ainda depende de SDK externo (nao da para testar aqui)

1. **Receptor NDI** — usar `NDIlib_framesync`: ele resolve a diferenca de clock
   entre a maquina de origem e a placa. A fila (`AsyncSource`) ja esta pronta.
2. **Host VST3** — entra por `IDspProcessor`, mesma interface dos internos.
   Scan em processo separado.
3. **vMix Adapter** — consumir `AutomationEngine::commands` numa thread de rede
   e mandar pela API TCP (porta 8099). Nunca dentro do callback de audio.

## Proximo passo pratico

Medir: LatencyMon na maquina alvo, depois buffer 64/128/256 e a carga real do callback.

## Validacao da automacao — as tres etapas

O erro comum e ligar a deteccao de audio antes de saber se o resto funciona.
A ordem certa separa os problemas:

**1. O comando sai?** `AutomationEngine::testFire(mix, canal)` enfileira um CUT sem
depender de audio nenhum. Ignora dominancia, permanencia e plano minimo — e teste,
nao producao. O comando vem marcado com `manual = true`, entao o log distingue o que
foi disparado pelo operador do que foi decidido pela deteccao.
`testCamera(mix, n)` faz o mesmo direto por camera, util para varrer todas de uma vez.

**2. Chega audio no canal?** `Channel::presence` responde sem opinar sobre threshold:
sinal presente, tempo de silencio, pico retido, contagem de clipping e alarme de canal
que tinha sinal e emudeceu. Cabo solto e mic no mute aparecem aqui, nao no trigger.

**3. O threshold esta certo para ESTE estudio?** `Channel::calibrator` escuta alguns
segundos, separa o piso de ruido do nivel de fala e sugere um threshold entre os dois,
com 8 dB de folga de cada lado. Melhor do que chutar -35 dBFS: o mesmo microfone em
salas diferentes pede numeros diferentes.

So depois disso vale ligar `trigger.enabled` e olhar o log de rejeicoes.
