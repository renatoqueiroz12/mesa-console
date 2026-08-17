#pragma once
#include "MixerEngine.h"
#include "Json.h"
#include <fstream>
#include <string>
#include <vector>

namespace mesa {

/** Uma CENA (o "show profile"): fotografia completa do console, recarregavel.
    Nao inclui medidores nem estado transitorio — so o que o operador configurou.

    IMPORTANTE: capture/apply rodam na thread de UI/config. Escrevem em atomics,
    entao podem ser chamados com o audio rodando; nunca dentro do callback. */

constexpr int kSceneFormatVersion = 1;

struct ChannelScene
{
    std::string name;
    int   inputIndex = -1;
    int   inputKind  = int (InputKind::Device);
    int   sourceType = int (SourceType::Line);
    float trimDb = 0.0f, faderDb = -100.0f, panPos = 0.0f;
    bool  on = false, mute = false, cue = false;
    unsigned busMask = 1u;

    bool  autoTrim = false;
    float autoTrimTargetDb = -20.0f;

    // rack de DSP
    std::vector<int> dspOrder;
    bool dspEnabled[int (DspType::NumTypes)] = { false, false, false, false, false };
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f;
    float compThresholdDb = -18.0f, compRatio = 3.0f;
    float gateThresholdDb = -45.0f;

    // mix-minus
    int   feedSource = 0;
    int   feedOutputPair = -1;

    bool  trigEnabled = false;
    int   trigSource  = int (TapPoint::Input);
    float thresholdDb = -35.0f, triggerMs = 150.0f, holdMs = 800.0f,
          releaseMs = 400.0f, cooldownMs = 500.0f, hysteresisDb = 5.0f;
    int   camera = 0;
    std::string command, releaseCommand;
    int   triggerTarget = 0;
    bool  logicEnabled = false;
    int   logicTarget = 0;
    std::string onCommand, offCommand;
};

struct Scene
{
    std::string name = "NOVA CENA";
    std::string notes;
    float masterGainDb = 0.0f;

    bool  autoEnabled = true, autoTestMode = false, autoDominance = true;
    float dominanceDb = 6.0f, minShotMs = 1200.0f;
    int   wideCamera = 0;

    // seção de monitoração faz parte do show profile
    int   monitorSource = 0;
    float monitorDb = -18.0f, phonesDb = -15.0f, cueDb = -12.0f, studioDb = -18.0f;

    std::vector<ChannelScene> channels;
};

// ---------------------------------------------------------------- capture / apply

inline Scene captureScene (const MixerEngine& mix, const std::string& name)
{
    Scene s;
    s.name = name;
    s.masterGainDb  = mix.masterGainDb.load();
    s.autoEnabled   = mix.automation.enabled.load();
    s.autoTestMode  = mix.automation.testMode.load();
    s.autoDominance = mix.automation.dominance.load();
    s.dominanceDb   = mix.automation.dominanceDb.load();
    s.minShotMs     = mix.automation.minShotMs.load();
    s.wideCamera    = mix.automation.wideCamera.load();
    s.monitorSource = mix.monitor.source.load();
    s.monitorDb     = mix.monitor.monitorDb.load();
    s.phonesDb      = mix.monitor.phonesDb.load();
    s.cueDb         = mix.monitor.cueDb.load();
    s.studioDb      = mix.monitor.studioDb.load();

    for (int i = 0; i < mix.numChannels(); ++i)
    {
        const auto& c = mix.channel (i);
        ChannelScene cs;
        cs.name       = c.name;
        cs.inputIndex = c.params.inputIndex.load();
        cs.inputKind  = c.params.inputKind.load();
        cs.sourceType = c.params.sourceType.load();
        cs.trimDb     = c.params.trimDb.load();
        cs.faderDb    = c.params.faderDb.load();
        cs.panPos     = c.params.panPos.load();
        cs.on         = c.params.on.load();
        cs.mute       = c.params.mute.load();
        cs.cue        = c.params.cue.load();
        cs.busMask    = c.params.busMask.load();

        cs.autoTrim         = c.params.autoTrim.enabled.load();
        cs.autoTrimTargetDb = c.params.autoTrim.targetDb.load();

        cs.feedSource     = c.params.feedSource.load();
        cs.feedOutputPair = c.params.feedOutputPair.load();

        auto& rack = const_cast<Channel&> (c).rack;
        cs.dspOrder = rack.order();
        for (int t = 0; t < int (DspType::NumTypes); ++t)
            cs.dspEnabled[t] = rack.isEnabled (DspType (t));
        cs.eqLowDb  = rack.equaliser().lowGainDb.load();
        cs.eqMidDb  = rack.equaliser().midGainDb.load();
        cs.eqHighDb = rack.equaliser().highGainDb.load();
        cs.compThresholdDb = rack.compressor().thresholdDb.load();
        cs.compRatio       = rack.compressor().ratio.load();
        cs.gateThresholdDb = rack.noiseGate().thresholdDb.load();

        cs.trigEnabled  = c.params.trigger.enabled.load();
        cs.trigSource   = c.params.trigger.source.load();
        cs.thresholdDb  = c.params.trigger.thresholdDb.load();
        cs.triggerMs    = c.params.trigger.triggerMs.load();
        cs.holdMs       = c.params.trigger.holdMs.load();
        cs.releaseMs    = c.params.trigger.releaseMs.load();
        cs.cooldownMs   = c.params.trigger.cooldownMs.load();
        cs.hysteresisDb = c.params.trigger.hysteresisDb.load();
        cs.camera         = c.params.trigger.camera.load();
        cs.command        = c.params.trigger.command.str();
        cs.releaseCommand = c.params.trigger.releaseCommand.str();
        cs.triggerTarget  = c.params.trigger.target.load();
        cs.logicEnabled   = c.params.logicEnabled.load();
        cs.logicTarget    = c.params.logicTarget.load();
        cs.onCommand      = c.params.onCommand.str();
        cs.offCommand     = c.params.offCommand.str();

        s.channels.push_back (std::move (cs));
    }
    return s;
}

/** Aplica a cena. Canais que estao NO AR nao trocam de fonte nem de estado:
    a mudanca fica pendente ate o operador desligar o canal — mesma logica de
    seguranca das consoles de broadcast. Retorna quantos canais ficaram pendentes. */
inline int applyScene (const Scene& s, MixerEngine& mix, bool respectOnAir = true)
{
    mix.masterGainDb.store (s.masterGainDb);
    mix.automation.enabled  .store (s.autoEnabled);
    mix.automation.testMode .store (s.autoTestMode);
    mix.automation.dominance.store (s.autoDominance);
    mix.automation.dominanceDb.store (s.dominanceDb);
    mix.automation.minShotMs  .store (s.minShotMs);
    mix.automation.wideCamera .store (s.wideCamera);
    mix.monitor.source   .store (s.monitorSource);
    mix.monitor.monitorDb.store (s.monitorDb);
    mix.monitor.phonesDb .store (s.phonesDb);
    mix.monitor.cueDb    .store (s.cueDb);
    mix.monitor.studioDb .store (s.studioDb);

    int pending = 0;
    const int n = std::min (int (s.channels.size()), mix.numChannels());

    for (int i = 0; i < n; ++i)
    {
        const auto& cs = s.channels[size_t (i)];
        auto& c = mix.channel (i);

        const bool onAir = respectOnAir && c.params.on.load();
        if (onAir && (cs.inputIndex != c.params.inputIndex.load() || ! cs.on))
        {
            ++pending;      // nao derruba o que esta no ar
            continue;
        }

        c.name = cs.name;
        c.params.inputIndex.store (cs.inputIndex);
        c.params.inputKind .store (cs.inputKind);
        c.params.sourceType.store (cs.sourceType);
        c.params.trimDb    .store (cs.trimDb);
        c.params.faderDb   .store (cs.faderDb);
        c.params.panPos    .store (cs.panPos);
        c.params.on        .store (cs.on);
        c.params.mute      .store (cs.mute);
        c.params.cue       .store (cs.cue);
        c.params.busMask   .store (cs.busMask);

        c.params.autoTrim.enabled .store (cs.autoTrim);
        c.params.autoTrim.targetDb.store (cs.autoTrimTargetDb);

        c.params.feedSource    .store (cs.feedSource);
        c.params.feedOutputPair.store (cs.feedOutputPair);

        if (! cs.dspOrder.empty()) c.rack.setOrder (cs.dspOrder);
        for (int t = 0; t < int (DspType::NumTypes); ++t)
            c.rack.setEnabled (DspType (t), cs.dspEnabled[t]);
        c.rack.equaliser().lowGainDb .store (cs.eqLowDb);
        c.rack.equaliser().midGainDb .store (cs.eqMidDb);
        c.rack.equaliser().highGainDb.store (cs.eqHighDb);
        c.rack.compressor().thresholdDb.store (cs.compThresholdDb);
        c.rack.compressor().ratio      .store (cs.compRatio);
        c.rack.noiseGate() .thresholdDb.store (cs.gateThresholdDb);

        c.params.trigger.enabled     .store (cs.trigEnabled);
        c.params.trigger.source      .store (cs.trigSource);
        c.params.trigger.thresholdDb .store (cs.thresholdDb);
        c.params.trigger.triggerMs   .store (cs.triggerMs);
        c.params.trigger.holdMs      .store (cs.holdMs);
        c.params.trigger.releaseMs   .store (cs.releaseMs);
        c.params.trigger.cooldownMs  .store (cs.cooldownMs);
        c.params.trigger.hysteresisDb.store (cs.hysteresisDb);
        c.params.trigger.camera      .store (cs.camera);
        c.params.trigger.command       .set (cs.command);
        c.params.trigger.releaseCommand.set (cs.releaseCommand);
        c.params.trigger.target.store (cs.triggerTarget);
        c.params.logicEnabled.store (cs.logicEnabled);
        c.params.logicTarget .store (cs.logicTarget);
        c.params.onCommand .set (cs.onCommand);
        c.params.offCommand.set (cs.offCommand);
    }
    return pending;
}

// ---------------------------------------------------------------- json

inline std::string sceneToJson (const Scene& s)
{
    using namespace json;
    auto root = object();
    root.set ("format", num (kSceneFormatVersion));
    root.set ("name",   text (s.name));
    root.set ("notes",  text (s.notes));
    root.set ("masterGainDb", num (s.masterGainDb));

    auto mon = object();
    mon.set ("source",    num (s.monitorSource));
    mon.set ("monitorDb", num (s.monitorDb));
    mon.set ("phonesDb",  num (s.phonesDb));
    mon.set ("cueDb",     num (s.cueDb));
    mon.set ("studioDb",  num (s.studioDb));
    root.set ("monitor", mon);

    auto autom = object();
    autom.set ("enabled",     boolean (s.autoEnabled));
    autom.set ("testMode",    boolean (s.autoTestMode));
    autom.set ("dominance",   boolean (s.autoDominance));
    autom.set ("dominanceDb", num (s.dominanceDb));
    autom.set ("minShotMs",   num (s.minShotMs));
    autom.set ("wideCamera",  num (s.wideCamera));
    root.set ("automation", autom);

    auto chans = array();
    for (const auto& c : s.channels)
    {
        auto o = object();
        o.set ("name",       text (c.name));
        o.set ("inputIndex", num (c.inputIndex));
        o.set ("inputKind",  num (c.inputKind));
        o.set ("sourceType", num (c.sourceType));
        o.set ("trimDb",     num (c.trimDb));
        o.set ("faderDb",    num (c.faderDb));
        o.set ("panPos",     num (c.panPos));
        o.set ("on",         boolean (c.on));
        o.set ("mute",       boolean (c.mute));
        o.set ("cue",        boolean (c.cue));
        o.set ("busMask",    num (c.busMask));

        auto at = object();
        at.set ("enabled",  boolean (c.autoTrim));
        at.set ("targetDb", num (c.autoTrimTargetDb));
        o.set ("autoTrim", at);

        auto fd = object();
        fd.set ("source",     num (c.feedSource));
        fd.set ("outputPair", num (c.feedOutputPair));
        o.set ("mixMinus", fd);

        auto lg = object();
        lg.set ("enabled",    boolean (c.logicEnabled));
        lg.set ("target",     num (c.logicTarget));
        lg.set ("onCommand",  text (c.onCommand));
        lg.set ("offCommand", text (c.offCommand));
        o.set ("logic", lg);

        auto ds = object();
        auto ord = array();
        for (int t : c.dspOrder) ord.arr.push_back (num (t));
        ds.set ("order", ord);
        auto en = array();
        for (int t = 0; t < int (DspType::NumTypes); ++t) en.arr.push_back (boolean (c.dspEnabled[t]));
        ds.set ("enabled",         en);
        ds.set ("eqLowDb",         num (c.eqLowDb));
        ds.set ("eqMidDb",         num (c.eqMidDb));
        ds.set ("eqHighDb",        num (c.eqHighDb));
        ds.set ("compThresholdDb", num (c.compThresholdDb));
        ds.set ("compRatio",       num (c.compRatio));
        ds.set ("gateThresholdDb", num (c.gateThresholdDb));
        o.set ("dsp", ds);

        auto tg = object();
        tg.set ("enabled",      boolean (c.trigEnabled));
        tg.set ("source",       num (c.trigSource));
        tg.set ("thresholdDb",  num (c.thresholdDb));
        tg.set ("triggerMs",    num (c.triggerMs));
        tg.set ("holdMs",       num (c.holdMs));
        tg.set ("releaseMs",    num (c.releaseMs));
        tg.set ("cooldownMs",   num (c.cooldownMs));
        tg.set ("hysteresisDb", num (c.hysteresisDb));
        tg.set ("camera",         num (c.camera));
        tg.set ("command",        text (c.command));
        tg.set ("releaseCommand", text (c.releaseCommand));
        tg.set ("target",         num (c.triggerTarget));
        o.set ("trigger", tg);

        chans.arr.push_back (o);
    }
    root.set ("channels", chans);
    return toString (root);
}

inline bool sceneFromJson (const std::string& src, Scene& out)
{
    json::Value root;
    if (! json::parse (src, root) || root.type != json::Value::Object) return false;
    if (int (root.number ("format", 0)) > kSceneFormatVersion) return false;

    out = Scene{};
    out.name  = root.string ("name", "SEM NOME");
    out.notes = root.string ("notes");
    out.masterGainDb = float (root.number ("masterGainDb", 0.0));

    if (auto* a = root.find ("automation"))
    {
        out.autoEnabled   = a->boolean ("enabled", true);
        out.autoTestMode  = a->boolean ("testMode", false);
        out.autoDominance = a->boolean ("dominance", true);
        out.dominanceDb   = float (a->number ("dominanceDb", 6.0));
        out.minShotMs     = float (a->number ("minShotMs", 1200.0));
        out.wideCamera    = int   (a->number ("wideCamera", 0));
    }

    if (auto* m = root.find ("monitor"))
    {
        out.monitorSource = int   (m->number ("source", 0));
        out.monitorDb     = float (m->number ("monitorDb", -18.0));
        out.phonesDb      = float (m->number ("phonesDb", -15.0));
        out.cueDb         = float (m->number ("cueDb", -12.0));
        out.studioDb      = float (m->number ("studioDb", -18.0));
    }

    auto* chans = root.find ("channels");
    if (chans == nullptr || chans->type != json::Value::Array) return false;

    for (const auto& o : chans->arr)
    {
        ChannelScene c;
        c.name       = o.string ("name");
        c.inputIndex = int   (o.number ("inputIndex", -1));
        c.inputKind  = int   (o.number ("inputKind", 0));
        c.sourceType = int   (o.number ("sourceType", int (SourceType::Line)));
        c.trimDb     = float (o.number ("trimDb", 0.0));
        c.faderDb    = float (o.number ("faderDb", -100.0));
        c.panPos     = float (o.number ("panPos", 0.0));
        c.on         = o.boolean ("on");
        c.mute       = o.boolean ("mute");
        c.cue        = o.boolean ("cue");
        c.busMask    = unsigned (o.number ("busMask", 1.0));

        if (auto* at = o.find ("autoTrim"))
        {
            c.autoTrim         = at->boolean ("enabled");
            c.autoTrimTargetDb = float (at->number ("targetDb", -20.0));
        }
        if (auto* fd = o.find ("mixMinus"))
        {
            c.feedSource     = int (fd->number ("source", 0));
            c.feedOutputPair = int (fd->number ("outputPair", -1));
        }
        if (auto* lg = o.find ("logic"))
        {
            c.logicEnabled = lg->boolean ("enabled");
            c.logicTarget  = int (lg->number ("target", 0));
            c.onCommand    = lg->string ("onCommand");
            c.offCommand   = lg->string ("offCommand");
        }
        if (auto* ds = o.find ("dsp"))
        {
            if (auto* ord = ds->find ("order"))
                for (const auto& v : ord->arr) c.dspOrder.push_back (int (v.num));
            if (auto* en = ds->find ("enabled"))
                for (size_t t = 0; t < en->arr.size() && t < size_t (DspType::NumTypes); ++t)
                    c.dspEnabled[t] = en->arr[t].b;
            c.eqLowDb         = float (ds->number ("eqLowDb", 0.0));
            c.eqMidDb         = float (ds->number ("eqMidDb", 0.0));
            c.eqHighDb        = float (ds->number ("eqHighDb", 0.0));
            c.compThresholdDb = float (ds->number ("compThresholdDb", -18.0));
            c.compRatio       = float (ds->number ("compRatio", 3.0));
            c.gateThresholdDb = float (ds->number ("gateThresholdDb", -45.0));
        }
        if (auto* tg = o.find ("trigger"))
        {
            c.trigEnabled  = tg->boolean ("enabled");
            c.trigSource   = int   (tg->number ("source", 0));
            c.thresholdDb  = float (tg->number ("thresholdDb", -35.0));
            c.triggerMs    = float (tg->number ("triggerMs", 150.0));
            c.holdMs       = float (tg->number ("holdMs", 800.0));
            c.releaseMs    = float (tg->number ("releaseMs", 400.0));
            c.cooldownMs   = float (tg->number ("cooldownMs", 500.0));
            c.hysteresisDb = float (tg->number ("hysteresisDb", 5.0));
            c.camera         = int (tg->number ("camera", 0));
            c.command        = tg->string ("command");
            c.releaseCommand = tg->string ("releaseCommand");
            c.triggerTarget  = int (tg->number ("target", 0));
        }
        out.channels.push_back (std::move (c));
    }
    return true;
}

inline bool saveSceneToFile (const Scene& s, const std::string& path)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    f << sceneToJson (s);
    return f.good();
}

inline bool loadSceneFromFile (const std::string& path, Scene& out)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) return false;
    std::string src ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    return sceneFromJson (src, out);
}

} // namespace mesa
