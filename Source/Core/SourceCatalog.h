#pragma once
#include "Channel.h"
#include "MixerEngine.h"
#include "Scene.h"
#include "Json.h"
#include <string>
#include <vector>

namespace mesa {

/** Uma FONTE de audio, como entidade propria.

    Ate aqui, "fonte" era so um punhado de campos dentro do fader. Isso
    contradizia a regra que o projeto ja tinha assumido: parametros de trigger
    pertencem a FONTE, nao ao fader. Um microfone tem o mesmo threshold e a
    mesma camera esteja ele no fader 1 ou no 7.

    SourceDef guarda tudo que viaja COM a fonte. O fader guarda so o que e
    dele: posicao, ON/OFF, bus, CUE. Carregar uma fonte num fader copia estes
    valores para os atomics do canal — o audio segue lendo atomic, como sempre. */
struct SourceDef
{
    std::string name;                    // "MIC 1 APRES", "PLAYOUT A", "TEL VX 1"
    int  kind = int (InputKind::Device); // placa ASIO | rede
    int  index = -1;                     // entrada da placa, quando kind = Device
    /** Slot da segunda ponta, quando a fonte e ESTEREO. -1 = mono. Como o
        index, vale so em execucao: quem manda e o que a rede entregou agora. */
    int  indexR = -1;
    std::string streamName;              // nome NDI exato, quando vem por NDI
    /** Placa SECUNDARIA: gravamos o NOME do dispositivo, nao so o indice da
        entrada. Com duas placas, "entrada 3" e ambiguo, e um settings.json
        levado para outra maquina apontaria para o lugar errado. */
    std::string deviceName;
    std::string deviceType;              // "ASIO" ou "Windows Audio"
    int  deviceChannel = 0;
    /** Canal Livewire (1..32767). Vira multicast por conta: 239.192.y.z com
        canal = y*256+z. Zero significa que esta fonte nao usa Livewire. */
    int  livewireChannel = 0;
    /** Nome da fonte como anunciado. Guardado junto do numero porque e o nome
        que sobrevive: IP muda, canal pode ser remapeado, nome fica. Tambem
        evita que a lista mostre "(fora da lista)" antes da primeira varredura. */
    std::string livewireNome;
    /** 0 = esquerdo, 1 = direito, 2 = soma (L+R)/2 em mono, 3 = ESTEREO.

        Um stream Livewire e sempre estereo. Como o canal desta mesa e mono com
        pan, alguem tem de decidir o que fazer com as duas pontas. Em 3 o canal
        passa a carregar as duas de verdade, cada uma no seu lado do
        barramento, e o pan vira balanco. */
    int  livewireSide = 0;

    /** GPIO: qual porta e pino do no acompanham o ON/OFF deste canal.
        Zero desliga. E o que acende a luz de ar e fecha o rele. */
    int  gpoPorta = 0, gpoPino = 0;
    /** Entrada que liga e desliga o canal de fora — botao de mesa, pedal. */
    int  gpiPorta = 0, gpiPino = 0;

    /** Transcrever a fala deste input. */
    bool transcrever = false;
    float falaThresholdDb = -42.0f;
    float falaMaxTrechoMs = 12000.0f;
    int  type = int (SourceType::Line);  // define mute de monitor e mix-minus

    float trimDb = 0.0f;
    float atrasoMs = 0.0f;           // sync com video, por fonte
    bool  onVaiParaZero = false;     // ligar o canal poe o fader em 0 dB

    /** CANAL ESTEREO com duas entradas da placa.

        A placa entrega as entradas uma a uma, mas linha e estereo: um toca-
        discos, um playout, um retorno de PC ocupam dois conectores. Sem isto
        sobrava carregar os dois em faders separados e casar fader, pan e ON de
        cada um na mao - trabalhoso e facil de errar no ar.

        As duas entradas sao escolhidas SEPARADAMENTE, e nao "esta e a
        seguinte". Placa nao garante que o par de um aparelho caia em conectores
        vizinhos, e quem remaneja cabo no rack nao deveria ter de respeitar uma
        regra nossa de numeracao.

        Ao contrario do indexR das fontes de rede, que e atribuido a cada
        religamento, estes sao ESCOLHA e ficam gravados: 3 e 4 continuam sendo
        3 e 4 amanha. */
    bool  deviceEstereo = false;
    /** Entrada da DIREITA na placa. -1 = nao escolhida. A esquerda e o index. */
    int   deviceIndexR = -1;

    // trigger: pertence a fonte
    bool  triggerEnabled = false;
    int   triggerTap     = 0;
    int   camera         = 0;
    float thresholdDb    = -35.0f;
    float triggerMs      = 250.0f;
    float hysteresisDb   = 6.0f;
    float holdMs         = 1200.0f;
    float releaseMs      = 400.0f;
    float cooldownMs     = 1500.0f;
    std::string command;
    int   triggerTarget = 0;

    // logica de fader-start: tambem pertence a fonte
    bool logicEnabled = false;
    std::string onCommand, offCommand;
    int  logicTarget = 0;

    // mix-minus
    int   feedBusOn  = 0;
    int   feedBusOff = 3;
    int   feedOutputPair = -1;
    float feedDimDb = -10.0f;
};

/** Catalogo: existe independente dos faders. Uma fonte cadastrada continua
    cadastrada mesmo que nenhum fader esteja usando. */
struct SourceCatalog
{
    std::vector<SourceDef> sources;

    SourceDef* find (const std::string& n)
    {
        for (auto& s : sources) if (s.name == n) return &s;
        return nullptr;
    }

    int indexOf (const std::string& n) const
    {
        for (size_t i = 0; i < sources.size(); ++i)
            if (sources[i].name == n) return int (i);
        return -1;
    }

    bool add (const SourceDef& s)
    {
        if (s.name.empty() || find (s.name) != nullptr) return false;
        sources.push_back (s);
        return true;
    }

    bool remove (const std::string& n)
    {
        for (size_t i = 0; i < sources.size(); ++i)
            if (sources[i].name == n) { sources.erase (sources.begin() + long (i)); return true; }
        return false;
    }
};

/** Carrega a fonte no fader. NAO mexe em fader, ON/OFF, bus nem CUE — isso e
    do operador, nao da fonte. */
/** Chave que identifica a FILA de uma fonte, para reaproveita-la entre
    religamentos em vez de recriar receptor a cada mudanca de catalogo. */
inline std::string chaveDaFonte (const SourceDef& s)
{
    if (s.livewireChannel > 0)
        return "lw:" + std::to_string (s.livewireChannel) + ":"
             + std::to_string (s.livewireSide);
    if (! s.streamName.empty()) return "ndi:" + s.streamName;
    if (! s.deviceName.empty()) return "dev:" + s.deviceName + ":"
                                     + std::to_string (s.deviceChannel);
    return {};
}

/** Comeco de toda chave de um canal Livewire, qualquer que seja o lado.

    Existe por causa de um defeito caro: a limpeza dos receptores listava os
    lados na mao ("...:0", "...:1", "...:2"). Quando o lado 3 (estereo) entrou,
    a chave dele nao batia com nenhum, o receptor recem-criado era destruido
    como se estivesse sobrando, e o motor ficava apontando para memoria
    liberada - a mesa caia no primeiro bloco de audio. Comparar pelo PREFIXO
    nao depende de alguem lembrar de atualizar duas listas. */
inline std::string prefixoLivewire (int canal)
{
    return "lw:" + std::to_string (canal) + ":";
}

/** A chave pertence a este canal Livewire? Vale para qualquer lado e para
    sufixos como "/R", usado pela segunda ponta de uma fonte estereo. */
inline bool chaveEhDoCanal (const std::string& chave, int canal)
{
    const std::string pref = prefixoLivewire (canal);
    return chave.size() >= pref.size()
        && chave.compare (0, pref.size(), pref) == 0;
}

inline void loadSource (const SourceDef& s, Channel& ch)
{
    ch.name = s.name;
    ch.params.inputKind .store (s.kind);
    ch.params.inputIndex .store (s.index);   // slot assincrono quando kind = Network
    // Em placa, a segunda ponta e o canal seguinte e sai da escolha gravada;
    // em rede, vem do slot que o religamento atribuiu.
    ch.params.inputIndexR.store (
        (s.kind == int (InputKind::Device) && s.deviceEstereo)
            ? s.deviceIndexR : s.indexR);
    ch.params.sourceType.store (s.type);
    ch.params.trimDb    .store (s.trimDb);
    ch.params.atrasoMs  .store (s.atrasoMs);
    ch.params.onVaiParaZero.store (s.onVaiParaZero);

    ch.params.trigger.enabled     .store (s.triggerEnabled);
    ch.params.trigger.source      .store (s.triggerTap);
    ch.params.trigger.camera      .store (s.camera);
    ch.params.trigger.thresholdDb .store (s.thresholdDb);
    ch.params.trigger.triggerMs   .store (s.triggerMs);
    ch.params.trigger.hysteresisDb.store (s.hysteresisDb);
    ch.params.trigger.holdMs      .store (s.holdMs);
    ch.params.trigger.releaseMs   .store (s.releaseMs);
    ch.params.trigger.cooldownMs  .store (s.cooldownMs);
    ch.params.trigger.command.set (s.command);
    ch.params.trigger.target      .store (s.triggerTarget);

    ch.params.logicEnabled.store (s.logicEnabled);
    ch.params.onCommand .set (s.onCommand);
    ch.params.offCommand.set (s.offCommand);
    ch.params.logicTarget.store (s.logicTarget);

    ch.params.feedBusOn     .store (s.feedBusOn);
    ch.params.feedBusOff    .store (s.feedBusOff);
    ch.params.feedOutputPair.store (s.feedOutputPair);
    ch.params.feedDimDb     .store (s.feedDimDb);
    ch.params.fala.enabled    .store (s.transcrever);
    ch.params.fala.thresholdDb.store (s.falaThresholdDb);
    ch.params.fala.maxTrechoMs.store (s.falaMaxTrechoMs);
}

/** Le de volta do fader para a fonte: serve para "salvar ajustes na fonte"
    depois de calibrar o threshold ouvindo. */
inline void captureSource (const Channel& ch, SourceDef& s)
{
    s.name       = ch.name;
    s.kind       = ch.params.inputKind .load();
    s.index      = ch.params.inputIndex.load();
    s.type       = ch.params.sourceType.load();
    s.trimDb     = ch.params.trimDb    .load();
    s.atrasoMs   = ch.params.atrasoMs  .load();
    s.onVaiParaZero = ch.params.onVaiParaZero.load();

    s.triggerEnabled = ch.params.trigger.enabled     .load();
    s.triggerTap     = ch.params.trigger.source      .load();
    s.camera         = ch.params.trigger.camera      .load();
    s.thresholdDb    = ch.params.trigger.thresholdDb .load();
    s.triggerMs      = ch.params.trigger.triggerMs   .load();
    s.hysteresisDb   = ch.params.trigger.hysteresisDb.load();
    s.holdMs         = ch.params.trigger.holdMs      .load();
    s.releaseMs      = ch.params.trigger.releaseMs   .load();
    s.cooldownMs     = ch.params.trigger.cooldownMs  .load();
    s.command        = ch.params.trigger.command.str();
    s.triggerTarget  = ch.params.trigger.target      .load();

    s.logicEnabled = ch.params.logicEnabled.load();
    s.onCommand    = ch.params.onCommand .str();
    s.offCommand   = ch.params.offCommand.str();
    s.logicTarget  = ch.params.logicTarget.load();

    s.feedBusOn      = ch.params.feedBusOn     .load();
    s.feedBusOff     = ch.params.feedBusOff    .load();
    s.feedOutputPair = ch.params.feedOutputPair.load();
    s.feedDimDb      = ch.params.feedDimDb     .load();
    s.transcrever    = ch.params.fala.enabled    .load();
    s.falaThresholdDb= ch.params.fala.thresholdDb.load();
    s.falaMaxTrechoMs= ch.params.fala.maxTrechoMs.load();
}

// ------------------------------------------------------------------ saidas

/** Um DESTINO de audio: para onde um bus, o CUE ou a monitoracao vai.
    Mesma ideia do canal de entrada, do outro lado da mesa. */
struct OutputDef
{
    std::string name;                    // "PGM 1", "GRAVACAO", "STREAM"
    int  kind = int (InputKind::Device); // placa | rede
    int  pair = -1;                      // par de saida da placa; -1 = nao roteado
    std::string streamName;              // nome do emissor NDI, quando for NDI
    std::string deviceName;              // placa secundaria
    std::string deviceType;              // "ASIO" ou "Windows Audio"
    /** Canal Livewire que a mesa TRANSMITE. Zero = nao transmite. */
    int  livewireChannel = 0;
    int  busSource = 0;                  // 0..3 = PGM 1..4, 4 = CUE, 5 = monitor, 6 = fone

    /** DIRECT OUT: manda uma ENTRADA direto para este destino, sem passar por
        fader nem por barramento. Vazio = desligado, e o destino segue o
        busSource.

        Guarda o NOME da fonte no catalogo, nao o indice: indice de fonte de
        rede e reatribuido a cada religamento, e destino que troca de fonte
        sozinho manda audio errado ao ar.

        E de ENTRADA, nao de fader, de proposito. O direct out de uma mesa sai
        do conector e existe mesmo que nenhum fader tenha aquela fonte
        carregada. Serve para gravar cada microfone em separado, alimentar
        processador externo, ou repassar uma fonte adiante pela rede: entra
        Livewire numa entrada e sai Livewire noutro canal, direto.

        O sinal e cru (antes de trim, DSP, fader e ON/OFF) e MONO, que e a
        natureza de uma entrada aqui. Em destino estereo vai igual nos dois
        lados, sem pan: pan e coisa de mistura, e direct out existe justamente
        para escapar dela. */
    std::string directSource;

    /** MIX-MINUS: manda para este destino o retorno de um CANAL - o programa
        menos a propria voz daquele canal. -1 = desligado.

        Aqui e por CANAL, e nao por entrada como o direct out, porque mix-minus
        depende do ON/OFF: e o estado do fader que decide se o retorno leva o
        programa ou a comunicacao. Sem fader nao existe essa pergunta.

        Com isto o retorno deixa de depender do par de saida da placa
        principal: da para mandar o mix-minus de um telefone por Livewire, que
        e como um hibrido em rede Axia recebe o dele. */
    int mixMinusChannel = -1;
    /** Ganho DESTA saida, em dB.

        Cada destino casa com um equipamento diferente: o transmissor quer um
        nivel, o gravador quer outro, o estudio outro. Um ganho global no
        barramento obrigaria a escolher qual deles fica certo. */
    float ganhoDb = 0.0f;
};

struct OutputCatalog
{
    std::vector<OutputDef> outputs;

    OutputDef* find (const std::string& n)
    {
        for (auto& o : outputs) if (o.name == n) return &o;
        return nullptr;
    }
    bool add (const OutputDef& o)
    {
        if (o.name.empty() || find (o.name) != nullptr) return false;
        outputs.push_back (o);
        return true;
    }
    bool remove (const std::string& n)
    {
        for (size_t i = 0; i < outputs.size(); ++i)
            if (outputs[i].name == n) { outputs.erase (outputs.begin() + long (i)); return true; }
        return false;
    }
};

// ------------------------------------------------------------------ paginas

/** Uma pagina: mapa de POSICAO do fader para NOME de fonte.
    Vazio numa posicao = fader sem fonte naquela pagina. */
struct FaderPage
{
    std::string name;                 // "Jornal", "Esportes", "Madrugada"
    std::vector<std::string> slots;   // por posicao de fader

    /** A PAGINA GUARDA TUDO, nao so quem fica em cada fader.

        Uma pagina so com os nomes das fontes resolvia metade do problema: o
        operador chegava na pagina certa e ainda tinha de reajustar fader, bus,
        trim e DSP de cada canal na mao. Guardando a cena inteira, escolher a
        pagina devolve a mesa como ela estava quando aquilo funcionou.

        Continua valendo a regra que importa: canal NO AR nao troca de fonte
        nem anda de fader. Rearrumar a mesa nunca pode calar quem esta falando,
        e por isso a cena e aplicada com respeito ao ar. */
    bool  guardaTudo = false;
    Scene cena;

    /** Quando foi gravada, em texto ja formatado.

        Guardado como texto e nao como instante: a lista quer MOSTRAR, e nao
        calcular. Formato fixo para ordenar por ele direto se um dia precisar. */
    std::string gravadaEm;
};

struct PageSet
{
    std::vector<FaderPage> pages;
    int active = 0;

    FaderPage* find (const std::string& n)
    {
        for (auto& p : pages) if (p.name == n) return &p;
        return nullptr;
    }
};

/** Aplica a pagina aos faders.

    Canal NO AR nao troca de fonte: fica pendente, exatamente como ja acontece
    ao carregar cena. Tirar alguem do ar no meio da fala porque o operador
    trocou de pagina seria falha grave.

    Devolve quantas posicoes ficaram pendentes. */
inline int applyPage (const FaderPage& page, const SourceCatalog& cat,
                      MixerEngine& mix, std::vector<int>* pendingOut = nullptr)
{
    // ESTADO PRIMEIRO, LAYOUT DEPOIS.
    //
    // Antes, snapshot com estado aplicava a cena e devolvia ali mesmo - os
    // slots nem eram olhados. A tela deixava escolher quem fica em cada fader e
    // aquilo nunca valia: a fonte vinha sempre da cena. Editar um campo que o
    // programa ignora e pior que nao ter o campo.
    //
    // Agora a cena repoe fader, bus, trim e DSP, e em seguida o layout diz
    // quem fica onde - o que o operador escolheu na lista ganha de quem estava
    // ali quando o snapshot foi gravado. As duas regras de recall safe
    // continuam valendo: applyScene pula quem esta no ar, e o laco abaixo
    // tambem.
    if (page.guardaTudo && ! page.cena.channels.empty())
        applyScene (page.cena, mix, true);

    int pending = 0;
    const int n = mix.numChannels();

    for (size_t i = 0; i < page.slots.size() && int (i) < n; ++i)
    {
        auto& ch = mix.channel (int (i));
        const std::string& wanted = page.slots[i];

        if (wanted.empty()) continue;
        if (ch.name == wanted) continue;              // ja e essa fonte

        if (ch.isMicOpen())                              // no ar: nao mexe
        {
            ++pending;
            if (pendingOut != nullptr) pendingOut->push_back (int (i));
            continue;
        }

        for (const auto& s : cat.sources)
            if (s.name == wanted) { loadSource (s, ch); break; }
    }
    return pending;
}

// ---------------------------------------------------------------- json

inline json::Value sourceToJson (const SourceDef& s)
{
    auto o = json::object();
    o.set ("name",           json::text    (s.name));
    o.set ("kind",           json::num     (s.kind));
    o.set ("index",          json::num     (s.index));
    o.set ("stream",         json::text    (s.streamName));
    o.set ("device",         json::text    (s.deviceName));
    o.set ("deviceType",     json::text    (s.deviceType));
    o.set ("lwChannel",      json::num     (s.livewireChannel));
    o.set ("lwNome",         json::text    (s.livewireNome));
    o.set ("lwSide",         json::num     (s.livewireSide));
    o.set ("gpoPorta",       json::num     (s.gpoPorta));
    o.set ("gpoPino",        json::num     (s.gpoPino));
    o.set ("gpiPorta",       json::num     (s.gpiPorta));
    o.set ("gpiPino",        json::num     (s.gpiPino));
    o.set ("transcrever",    json::boolean (s.transcrever));
    o.set ("falaThresholdDb", json::num     (s.falaThresholdDb));
    o.set ("falaMaxMs",       json::num     (s.falaMaxTrechoMs));
    o.set ("deviceChannel",  json::num     (s.deviceChannel));
    o.set ("type",           json::num     (s.type));
    o.set ("trimDb",         json::num     (s.trimDb));
    o.set ("atrasoMs",       json::num     (s.atrasoMs));
    o.set ("onZero",         json::boolean (s.onVaiParaZero));
    o.set ("devEstereo",     json::boolean (s.deviceEstereo));
    o.set ("devIndexR",      json::num     (s.deviceIndexR));
    o.set ("trigEnabled",    json::boolean (s.triggerEnabled));
    o.set ("trigTap",        json::num     (s.triggerTap));
    o.set ("camera",         json::num     (s.camera));
    o.set ("thresholdDb",    json::num     (s.thresholdDb));
    o.set ("triggerMs",      json::num     (s.triggerMs));
    o.set ("hysteresisDb",   json::num     (s.hysteresisDb));
    o.set ("holdMs",         json::num     (s.holdMs));
    o.set ("releaseMs",      json::num     (s.releaseMs));
    o.set ("cooldownMs",     json::num     (s.cooldownMs));
    o.set ("command",        json::text    (s.command));
    o.set ("trigTarget",     json::num     (s.triggerTarget));
    o.set ("logicEnabled",   json::boolean (s.logicEnabled));
    o.set ("onCommand",      json::text    (s.onCommand));
    o.set ("offCommand",     json::text    (s.offCommand));
    o.set ("logicTarget",    json::num     (s.logicTarget));
    o.set ("feedBusOn",      json::num     (s.feedBusOn));
    o.set ("feedBusOff",     json::num     (s.feedBusOff));
    o.set ("feedOutputPair", json::num     (s.feedOutputPair));
    o.set ("feedDimDb",      json::num     (s.feedDimDb));
    return o;
}

inline SourceDef sourceFromJson (const json::Value& o)
{
    SourceDef s;
    s.name           = o.string  ("name");
    s.kind           = int (o.number ("kind", 0));
    s.index          = int (o.number ("index", -1));
    s.streamName     = o.string  ("stream");
    s.deviceName     = o.string  ("device");
    s.deviceType     = o.string  ("deviceType");
    s.livewireChannel = int (o.number ("lwChannel", 0));
    s.livewireNome    = o.string ("lwNome");
    s.livewireSide    = int (o.number ("lwSide", 0));
    s.gpoPorta        = int (o.number ("gpoPorta", 0));
    s.gpoPino         = int (o.number ("gpoPino", 0));
    s.gpiPorta        = int (o.number ("gpiPorta", 0));
    s.gpiPino         = int (o.number ("gpiPino", 0));
    s.transcrever     = o.boolean ("transcrever", false);
    s.falaThresholdDb = float (o.number ("falaThresholdDb", -42.0));
    s.falaMaxTrechoMs = float (o.number ("falaMaxMs", 12000.0));
    s.deviceChannel  = int (o.number ("deviceChannel", 0));
    s.type           = int (o.number ("type", double (int (SourceType::Line))));
    s.trimDb         = float (o.number ("trimDb", 0.0));
    s.atrasoMs       = float (o.number ("atrasoMs", 0.0));
    s.onVaiParaZero  = o.boolean ("onZero", false);
    s.deviceEstereo  = o.boolean ("devEstereo", false);
    s.deviceIndexR   = int (o.number ("devIndexR", -1));
    s.triggerEnabled = o.boolean ("trigEnabled", false);
    s.triggerTap     = int (o.number ("trigTap", 0));
    s.camera         = int (o.number ("camera", 0));
    s.thresholdDb    = float (o.number ("thresholdDb", -35.0));
    s.triggerMs      = float (o.number ("triggerMs", 250.0));
    s.hysteresisDb   = float (o.number ("hysteresisDb", 6.0));
    s.holdMs         = float (o.number ("holdMs", 1200.0));
    s.releaseMs      = float (o.number ("releaseMs", 400.0));
    s.cooldownMs     = float (o.number ("cooldownMs", 1500.0));
    s.command        = o.string  ("command");
    s.triggerTarget  = int (o.number ("trigTarget", 0));
    s.logicEnabled   = o.boolean ("logicEnabled", false);
    s.onCommand      = o.string  ("onCommand");
    s.offCommand     = o.string  ("offCommand");
    s.logicTarget    = int (o.number ("logicTarget", 0));
    const int feedAntigo = int (o.number ("feedSource", -1));   // catalogo de antes do par
    s.feedBusOn      = int (o.number ("feedBusOn",
                                      feedAntigo >= 1 ? double (feedAntigo - 1) : 0.0));
    s.feedBusOff     = int (o.number ("feedBusOff", 3));
    s.feedOutputPair = int (o.number ("feedOutputPair", -1));
    s.feedDimDb      = float (o.number ("feedDimDb", -10.0));
    return s;
}

inline json::Value catalogToJson (const SourceCatalog& c)
{
    auto a = json::array();
    for (const auto& s : c.sources) a.arr.push_back (sourceToJson (s));
    return a;
}

inline void catalogFromJson (const json::Value& a, SourceCatalog& c)
{
    c.sources.clear();
    if (a.type != json::Value::Array) return;
    for (const auto& v : a.arr)
        if (v.type == json::Value::Object) c.sources.push_back (sourceFromJson (v));
}

inline json::Value outputsToJson (const OutputCatalog& c)
{
    auto a = json::array();
    for (const auto& o : c.outputs)
    {
        auto v = json::object();
        v.set ("name",   json::text (o.name));
        v.set ("kind",   json::num  (o.kind));
        v.set ("pair",   json::num  (o.pair));
        v.set ("ganhoDb", json::num  (o.ganhoDb));
        v.set ("stream", json::text (o.streamName));
        v.set ("device", json::text (o.deviceName));
        v.set ("deviceType", json::text (o.deviceType));
        v.set ("lwChannel", json::num (o.livewireChannel));
        v.set ("bus",    json::num  (o.busSource));
        v.set ("directSource", json::text (o.directSource));
        v.set ("mixMinus",     json::num  (o.mixMinusChannel));
        a.arr.push_back (v);
    }
    return a;
}

inline void outputsFromJson (const json::Value& a, OutputCatalog& c)
{
    c.outputs.clear();
    if (a.type != json::Value::Array) return;
    for (const auto& v : a.arr)
    {
        if (v.type != json::Value::Object) continue;
        OutputDef o;
        o.name       = v.string ("name");
        o.kind       = int (v.number ("kind", 0));
        o.pair       = int (v.number ("pair", -1));
        o.ganhoDb    = float (v.number ("ganhoDb", 0.0));
        o.streamName = v.string ("stream");
        o.deviceName = v.string ("device");
        o.deviceType = v.string ("deviceType");
        o.livewireChannel = int (v.number ("lwChannel", 0));
        o.busSource  = int (v.number ("bus", 0));
        o.directSource  = v.string ("directSource");
        o.mixMinusChannel = int (v.number ("mixMinus", -1));
        c.outputs.push_back (o);
    }
}

inline json::Value pagesToJson (const PageSet& ps)
{
    auto a = json::array();
    for (const auto& p : ps.pages)
    {
        auto o = json::object();
        o.set ("name", json::text (p.name));
        auto slots = json::array();
        for (const auto& sl : p.slots) slots.arr.push_back (json::text (sl));
        o.set ("slots", slots);
        o.set ("guardaTudo", json::boolean (p.guardaTudo));
        o.set ("gravadaEm",  json::text (p.gravadaEm));
        if (p.guardaTudo) o.set ("cena", sceneToValue (p.cena));
        a.arr.push_back (o);
    }
    return a;
}

inline void pagesFromJson (const json::Value& a, PageSet& ps)
{
    ps.pages.clear();
    if (a.type != json::Value::Array) return;
    for (const auto& v : a.arr)
    {
        if (v.type != json::Value::Object) continue;
        FaderPage p;
        p.name = v.string ("name");
        if (auto* sl = v.find ("slots"))
            if (sl->type == json::Value::Array)
                for (const auto& e : sl->arr) p.slots.push_back (e.str);

        p.guardaTudo = v.boolean ("guardaTudo", false);
        p.gravadaEm  = v.string ("gravadaEm");
        if (auto* c = v.find ("cena")) sceneFromValue (*c, p.cena);
        ps.pages.push_back (p);
    }
}

} // namespace mesa
