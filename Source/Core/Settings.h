#pragma once
#include "Json.h"
#include "MixerEngine.h"
#include "SourceCatalog.h"
#include <vector>
#include <fstream>

namespace mesa {

/** CONFIGURACOES DA INSTALACAO — nao pertencem a cena.
    Trocar de show nao muda dispositivo, roteamento nem endereco do vMix. */

constexpr int kSettingsFormatVersion = 1;

struct DeviceSettings
{
    std::string driverType = "ASIO";     // ASIO | WASAPI | CoreAudio
    std::string deviceName;
    double sampleRate = 48000.0;
    int    bufferSize = 128;
    int    numInputs  = 16;
    int    numOutputs = 8;
    int    channelCount = 12;            // strips criados
};

struct RoutingSettings
{
    int busOutputPair[kNumBuses] = { 0, 1, 2, 3 };   // -1 = nao roteado
    int cueOutputPair     = -1;
    int monitorOutputPair = -1;   // monitor do controle
    int phonesOutputPair  = -1;   // fone do operador
    int studioOutputPair  = -1;   // monitor do estudio
    int ext1InputPair     = -1;   // fontes externas do seletor de monitor
    int ext2InputPair     = -1;
    float masterGainDb = 0.0f;
};

/** Como a superficie e desenhada. Pertence a instalacao, nao a cena. */
struct SurfaceSettings
{
    int  fadersPerLayer = 8;
    int  layers         = 2;
    std::string meterMode = "PPM";   // PPM | VU
    float trimRangeDb   = 25.0f;
    float faderTopDb    = 10.0f;
};

struct VmixSettings
{
    bool        enabled  = true;
    std::string host     = "127.0.0.1";
    int         port     = 8099;         // API TCP; HTTP seria 8088
    std::string protocol = "TCP";
    int         timeoutMs = 500;
};

/** Fontes de rede declaradas na instalacao. O nome e o do emissor NDI
    (ex.: "CARTUCHEIRA (Player 1)") ou o stream AES67. */
struct NetworkSource
{
    std::string kind = "NDI";      // NDI | AES67 | Local
    std::string name;              // nome do emissor: "MAQUINA (Player 1)"
    std::string machine;           // maquina de origem, so para diagnostico
    int  channels     = 2;
    bool audioOnly    = true;
    int  bufferBlocks = 8;         // profundidade da fila, em blocos de audio

    /** Para onde o console manda START/STOP quando o canal abre e fecha
        (fader-start). Vazio = sem controle remoto. */
    std::string controlUrl;
    bool sendStartStop = false;
};

/** Rede: descoberta e identificacao desta maquina. */
/** Para onde um comando e enviado. A mesa nao presume vMix: cada destino tem
    protocolo, host e porta. Cartucheira por UDP e vMix por TCP convivem. */
struct CommandTarget
{
    std::string name;                 // rotulo que aparece na tela
    std::string protocol = "TCP";     // TCP | UDP | HTTP
    std::string host = "127.0.0.1";
    int  port = 8099;
    bool appendNewline = true;        // a API TCP do vMix espera fim de linha
};

struct NetworkSettings
{
    std::string machineName;       // nome com que este console aparece na rede
    std::string discoveryServer;   // necessario quando as maquinas nao dividem a sub-rede
    bool preferMulticast = false;
};

struct DspSettings
{
    bool        scanOutOfProcess = true;  // plugin que trava nao derruba o console
    std::string vst3Path;
};

/** Perfil de acesso: o que cada operador pode mexer no menu do canal.
    O PIN aqui e de operacao, nao de seguranca — evita engano, nao ataque. */
struct AccessProfile
{
    std::string name;
    bool source = false, gain = true, buses = false, dsp = false, trigger = false;
};

/** Usuario da mesa. O PIN aqui e de OPERACAO, nao de seguranca:
    serve para impedir engano, nao para resistir a ataque. Fica em texto no
    arquivo de configuracao — quem tem acesso ao disco tem acesso ao PIN. */
struct User
{
    std::string name;
    std::string profile = "Operador";
    std::string pin;
    bool enabled = true;
};

struct AccessSettings
{
    std::vector<AccessProfile> profiles;
    std::vector<User> users;
    std::string active = "Operador";    // perfil em vigor
    std::string activeUser;             // quem esta logado (vazio = ninguem)
    std::string engineerPin = "1234";
    bool requireLogin = false;          // se true, sem login vale o perfil mais restrito

    static AccessSettings defaults()
    {
        AccessSettings a;
        a.profiles.push_back ({ "Operador",   false, true, false, false, false });
        a.profiles.push_back ({ "Produtor",   true,  true, true,  false, true  });
        a.profiles.push_back ({ "Engenheiro", true,  true, true,  true,  true  });
        a.users.push_back ({ "operador", "Operador",   "",     true });
        a.users.push_back ({ "tecnico",  "Engenheiro", "1234", true });
        return a;
    }

    User* findUser (const std::string& n)
    {
        for (auto& u : users) if (u.name == n) return &u;
        return nullptr;
    }

    /** Entra com um usuario. PIN vazio = entrada livre (util para o operador). */
    bool login (const std::string& name, const std::string& pin)
    {
        auto* u = findUser (name);
        if (u == nullptr || ! u->enabled) return false;
        if (! u->pin.empty() && u->pin != pin) return false;
        activeUser = u->name;
        active     = u->profile;
        return true;
    }

    void logout()
    {
        activeUser.clear();
        active = users.empty() ? "Operador" : users.front().profile;
    }

    bool addUser (const User& u)
    {
        if (u.name.empty() || findUser (u.name) != nullptr) return false;
        if (find (u.profile) == nullptr) return false;      // perfil precisa existir
        users.push_back (u);
        return true;
    }

    bool removeUser (const std::string& n)
    {
        for (size_t i = 0; i < users.size(); ++i)
            if (users[i].name == n)
            {
                if (users.size() == 1) return false;        // nunca fica sem ninguem
                if (activeUser == n) logout();
                users.erase (users.begin() + long (i));
                return true;
            }
        return false;
    }

    const AccessProfile* find (const std::string& n) const
    {
        for (auto& p : profiles) if (p.name == n) return &p;
        return nullptr;
    }
    bool can (const std::string& area) const
    {
        auto* p = find (active);
        if (p == nullptr) return false;
        if (area == "source")  return p->source;
        if (area == "gain")    return p->gain;
        if (area == "buses")   return p->buses;
        if (area == "dsp")     return p->dsp;
        if (area == "trigger") return p->trigger;
        return false;
    }
};

struct Settings
{
    /** Fontes e paginas sao INSTALACAO, nao show: a fiacao e os streams de rede
        nao mudam quando troca o programa. */
    SourceCatalog catalog;
    OutputCatalog outputs;

    /** Recepcao de comandos externos (cartucheira, playout, automacao). */
    bool remoteEnabled   = true;
    int  remoteUdpPort   = 8890;
    int  remoteTcpPort   = 8890;
    /** ON externo poe o fader em 0 dB se ele estiver abaixo. E o pedido tipico
        do playout: "entra no ar em nivel", sem depender de onde o fader ficou. */
    bool remoteOnSetsFader = true;
    float remoteOnFaderDb  = 0.0f;

    /** 0 = minima, 1 = equilibrada, 2 = segura. Controla a profundidade das
        filas das placas secundarias — ou seja, a latencia delas. */
    int secondaryLatencyMode = 0;
    PageSet       pages;

    AccessSettings access = AccessSettings::defaults();
    std::vector<CommandTarget> targets = {
        { "vMix",        "TCP", "127.0.0.1", 8099, true  },
        { "Cartucheira", "UDP", "127.0.0.1", 8889, false }
    };
    std::vector<NetworkSource> networkSources;
    NetworkSettings network;
    DeviceSettings  device;
    SurfaceSettings surface;
    RoutingSettings routing;
    VmixSettings    vmix;
    DspSettings     dsp;
};

/** Aplica ao engine o que e aplicavel em tempo real (roteamento e master). */
inline void applyRouting (const Settings& s, MixerEngine& mix)
{
    for (int b = 0; b < kNumBuses; ++b)
        mix.busParams[b].outputPair.store (s.routing.busOutputPair[b]);
    mix.monitor.cuePair    .store (s.routing.cueOutputPair);
    mix.monitor.monitorPair.store (s.routing.monitorOutputPair);
    mix.monitor.phonesPair .store (s.routing.phonesOutputPair);
    mix.monitor.studioPair .store (s.routing.studioOutputPair);
    mix.monitor.ext1Pair   .store (s.routing.ext1InputPair);
    mix.monitor.ext2Pair   .store (s.routing.ext2InputPair);
    mix.masterGainDb.store (s.routing.masterGainDb);
}

inline std::string settingsToJson (const Settings& s)
{
    using namespace json;
    auto root = object();
    root.set ("format", num (kSettingsFormatVersion));

    auto d = object();
    d.set ("driverType",   text (s.device.driverType));
    d.set ("deviceName",   text (s.device.deviceName));
    d.set ("sampleRate",   num (s.device.sampleRate));
    d.set ("bufferSize",   num (s.device.bufferSize));
    d.set ("numInputs",    num (s.device.numInputs));
    d.set ("numOutputs",   num (s.device.numOutputs));
    d.set ("channelCount", num (s.device.channelCount));
    root.set ("device", d);

    auto r = object();
    auto pairs = array();
    for (int b = 0; b < kNumBuses; ++b) pairs.arr.push_back (num (s.routing.busOutputPair[b]));
    r.set ("busOutputPair", pairs);
    r.set ("cueOutputPair",     num (s.routing.cueOutputPair));
    r.set ("monitorOutputPair", num (s.routing.monitorOutputPair));
    r.set ("phonesOutputPair",  num (s.routing.phonesOutputPair));
    r.set ("studioOutputPair",  num (s.routing.studioOutputPair));
    r.set ("ext1InputPair",     num (s.routing.ext1InputPair));
    r.set ("ext2InputPair",     num (s.routing.ext2InputPair));
    r.set ("masterGainDb",      num (s.routing.masterGainDb));
    root.set ("routing", r);

    auto sf = object();
    sf.set ("fadersPerLayer", num (s.surface.fadersPerLayer));
    sf.set ("layers",         num (s.surface.layers));
    sf.set ("meterMode",      text (s.surface.meterMode));
    sf.set ("trimRangeDb",    num (s.surface.trimRangeDb));
    sf.set ("faderTopDb",     num (s.surface.faderTopDb));
    root.set ("surface", sf);

    auto v = object();
    v.set ("enabled",  boolean (s.vmix.enabled));
    v.set ("host",     text (s.vmix.host));
    v.set ("port",     num (s.vmix.port));
    v.set ("protocol", text (s.vmix.protocol));
    v.set ("timeoutMs",num (s.vmix.timeoutMs));
    root.set ("vmix", v);

    auto ns = array();
    for (const auto& n : s.networkSources)
    {
        auto o = object();
        o.set ("kind",         text (n.kind));
        o.set ("name",         text (n.name));
        o.set ("channels",     num (n.channels));
        o.set ("audioOnly",    boolean (n.audioOnly));
        o.set ("bufferBlocks",  num (n.bufferBlocks));
        o.set ("machine",       text (n.machine));
        o.set ("controlUrl",    text (n.controlUrl));
        o.set ("sendStartStop", boolean (n.sendStartStop));
        ns.arr.push_back (o);
    }
    root.set ("networkSources", ns);

    auto ac = object();
    ac.set ("active",       text (s.access.active));
    ac.set ("activeUser",   text (s.access.activeUser));
    ac.set ("engineerPin",  text (s.access.engineerPin));
    ac.set ("requireLogin", boolean (s.access.requireLogin));
    auto us = array();
    for (const auto& u : s.access.users)
    {
        auto o = object();
        o.set ("name",    text (u.name));
        o.set ("profile", text (u.profile));
        o.set ("pin",     text (u.pin));
        o.set ("enabled", boolean (u.enabled));
        us.arr.push_back (o);
    }
    ac.set ("users", us);
    auto profs = array();
    for (const auto& pr : s.access.profiles)
    {
        auto o = object();
        o.set ("name",    text (pr.name));
        o.set ("source",  boolean (pr.source));
        o.set ("gain",    boolean (pr.gain));
        o.set ("buses",   boolean (pr.buses));
        o.set ("dsp",     boolean (pr.dsp));
        o.set ("trigger", boolean (pr.trigger));
        profs.arr.push_back (o);
    }
    ac.set ("profiles", profs);
    root.set ("access", ac);

    auto tg = array();
    for (const auto& t : s.targets)
    {
        auto o = object();
        o.set ("name",          text (t.name));
        o.set ("protocol",      text (t.protocol));
        o.set ("host",          text (t.host));
        o.set ("port",          num (t.port));
        o.set ("appendNewline", boolean (t.appendNewline));
        tg.arr.push_back (o);
    }
    root.set ("targets", tg);

    auto net = object();
    net.set ("machineName",     text (s.network.machineName));
    net.set ("discoveryServer", text (s.network.discoveryServer));
    net.set ("preferMulticast", boolean (s.network.preferMulticast));
    root.set ("network", net);

    auto p = object();
    p.set ("scanOutOfProcess", boolean (s.dsp.scanOutOfProcess));
    p.set ("vst3Path",         text (s.dsp.vst3Path));
    root.set ("dsp", p);

    root.set ("remoteEnabled",   boolean (s.remoteEnabled));
    root.set ("remoteUdpPort",   num (s.remoteUdpPort));
    root.set ("remoteTcpPort",   num (s.remoteTcpPort));
    root.set ("remoteOnSetsFader", boolean (s.remoteOnSetsFader));
    root.set ("remoteOnFaderDb", num (s.remoteOnFaderDb));
    root.set ("secondaryLatency", num (s.secondaryLatencyMode));
    root.set ("catalog", catalogToJson (s.catalog));
    root.set ("outputs", outputsToJson (s.outputs));
    auto pg = object();
    pg.set ("active", num (s.pages.active));
    pg.set ("list",   pagesToJson (s.pages));
    root.set ("pages", pg);

    return toString (root);
}

inline bool settingsFromJson (const std::string& src, Settings& out)
{
    json::Value root;
    if (! json::parse (src, root) || root.type != json::Value::Object) return false;
    if (int (root.number ("format", 0)) > kSettingsFormatVersion) return false;

    out = Settings{};

    if (auto* d = root.find ("device"))
    {
        out.device.driverType   = d->string ("driverType", "ASIO");
        out.device.deviceName   = d->string ("deviceName");
        out.device.sampleRate   = d->number ("sampleRate", 48000.0);
        out.device.bufferSize   = int (d->number ("bufferSize", 128));
        out.device.numInputs    = int (d->number ("numInputs", 16));
        out.device.numOutputs   = int (d->number ("numOutputs", 8));
        out.device.channelCount = int (d->number ("channelCount", 12));
    }
    if (auto* r = root.find ("routing"))
    {
        if (auto* pairs = r->find ("busOutputPair"))
            for (int b = 0; b < kNumBuses && size_t (b) < pairs->arr.size(); ++b)
                out.routing.busOutputPair[b] = int (pairs->arr[size_t (b)].num);
        out.routing.cueOutputPair     = int (r->number ("cueOutputPair", -1));
        out.routing.monitorOutputPair = int (r->number ("monitorOutputPair", -1));
        out.routing.phonesOutputPair  = int (r->number ("phonesOutputPair", -1));
        out.routing.studioOutputPair  = int (r->number ("studioOutputPair", -1));
        out.routing.ext1InputPair     = int (r->number ("ext1InputPair", -1));
        out.routing.ext2InputPair     = int (r->number ("ext2InputPair", -1));
        out.routing.masterGainDb      = float (r->number ("masterGainDb", 0.0));
    }
    if (auto* sf = root.find ("surface"))
    {
        out.surface.fadersPerLayer = int (sf->number ("fadersPerLayer", 8));
        out.surface.layers         = int (sf->number ("layers", 2));
        out.surface.meterMode      = sf->string ("meterMode", "PPM");
        out.surface.trimRangeDb    = float (sf->number ("trimRangeDb", 25.0));
        out.surface.faderTopDb     = float (sf->number ("faderTopDb", 10.0));
    }
    if (auto* v = root.find ("vmix"))
    {
        out.vmix.enabled   = v->boolean ("enabled", true);
        out.vmix.host      = v->string ("host", "127.0.0.1");
        out.vmix.port      = int (v->number ("port", 8099));
        out.vmix.protocol  = v->string ("protocol", "TCP");
        out.vmix.timeoutMs = int (v->number ("timeoutMs", 500));
    }
    if (auto* ns = root.find ("networkSources"))
        for (const auto& o : ns->arr)
        {
            NetworkSource n;
            n.kind         = o.string ("kind", "NDI");
            n.name         = o.string ("name");
            n.channels     = int (o.number ("channels", 2));
            n.audioOnly    = o.boolean ("audioOnly", true);
            n.bufferBlocks  = int (o.number ("bufferBlocks", 8));
            n.machine       = o.string ("machine");
            n.controlUrl    = o.string ("controlUrl");
            n.sendStartStop = o.boolean ("sendStartStop", false);
            out.networkSources.push_back (n);
        }

    if (auto* ac = root.find ("access"))
    {
        out.access.active       = ac->string ("active", "Operador");
        out.access.activeUser   = ac->string ("activeUser");
        out.access.engineerPin  = ac->string ("engineerPin", "1234");
        out.access.requireLogin = ac->boolean ("requireLogin", false);
        if (auto* us = ac->find ("users"))
        {
            out.access.users.clear();
            for (const auto& o : us->arr)
            {
                User u;
                u.name    = o.string ("name");
                u.profile = o.string ("profile", "Operador");
                u.pin     = o.string ("pin");
                u.enabled = o.boolean ("enabled", true);
                out.access.users.push_back (u);
            }
        }
        if (auto* profs = ac->find ("profiles"))
        {
            out.access.profiles.clear();
            for (const auto& o : profs->arr)
            {
                AccessProfile pr;
                pr.name    = o.string ("name");
                pr.source  = o.boolean ("source");
                pr.gain    = o.boolean ("gain", true);
                pr.buses   = o.boolean ("buses");
                pr.dsp     = o.boolean ("dsp");
                pr.trigger = o.boolean ("trigger");
                out.access.profiles.push_back (pr);
            }
        }
    }

    if (auto* tg = root.find ("targets"))
    {
        out.targets.clear();
        for (const auto& o : tg->arr)
        {
            CommandTarget t;
            t.name          = o.string ("name");
            t.protocol      = o.string ("protocol", "TCP");
            t.host          = o.string ("host", "127.0.0.1");
            t.port          = int (o.number ("port", 8099));
            t.appendNewline = o.boolean ("appendNewline", true);
            out.targets.push_back (t);
        }
    }

    if (auto* net = root.find ("network"))
    {
        out.network.machineName     = net->string ("machineName");
        out.network.discoveryServer = net->string ("discoveryServer");
        out.network.preferMulticast = net->boolean ("preferMulticast", false);
    }

    if (auto* p = root.find ("dsp"))
    {
        out.dsp.scanOutOfProcess = p->boolean ("scanOutOfProcess", true);
        out.dsp.vst3Path         = p->string ("vst3Path");
    }
    out.remoteEnabled     = root.boolean ("remoteEnabled", true);
    out.remoteUdpPort     = int (root.number ("remoteUdpPort", 8890));
    out.remoteTcpPort     = int (root.number ("remoteTcpPort", 8890));
    out.remoteOnSetsFader = root.boolean ("remoteOnSetsFader", true);
    out.remoteOnFaderDb   = float (root.number ("remoteOnFaderDb", 0.0));
    out.secondaryLatencyMode = int (root.number ("secondaryLatency", 0));
    if (auto* c = root.find ("catalog")) catalogFromJson (*c, out.catalog);
    if (auto* o = root.find ("outputs")) outputsFromJson (*o, out.outputs);
    if (auto* pg = root.find ("pages"))
    {
        out.pages.active = int (pg->number ("active", 0));
        if (auto* l = pg->find ("list")) pagesFromJson (*l, out.pages);
    }

    return true;
}

inline bool saveSettings (const Settings& s, const std::string& path)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    f << settingsToJson (s);
    return f.good();
}

inline bool loadSettings (const std::string& path, Settings& out)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) return false;
    std::string src ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    return settingsFromJson (src, out);
}

} // namespace mesa
