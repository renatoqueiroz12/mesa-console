#pragma once
#include "Channel.h"
#include "MixerEngine.h"
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
    std::string streamName;              // nome NDI exato, quando vem por NDI
    /** Placa SECUNDARIA: gravamos o NOME do dispositivo, nao so o indice da
        entrada. Com duas placas, "entrada 3" e ambiguo, e um settings.json
        levado para outra maquina apontaria para o lugar errado. */
    std::string deviceName;
    std::string deviceType;              // "ASIO" ou "Windows Audio"
    int  deviceChannel = 0;
    int  type = int (SourceType::Line);  // define mute de monitor e mix-minus

    float trimDb = 0.0f;

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
    int   feedSource = 0;
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
inline void loadSource (const SourceDef& s, Channel& ch)
{
    ch.name = s.name;
    ch.params.inputKind .store (s.kind);
    ch.params.inputIndex.store (s.index);   // slot assincrono quando kind = Network
    ch.params.sourceType.store (s.type);
    ch.params.trimDb    .store (s.trimDb);

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

    ch.params.feedSource    .store (s.feedSource);
    ch.params.feedOutputPair.store (s.feedOutputPair);
    ch.params.feedDimDb     .store (s.feedDimDb);
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

    s.feedSource     = ch.params.feedSource    .load();
    s.feedOutputPair = ch.params.feedOutputPair.load();
    s.feedDimDb      = ch.params.feedDimDb     .load();
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
    int  busSource = 0;                  // 0..3 = PGM 1..4, 4 = CUE, 5 = monitor, 6 = fone
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
    o.set ("deviceChannel",  json::num     (s.deviceChannel));
    o.set ("type",           json::num     (s.type));
    o.set ("trimDb",         json::num     (s.trimDb));
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
    o.set ("feedSource",     json::num     (s.feedSource));
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
    s.deviceChannel  = int (o.number ("deviceChannel", 0));
    s.type           = int (o.number ("type", double (int (SourceType::Line))));
    s.trimDb         = float (o.number ("trimDb", 0.0));
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
    s.feedSource     = int (o.number ("feedSource", 0));
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
        v.set ("stream", json::text (o.streamName));
        v.set ("device", json::text (o.deviceName));
        v.set ("deviceType", json::text (o.deviceType));
        v.set ("bus",    json::num  (o.busSource));
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
        o.streamName = v.string ("stream");
        o.deviceName = v.string ("device");
        o.deviceType = v.string ("deviceType");
        o.busSource  = int (v.number ("bus", 0));
        c.outputs.push_back (o);
    }
}

/** Aplica os destinos ao motor: por enquanto so os de placa: o envio NDI
    depende do NetworkSink, que ainda nao esta ligado ao mixer. */
inline void applyOutputs (const OutputCatalog& c, MixerEngine& mix)
{
    for (const auto& o : c.outputs)
    {
        if (o.kind != int (InputKind::Device)) continue;
        switch (o.busSource)
        {
            case 0: case 1: case 2: case 3:
                mix.busParams[size_t (o.busSource)].outputPair.store (o.pair); break;
            case 4: mix.monitor.cuePair    .store (o.pair); break;
            case 5: mix.monitor.monitorPair.store (o.pair); break;
            case 6: mix.monitor.phonesPair .store (o.pair); break;
            case 7: mix.monitor.studioPair .store (o.pair); break;
            default: break;
        }
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
        ps.pages.push_back (p);
    }
}

} // namespace mesa
