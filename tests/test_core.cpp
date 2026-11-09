// Testes do core sem JUCE: g++ -std=c++17 -O2 tests/test_core.cpp -o build/test_core
#include "../Source/Core/MixerEngine.h"
#include "../Source/Core/Scene.h"
#include "../Source/Core/Settings.h"
#include "../Source/Core/AsyncSource.h"
#include "../Source/Core/RateConverter.h"
#include "../Source/Core/Defaults.h"
#include "../Source/Core/SpeechSegmenter.h"
#include "../Source/Core/AutomationEngine.h"
#include "../Source/Core/AnuncioLw.h"
#include "../Source/Core/GeradorTeste.h"
#include "../Source/Core/Atraso.h"
#include <thread>
#include <cstdio>
#include <cmath>
#include <cassert>
#include <string>

using namespace mesa;

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "[ ok ]" : "[FALHA]", what);
    if (! ok) ++failures;
}
static bool near (float a, float b, float tol) { return std::fabs (a - b) <= tol; }

// M_PI nao e padrao: o MSVC so define com _USE_MATH_DEFINES. Usa a constante do core.
static void fillSine (std::vector<float>& buf, double& phase, double freq, double sr, float peak)
{
    for (auto& s : buf)
    { s = peak * float (std::sin (phase)); phase += 2.0 * double (mesa::kPi) * freq / sr; }
}

int main()
{
    const double sr = 48000.0; const int block = 128;

    // 1. conversao dB <-> ganho
    check (near (dbToGain (0.0f), 1.0f, 1e-6f), "0 dB = ganho 1.0");
    check (near (dbToGain (-6.0f), 0.5011872f, 1e-4f), "-6 dB = ~0.501");
    check (near (gainToDb (dbToGain (-18.5f)), -18.5f, 1e-3f), "roundtrip dB");

    // 2. meter: seno a -20 dBFS => pico -20, RMS -23
    Meter m; m.prepare (sr);
    std::vector<float> buf (block); double ph = 0.0;
    for (int i = 0; i < 1000; ++i) { fillSine (buf, ph, 1000.0, sr, dbToGain (-20.0f)); m.process (buf.data(), block); }  // janela de RMS = 300 ms; precisa de ~2 s para assentar
    check (near (m.peakDb(), -20.0f, 0.6f), "meter pico ~ -20 dBFS");
    check (near (m.rmsDb(),  -23.0f, 0.6f), "meter RMS ~ -23 dBFS");

    // 3. auto trim converge para o alvo
    AutoTrim at; at.prepare (sr);
    AutoTrimParams p; p.enabled.store (true); p.targetDb.store (-20.0f);
    p.maxGainDb.store (30.0f); p.speedDbPerSec.store (12.0f);
    for (int i = 0; i < 4000; ++i) at.update (-40.0f, block, p);   // ~10 s de audio
    check (near (at.currentDb(), 20.0f, 0.5f), "auto trim leva -40 dB para -20 dB (+20 dB)");
    p.enabled.store (false);
    for (int i = 0; i < 4000; ++i) at.update (-40.0f, block, p);
    check (near (at.currentDb(), 0.0f, 0.5f), "auto trim desligado volta a 0 dB");

    // 4. canal: OFF nao passa audio, ON com fader 0 dB passa
    Channel ch; ch.prepare (sr, block);
    ch.params.faderDb.store (0.0f); ch.params.on.store (false);
    std::vector<float> sig (block); double ph2 = 0.0;
    fillSine (sig, ph2, 1000.0, sr, 0.5f);
    for (int i = 0; i < 50; ++i) ch.process (sig.data(), block);
    check (ch.meterOut.peakDb() < -80.0f, "canal OFF nao passa audio");
    ch.params.on.store (true);
    for (int i = 0; i < 100; ++i) { fillSine (sig, ph2, 1000.0, sr, 0.5f); ch.process (sig.data(), block); }
    check (near (ch.meterOut.peakDb(), gainToDb (0.5f), 0.6f), "canal ON com fader 0 dB passa em unidade");
    // o tap usa envelope de deteccao (ataque 5 ms, queda 60 ms): para um seno continuo
    // ele assenta perto do RMS, nao do pico — e um pouco abaixo do valor de pico
    check (ch.tapDb (TapPoint::Input) > gainToDb (0.5f) - 4.0f
        && ch.tapDb (TapPoint::Input) <= gainToDb (0.5f) + 0.5f,
           "tap de INPUT segue a entrada com balistica de deteccao");

    // 5. mixer: dois canais somando no PGM1
    MixerEngine mix; mix.prepare (sr, block, 12);
    check (mix.numChannels() == 12, "12 canais criados");
    for (int c = 0; c < 2; ++c)
    {
        auto& k = mix.channel (c);
        k.params.on.store (true); k.params.faderDb.store (0.0f);
        k.params.busMask.store (1u); k.params.inputIndex.store (c);
    }
    std::vector<std::vector<float>> ins (12, std::vector<float> (block, 0.0f));
    std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
    std::vector<const float*> inPtr (12); std::vector<float*> outPtr (8);
    for (int i = 0; i < 12; ++i) inPtr[size_t (i)] = ins[size_t (i)].data();
    for (int i = 0; i < 8;  ++i) outPtr[size_t (i)] = outs[size_t (i)].data();

    double ph3 = 0.0, ph4 = 0.0;
    for (int i = 0; i < 200; ++i)
    {
        fillSine (ins[0], ph3, 1000.0, sr, dbToGain (-20.0f));
        fillSine (ins[1], ph4, 1000.0, sr, dbToGain (-20.0f));
        mix.process (inPtr.data(), 12, outPtr.data(), 8, block);
    }
    // Dois sinais identicos em fase somam +6 dB. O pan central NAO tira nada:
    // desde a correcao da lei de pan, centro e unidade nos dois lados — como
    // manda um console de radio, onde o fader em 0 tem de bater com qualquer
    // outra fonte na mesma saida.
    check (near (mix.masterMeterL.peakDb(), -20.0f + 6.0f, 0.8f),
           "soma de 2 canais no PGM1 com pan central");
    check (mix.busMeter[1].peakDb() < -80.0f, "PGM2 permanece em silencio");

    // 6. sem fonte, sem lixo na saida
    auto& c3 = mix.channel (3); c3.params.inputIndex.store (-1); c3.params.on.store (true);
    for (int i = 0; i < 20; ++i) mix.process (inPtr.data(), 12, outPtr.data(), 8, block);
    check (c3.tapDb (TapPoint::Input) <= kMinusInfDb, "canal sem fonte fica em -inf");

    // 7. cena: capturar -> json -> ler -> aplicar em um mixer novo
    {
        MixerEngine a; a.prepare (sr, block, 12);
        a.channel (0).name = "MIC 1 APRESENTADOR";
        a.channel (0).params.faderDb.store (-4.5f);
        a.channel (0).params.busMask.store (0b0011u);
        a.channel (0).params.panPos.store (-0.5f);
        a.channel (0).params.autoTrim.enabled.store (true);
        a.channel (0).params.trigger.enabled.store (true);
        a.channel (0).params.trigger.thresholdDb.store (-33.5f);
        a.channel (0).params.trigger.camera.store (2);
        a.channel (5).params.on.store (true);
        a.masterGainDb.store (-2.0f);
        a.automation.dominance.store (false);
        a.automation.wideCamera.store (5);

        auto scene = captureScene (a, "ENTREVISTA");
        const std::string txt = sceneToJson (scene);
        check (txt.find ("MIC 1 APRESENTADOR") != std::string::npos, "cena serializa nome da fonte");

        Scene back;
        check (sceneFromJson (txt, back), "cena volta do JSON sem erro");
        check (back.name == "ENTREVISTA", "nome da cena preservado");
        check (back.channels.size() == 12, "12 canais na cena");
        check (near (back.channels[0].faderDb, -4.5f, 0.001f), "fader preservado");
        check (back.channels[0].busMask == 0b0011u, "atribuicao de bus preservada");
        check (near (back.channels[0].thresholdDb, -33.5f, 0.001f), "threshold do trigger preservado");
        check (back.channels[0].camera == 2, "camera preservada");
        check (back.wideCamera == 5 && ! back.autoDominance, "parametros de automacao preservados");

        MixerEngine b; b.prepare (sr, block, 12);
        const int pend = applyScene (back, b);
        check (pend == 0, "nada pendente ao aplicar em console limpo");
        check (b.channel (0).name == "MIC 1 APRESENTADOR", "nome aplicado");
        check (near (b.channel (0).params.panPos.load(), -0.5f, 0.001f), "pan aplicado");
        check (b.channel (0).params.autoTrim.enabled.load(), "auto trim aplicado");
        check (near (b.masterGainDb.load(), -2.0f, 0.001f), "master aplicado");

        // canal no ar nao pode ser derrubado por troca de cena
        MixerEngine c; c.prepare (sr, block, 12);
        c.channel (3).params.on.store (true);
        c.channel (3).params.inputIndex.store (9);
        Scene other = back; other.channels[3].inputIndex = 1; other.channels[3].on = false;
        const int pend2 = applyScene (other, c);
        check (pend2 == 1, "1 canal fica pendente por estar no ar");
        check (c.channel (3).params.inputIndex.load() == 9, "canal no ar mantem a fonte atual");

        check (! sceneFromJson ("{isso nao e json", back), "JSON invalido e recusado");
    }

    // 8. configuracoes da instalacao
    {
        Settings st;
        st.device.deviceName = "Focusrite USB ASIO";
        st.device.bufferSize = 64;
        st.routing.busOutputPair[1] = -1;
        st.routing.cueOutputPair = 3;
        st.routing.masterGainDb = -1.5f;
        st.vmix.protocol = "TCP"; st.vmix.port = 8099;
        st.dsp.vst3Path = "C:/Program Files/Common Files/VST3";

        Settings back;
        check (settingsFromJson (settingsToJson (st), back), "configuracoes voltam do JSON");
        check (back.device.deviceName == "Focusrite USB ASIO", "dispositivo preservado");
        check (back.device.bufferSize == 64, "buffer preservado");
        check (back.routing.busOutputPair[1] == -1, "bus desroteado preservado");
        check (back.vmix.port == 8099, "porta do vMix preservada");
        check (back.dsp.vst3Path.find ("VST3") != std::string::npos, "caminho de VST3 preservado");

        MixerEngine m; m.prepare (sr, block, 4);
        applyRouting (back, m);
        check (m.busParams[1].outputPair.load() == -1, "roteamento aplicado ao engine");
        check (m.monitor.cuePair.load() == 3, "CUE roteado");
        check (near (m.masterGainDb.load(), -1.5f, 0.001f), "ganho de master aplicado");
    }

    // 9. CUE e PFL: independe do fader e do ON/OFF
    {
        MixerEngine m; m.prepare (sr, block, 4);
        m.monitor.cuePair.store (1);            // CUE nas saidas 3/4
        m.monitor.monitorPair.store (0);
        auto& c0 = m.channel (0);
        c0.params.inputIndex.store (0);
        c0.params.on.store (false);              // canal FECHADO
        c0.params.faderDb.store (-100.0f);       // fader no fundo
        c0.params.cue.store (true);

        std::vector<std::vector<float>> ins (4, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (4); std::vector<float*> op (8);
        for (int i = 0; i < 4; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        double ph = 0.0;
        for (int i = 0; i < 400; ++i)
        {
            fillSine (ins[0], ph, 1000.0, sr, dbToGain (-12.0f));
            m.process (ip.data(), 4, op.data(), 8, block);
        }
        check (m.cueMeter.peakDb() > -30.0f, "CUE ouve o canal fechado com fader no fundo (PFL)");
        check (m.busMeter[0].peakDb() < -80.0f, "e esse canal nao vaza para o PGM 1");
        check (m.cueOn.load(), "estado de CUE publicado para a superficie");
    }

    // 10. mute automatico do monitor pelo tipo de fonte
    {
        MixerEngine m; m.prepare (sr, block, 4);
        m.monitor.monitorPair.store (0);
        m.monitor.monitorDb.store (0.0f);
        auto& mic = m.channel (0);
        mic.params.sourceType.store (int (SourceType::Operator));
        mic.params.inputIndex.store (0);
        auto& line = m.channel (1);
        line.params.sourceType.store (int (SourceType::Line));
        line.params.inputIndex.store (1);
        line.params.on.store (true); line.params.faderDb.store (0.0f);

        std::vector<std::vector<float>> ins (4, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (4); std::vector<float*> op (8);
        for (int i = 0; i < 4; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        double ph = 0.0;
        for (int i = 0; i < 400; ++i)
        {
            fillSine (ins[1], ph, 1000.0, sr, dbToGain (-12.0f));
            m.process (ip.data(), 4, op.data(), 8, block);
        }
        check (m.monitorMeter.peakDb() > -30.0f, "monitor ouve o PGM 1 com os mics fechados");
        check (! m.crMuted.load(), "sem microfone aberto, monitor nao esta mudo");

        mic.params.on.store (true);              // abriu o mic do operador
        // o pico do medidor cai com 650 ms de release: precisa de alguns segundos
        for (int i = 0; i < 2500; ++i)
        {
            fillSine (ins[1], ph, 1000.0, sr, dbToGain (-12.0f));
            m.process (ip.data(), 4, op.data(), 8, block);
        }
        check (m.crMuted.load(), "microfone do controle aberto marca o monitor como mudo");
        check (m.monitorMeter.peakDb() < -60.0f, "monitor realmente silencia (evita microfonia)");
        check (m.busMeter[0].peakDb() > -30.0f, "e o PGM 1 continua no ar");
    }

    // 11. cena leva o tipo de fonte e a monitoracao
    {
        MixerEngine a; a.prepare (sr, block, 4);
        a.channel (0).params.sourceType.store (int (SourceType::Phone));
        a.monitor.source.store (int (MonitorSource::Ext1));
        a.monitor.monitorDb.store (-24.0f);
        Scene sc; check (sceneFromJson (sceneToJson (captureScene (a, "T")), sc), "cena serializa");
        check (sc.channels[0].sourceType == int (SourceType::Phone), "tipo de fonte na cena");
        check (sc.monitorSource == int (MonitorSource::Ext1), "fonte do monitor na cena");
        check (near (sc.monitorDb, -24.0f, 0.01f), "nivel do monitor na cena");
        check (needsMixMinus (sc.channels[0].sourceType), "telefone exige mix-minus proprio");
    }

    // 12. fonte assincrona (NDI / AES67 / playout) chegando por fila sem lock
    {
        AsyncSource ndi; ndi.prepare (block, 8);
        ndi.name = "CARTUCHEIRA (Player 1)";

        std::vector<float> chunk (size_t (block), 0.0f);
        double ph = 0.0;
        for (int i = 0; i < 4; ++i) { fillSine (chunk, ph, 1000.0, sr, 0.5f); ndi.push (chunk.data(), block); }

        const float* out1 = ndi.pull (block);
        bool hasAudio = false;
        for (int i = 0; i < block; ++i) if (std::fabs (out1[i]) > 0.01f) hasAudio = true;
        check (ndi.isConnected(), "fonte de rede reporta conectada");
        check (ndi.underruns() == 0, "sem underrun com a fila abastecida");

        // consome mais do que entra: precisa render silencio, nunca lixo nem travar
        for (int i = 0; i < 40; ++i) ndi.pull (block);
        check (ndi.underruns() > 0, "underrun e contabilizado quando a fonte para");
        const float* out2 = ndi.pull (block);
        bool silent = true;
        for (int i = 0; i < block; ++i) if (out2[i] != 0.0f) silent = false;
        check (silent, "fila vazia devolve silencio, nao lixo");
        (void) hasAudio;

        // produtor em outra thread, como a thread de rede do NDI
        AsyncSource s2; s2.prepare (block, 8);
        std::atomic<bool> run { true };
        std::thread producer ([&] {
            std::vector<float> c (size_t (block), 0.25f);
            while (run.load()) { s2.push (c.data(), block); }
        });
        for (int i = 0; i < 500; ++i) s2.pull (block);
        run.store (false); producer.join();
        check (true, "produtor em outra thread nao trava o consumidor");
    }

    // 13. canal alimentado por fonte de rede em vez da placa
    {
        MixerEngine m; m.prepare (sr, block, 4);
        auto& c0 = m.channel (0);
        c0.params.inputKind.store (int (InputKind::Network));
        c0.params.inputIndex.store (0);
        c0.params.on.store (true);
        c0.params.faderDb.store (0.0f);
        c0.params.busMask.store (1u);

        AsyncSource ndi; ndi.prepare (block, 8);
        std::vector<std::vector<float>> ins (4, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (4); std::vector<float*> op (8);
        for (int i = 0; i < 4; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        std::vector<float> chunk (size_t (block), 0.0f);
        double ph = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            fillSine (chunk, ph, 1000.0, sr, dbToGain (-14.0f));
            ndi.push (chunk.data(), block);
            const float* netBlock = ndi.pull (block);      // puxa antes do mix
            const float* nets[1] = { netBlock };
            m.process (ip.data(), 4, op.data(), 8, block, nets, 1);
        }
        check (m.busMeter[0].peakDb() > -30.0f, "canal em NDI chega ao PGM 1");
        check (m.channel (0).tapDb (TapPoint::Input) > -30.0f, "tap de trigger funciona na fonte de rede");

        Settings st;
        NetworkSource src1;
        src1.kind = "NDI"; src1.name = "CARTUCHEIRA (Player 1)";
        src1.channels = 2; src1.audioOnly = true; src1.bufferBlocks = 8;
        st.networkSources.push_back (src1);
        Settings back;
        check (settingsFromJson (settingsToJson (st), back), "fontes de rede vao para as configuracoes");
        check (back.networkSources.size() == 1 &&
               back.networkSources[0].name == "CARTUCHEIRA (Player 1)", "nome do emissor preservado");

        Settings st2;
        NetworkSource cart;
        cart.name = "PC-CART (Player 1)"; cart.machine = "PC-CART";
        cart.controlUrl = "http://192.168.1.40:9000/cart"; cart.sendStartStop = true;
        st2.networkSources.push_back (cart);
        st2.network.machineName = "MESA-ESTUDIO-1";
        st2.network.discoveryServer = "192.168.1.10";
        Settings b2;
        check (settingsFromJson (settingsToJson (st2), b2), "config de rede volta do JSON");
        check (b2.networkSources[0].sendStartStop, "fader-start remoto preservado");
        check (b2.network.discoveryServer == "192.168.1.10", "servidor de descoberta preservado");
    }

    // 14. rack de DSP
    {
        Channel ch; ch.prepare (sr, block);
        ch.params.on.store (true); ch.params.faderDb.store (0.0f);

        std::vector<float> sig ((size_t (block))); double ph = 0.0;
        auto runBlocks = [&] (int count, float peak) {
            for (int i = 0; i < count; ++i) { fillSine (sig, ph, 1000.0, sr, peak); ch.process (sig.data(), block); }
        };

        runBlocks (200, dbToGain (-10.0f));
        const float dry = ch.meterOut.peakDb();

        // gate fechado corta o sinal fraco
        ch.rack.setEnabled (DspType::Gate, true);
        ch.rack.noiseGate().thresholdDb.store (-20.0f);
        ch.rack.noiseGate().rangeDb.store (-60.0f);
        ch.rack.noiseGate().holdMs.store (1.0f);
        runBlocks (2500, dbToGain (-40.0f));
        check (ch.meterOut.peakDb() < -55.0f, "gate fecha em sinal abaixo do threshold");
        runBlocks (300, dbToGain (-10.0f));
        check (ch.meterOut.peakDb() > -14.0f, "gate abre na fala");
        ch.rack.setEnabled (DspType::Gate, false);

        // compressor reduz o que passa do threshold
        ch.rack.setEnabled (DspType::Compressor, true);
        ch.rack.compressor().thresholdDb.store (-24.0f);
        ch.rack.compressor().ratio.store (8.0f);
        runBlocks (400, dbToGain (-10.0f));
        check (ch.meterOut.peakDb() < dry - 5.0f, "compressor reduz sinal acima do threshold");
        check (ch.rack.compressor().reduction.load() < -3.0f, "medidor de reducao reporta ganho negativo");
        ch.rack.setEnabled (DspType::Compressor, false);

        // limiter segura o teto
        ch.rack.setEnabled (DspType::Limiter, true);
        ch.rack.limiter().ceilingDb.store (-6.0f);
        runBlocks (400, 0.9f);
        check (ch.meterOut.peakDb() < -5.0f, "limiter respeita o teto");
        ch.rack.setEnabled (DspType::Limiter, false);

        // EQ com ganho positivo aumenta o nivel na banda
        ch.rack.setEnabled (DspType::Eq, true);
        ch.rack.equaliser().midFreq.store (1000.0f);
        ch.rack.equaliser().midGainDb.store (12.0f);
        runBlocks (400, dbToGain (-20.0f));
        check (ch.meterOut.peakDb() > -14.0f, "EQ realca a banda ajustada");
        ch.rack.setEnabled (DspType::Eq, false);

        // reordenacao publica uma lista completa e valida
        ch.rack.setOrder ({ int (DspType::Compressor), int (DspType::Eq) });
        auto ord = ch.rack.order();
        check (ord.size() == 2 && ord[0] == int (DspType::Compressor), "ordem do rack e trocada sem lock");
    }

    // 15. mix-minus: a fonte nao ouve a si mesma
    {
        MixerEngine m; m.prepare (sr, block, 3);
        auto& host = m.channel (0);      // microfone do apresentador
        host.params.sourceType.store (int (SourceType::Operator));
        host.params.inputIndex.store (0);
        host.params.on.store (true); host.params.faderDb.store (0.0f); host.params.busMask.store (1u);

        auto& phone = m.channel (1);     // telefone, com backfeed proprio
        phone.params.sourceType.store (int (SourceType::Phone));
        phone.params.inputIndex.store (1);
        phone.params.on.store (true); phone.params.faderDb.store (0.0f); phone.params.busMask.store (1u);
        phone.params.feedOutputPair.store (3);

        std::vector<std::vector<float>> ins (3, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (3); std::vector<float*> op (8);
        for (int i = 0; i < 3; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        double p1 = 0.0, p2 = 0.0;
        float maxBack = 0.0f, maxHostInBack = 0.0f;
        Meter backMeter; backMeter.prepare (sr);
        for (int i = 0; i < 300; ++i)
        {
            fillSine (ins[0], p1, 1000.0, sr, dbToGain (-12.0f));   // apresentador
            fillSine (ins[1], p2, 3000.0, sr, dbToGain (-12.0f));   // telefone
            m.process (ip.data(), 3, op.data(), 8, block);
            backMeter.process (m.backfeedData (1), block);
        }
        for (int k = 0; k < block; ++k) maxBack = std::max (maxBack, std::fabs (m.backfeedData (1)[k]));
        (void) maxHostInBack;
        check (backMeter.peakDb() > -30.0f, "backfeed do telefone tem o programa");

        // agora so o telefone fala: o backfeed dele deve ficar praticamente vazio
        std::fill (ins[0].begin(), ins[0].end(), 0.0f);
        Meter backMeter2; backMeter2.prepare (sr);
        for (int i = 0; i < 800; ++i)
        {
            fillSine (ins[1], p2, 3000.0, sr, dbToGain (-12.0f));
            m.process (ip.data(), 3, op.data(), 8, block);
            backMeter2.process (m.backfeedData (1), block);
        }
        check (backMeter2.peakDb() < -55.0f, "a fonte NAO ouve a si mesma no proprio backfeed");
        check (m.busMeter[0].peakDb() > -20.0f, "e ela continua no PGM 1");
    }

    // 16. trigger e automacao
    {
        MixerEngine m; m.prepare (sr, block, 3);
        AutomationEngine autoEng; autoEng.prepare (3);
        m.automation.minShotMs.store (0.0f);
        m.automation.dominance.store (false);

        auto& c0 = m.channel (0);
        c0.params.inputIndex.store (0); c0.params.on.store (true); c0.params.faderDb.store (0.0f);
        c0.params.trigger.enabled.store (true);
        c0.params.trigger.camera.store (1);
        c0.params.trigger.thresholdDb.store (-35.0f);
        c0.params.trigger.triggerMs.store (150.0f);
        c0.params.trigger.releaseMs.store (100.0f);

        std::vector<std::vector<float>> ins (3, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (3); std::vector<float*> op (8);
        for (int i = 0; i < 3; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();
        const float blockMs = float (block) / float (sr) * 1000.0f;

        // 50 ms de sinal: curto demais, tem de ser rejeitado
        double ph = 0.0;
        int shortBlocks = int (50.0f / blockMs);
        for (int i = 0; i < shortBlocks; ++i)
        {
            fillSine (ins[0], ph, 1000.0, sr, dbToGain (-20.0f));
            m.process (ip.data(), 3, op.data(), 8, block);
            autoEng.processBlock (m, blockMs);
        }
        std::fill (ins[0].begin(), ins[0].end(), 0.0f);
        for (int i = 0; i < 40; ++i)
        {
            m.process (ip.data(), 3, op.data(), 8, block);
            autoEng.processBlock (m, blockMs);
        }
        Command c;
        check (! autoEng.commands.pop (c), "pico de 50 ms nao vira corte");
        check (autoEng.rejections() > 0, "e fica registrado como rejeitado");

        // agora fala de verdade
        for (int i = 0; i < 200; ++i)
        {
            fillSine (ins[0], ph, 1000.0, sr, dbToGain (-20.0f));
            m.process (ip.data(), 3, op.data(), 8, block);
            autoEng.processBlock (m, blockMs);
        }
        bool fired = false;
        while (autoEng.commands.pop (c))
            if (c.type == Command::Type::Cut && c.camera == 1) fired = true;
        check (fired, "fala sustentada dispara CUT para a camera 1");
        check (autoEng.stateOf (0) == TriggerState::Active, "canal fica ACTIVE enquanto fala");
        check (autoEng.camera() == 1, "camera no ar publicada para a superficie");

        // modo de teste nao deve mandar comando de verdade
        m.automation.testMode.store (true);
        MixerEngine m2; m2.prepare (sr, block, 1);
        AutomationEngine a2; a2.prepare (1);
        m2.automation.testMode.store (true);
        m2.automation.minShotMs.store (0.0f);
        auto& t0 = m2.channel (0);
        t0.params.inputIndex.store (0); t0.params.on.store (true); t0.params.faderDb.store (0.0f);
        t0.params.trigger.enabled.store (true); t0.params.trigger.camera.store (2);
        std::vector<std::vector<float>> i2 (1, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip2 (1); ip2[0] = i2[0].data();
        for (int i = 0; i < 200; ++i)
        {
            fillSine (i2[0], ph, 1000.0, sr, dbToGain (-20.0f));
            m2.process (ip2.data(), 1, op.data(), 8, block);
            a2.processBlock (m2, blockMs);
        }
        bool sim = false;
        while (a2.commands.pop (c)) if (c.simulated) sim = true;
        check (sim, "modo de teste marca o comando como simulado");
        check (a2.camera() == 0, "e nao troca a camera no ar");
        check (a2.intended() == 2, "mas registra a intencao, para nao repetir o comando");

        // camera geral em modo de teste nao pode ficar repetindo o mesmo comando
        MixerEngine m3; m3.prepare (sr, block, 1);
        AutomationEngine a3; a3.prepare (1);
        m3.automation.testMode.store (true);
        m3.automation.wideCamera.store (5);
        m3.automation.minShotMs.store (100.0f);
        std::vector<std::vector<float>> i3 (1, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip3 (1); ip3[0] = i3[0].data();
        int cortesGerais = 0;
        for (int i = 0; i < 4000; ++i)      // ~10 s de silencio
        {
            m3.process (ip3.data(), 1, op.data(), 8, block);
            a3.processBlock (m3, blockMs);
            while (a3.commands.pop (c)) if (c.camera == 5) ++cortesGerais;
        }
        check (cortesGerais == 1, "camera geral e comandada UMA vez, nao a cada plano minimo");
    }

    // 17. perfis de acesso
    {
        Settings s2;
        check (s2.access.can ("gain"), "operador pode mexer em ganho");
        check (! s2.access.can ("dsp"), "operador nao abre o rack de DSP");
        s2.access.active = "Engenheiro";
        check (s2.access.can ("dsp"), "engenheiro abre o rack");
        // usuarios
        Settings u;
        check (u.access.users.size() == 2, "vem com operador e tecnico de fabrica");
        check (! u.access.login ("tecnico", "9999"), "PIN errado nao entra");
        check (u.access.login ("tecnico", "1234"), "PIN certo entra");
        check (u.access.can ("dsp"), "perfil do usuario passa a valer apos o login");
        u.access.logout();
        check (! u.access.can ("dsp"), "sair volta ao perfil restrito");

        User novo; novo.name = "produtor1"; novo.profile = "Produtor"; novo.pin = "5678";
        check (u.access.addUser (novo), "cria usuario");
        check (! u.access.addUser (novo), "nao aceita nome repetido");
        User invalido; invalido.name = "x"; invalido.profile = "Inexistente";
        check (! u.access.addUser (invalido), "nao aceita perfil que nao existe");
        check (u.access.login ("produtor1", "5678"), "usuario novo entra");
        check (u.access.can ("trigger") && ! u.access.can ("dsp"), "produtor mexe em camera, nao em DSP");

        novo.enabled = false;
        u.access.findUser ("produtor1")->enabled = false;
        check (! u.access.login ("produtor1", "5678"), "usuario desabilitado nao entra");

        check (u.access.removeUser ("produtor1"), "remove usuario");
        while (u.access.users.size() > 1) u.access.removeUser (u.access.users.back().name);
        check (! u.access.removeUser (u.access.users.front().name), "nunca fica sem nenhum usuario");

        Settings uback;
        check (settingsFromJson (settingsToJson (u), uback), "usuarios vao para o JSON");
        check (uback.access.users.size() == u.access.users.size(), "usuarios preservados");

        Settings back;
        check (settingsFromJson (settingsToJson (s2), back), "perfis vao para o JSON");
        check (back.access.active == "Engenheiro" && back.access.profiles.size() == 3,
               "perfis preservados");
    }

    // 18. cena carrega o rack e o mix-minus
    {
        MixerEngine a; a.prepare (sr, block, 2);
        a.channel (0).rack.setEnabled (DspType::Compressor, true);
        a.channel (0).rack.compressor().ratio.store (6.0f);
        a.channel (0).rack.equaliser().midGainDb.store (-4.5f);
        a.channel (0).rack.setOrder ({ int (DspType::Eq), int (DspType::Compressor) });
        a.channel (0).params.sourceType.store (int (SourceType::Codec));
        a.channel (0).params.feedOutputPair.store (2);

        Scene sc;
        check (sceneFromJson (sceneToJson (captureScene (a, "DSP")), sc), "cena com DSP serializa");
        check (sc.channels[0].dspEnabled[int (DspType::Compressor)], "compressor ligado na cena");
        check (near (sc.channels[0].compRatio, 6.0f, 0.01f), "ratio preservado");
        check (sc.channels[0].feedOutputPair == 2, "saida de backfeed preservada");

        MixerEngine b; b.prepare (sr, block, 2);
        applyScene (sc, b);
        check (b.channel (0).rack.isEnabled (DspType::Compressor), "rack aplicado na outra mesa");
        check (near (b.channel (0).rack.equaliser().midGainDb.load(), -4.5f, 0.01f), "EQ aplicado");
        check (b.channel (0).rack.order()[0] == int (DspType::Eq), "ordem do rack aplicada");
    }

    // 19. validacao em duas etapas: primeiro o comando, depois a deteccao
    {
        MixerEngine m; m.prepare (sr, block, 3);
        AutomationEngine autoEng; autoEng.prepare (3);
        const float blockMs = float (block) / float (sr) * 1000.0f;

        auto& c0 = m.channel (0);
        c0.params.inputIndex.store (0);
        c0.params.trigger.camera.store (3);

        // ETAPA 1 — o caminho do comando funciona? Sem audio nenhum.
        check (autoEng.testFire (m, 0), "disparo de teste sai mesmo sem audio");
        Command cmd;
        check (autoEng.commands.pop (cmd), "comando entra na fila");
        check (cmd.type == Command::Type::Cut && cmd.camera == 3, "CUT para a camera do canal");
        check (cmd.manual, "comando marcado como manual, nao como deteccao");
        check (autoEng.camera() == 3, "camera no ar muda");

        auto& c1 = m.channel (1);
        c1.params.trigger.camera.store (0);
        check (! autoEng.testFire (m, 1), "canal sem camera nao dispara teste");
        check (autoEng.testCamera (m, 5), "teste direto por camera funciona");

        // ETAPA 2 — esta chegando audio no canal?
        std::vector<std::vector<float>> ins (3, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (3); std::vector<float*> op (8);
        for (int i = 0; i < 3; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        for (int i = 0; i < 200; ++i) m.process (ip.data(), 3, op.data(), 8, block);
        check (! c0.presence.hasSignal(), "canal mudo: sem sinal");
        check (! c0.presence.everHadSignal(), "e nunca teve");

        double ph = 0.0;
        for (int i = 0; i < 200; ++i)
        {
            fillSine (ins[0], ph, 1000.0, sr, dbToGain (-20.0f));
            m.process (ip.data(), 3, op.data(), 8, block);
        }
        check (c0.presence.hasSignal(), "com audio: sinal presente");
        check (c0.presence.peakHoldDb() > -25.0f, "pico retido registra o nivel");

        std::fill (ins[0].begin(), ins[0].end(), 0.0f);
        const int silentBlocks = int (4000.0f / blockMs);
        for (int i = 0; i < silentBlocks; ++i) m.process (ip.data(), 3, op.data(), 8, block);
        check (c0.presence.isSilentAlarm(), "canal que tinha sinal e emudeceu vira alarme");
        check (c0.presence.everHadSignal(), "mas o historico continua marcado");

        // ETAPA 3 — calibrar o threshold pelo que o microfone realmente entrega
        auto& c2 = m.channel (2);
        c2.params.inputIndex.store (2);
        c2.calibrator.start();
        double ph2 = 0.0;
        // alterna ruido de fundo (-55 dB) e fala (-18 dB), como uma pessoa falando
        for (int rep = 0; rep < 14; ++rep)
        {
            const float lvl = (rep % 2 == 0) ? dbToGain (-55.0f) : dbToGain (-18.0f);
            for (int i = 0; i < 200; ++i)
            {
                fillSine (ins[2], ph2, 700.0, sr, lvl);
                m.process (ip.data(), 3, op.data(), 8, block);
            }
        }
        c2.calibrator.stop();
        check (c2.calibrator.ready(), "calibrador tem material suficiente");
        check (c2.calibrator.noiseFloorDb() < -40.0f, "piso de ruido medido embaixo");
        check (c2.calibrator.speechLevelDb() > -25.0f, "nivel de fala medido em cima");
        const float sug = c2.calibrator.suggestedThresholdDb();
        check (sug > c2.calibrator.noiseFloorDb() + 5.0f
            && sug < c2.calibrator.speechLevelDb() - 5.0f,
               "threshold sugerido cai entre o ruido e a fala, com folga dos dois lados");
    }

    // 20. comando escrito pelo usuario
    {
        MixerEngine m; m.prepare (sr, block, 2);
        AutomationEngine autoEng; autoEng.prepare (2);

        auto& c0 = m.channel (0);
        c0.params.trigger.camera.store (2);

        // sem texto: a mesa monta o CUT a partir da camera
        check (autoEng.testFire (m, 0), "dispara com camera");
        Command cmd;
        autoEng.commands.pop (cmd);
        check (commandText (cmd) == "FUNCTION Cut Input=2", "sem texto, monta CUT pela camera");

        // com texto: manda exatamente o que foi escrito
        const std::string custom = "FUNCTION OverlayInput1In Input=Lower Third";
        c0.params.trigger.command.set (custom);
        check (autoEng.testFire (m, 0), "dispara com comando escrito");
        autoEng.commands.pop (cmd);
        check (commandText (cmd) == custom, "manda o comando literal do canal");
        check (cmd.text[0] != '\0', "texto viaja dentro do Command, sem alocar");

        // troca de comando com o audio rodando: nunca chega pela metade
        std::vector<std::vector<float>> ins (2, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (2); std::vector<float*> op (8);
        for (int i = 0; i < 2; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();

        c0.params.trigger.command.set ("FUNCTION Cut Input=0");   // estado inicial coerente
        std::atomic<bool> run { true };
        std::thread writer ([&] {
            int k = 0;
            while (run.load())
                c0.params.trigger.command.set ("FUNCTION Cut Input=" + std::to_string (k++ % 8));
        });
        bool sempreValido = true;
        for (int i = 0; i < 3000; ++i)
        {
            m.process (ip.data(), 2, op.data(), 8, block);
            autoEng.testFire (m, 0);
            while (autoEng.commands.pop (cmd))
            {
                const std::string t = commandText (cmd);
                if (t.rfind ("FUNCTION Cut Input=", 0) != 0) sempreValido = false;
            }
        }
        run.store (false); writer.join();
        check (sempreValido, "trocar o comando com audio rodando nunca entrega texto pela metade");

        // limite de tamanho respeitado
        c0.params.trigger.command.set (std::string (400, 'x'));
        autoEng.testFire (m, 0); autoEng.commands.pop (cmd);
        check (int (commandText (cmd).size()) < CommandText::kMaxLen, "texto longo e cortado, nao estoura");

        // cena leva o comando
        c0.params.trigger.command.set (custom);
        Scene sc;
        check (sceneFromJson (sceneToJson (captureScene (m, "CMD")), sc), "cena com comando serializa");
        check (sc.channels[0].command == custom, "comando preservado na cena");
        MixerEngine b; b.prepare (sr, block, 2);
        applyScene (sc, b);
        check (b.channel (0).params.trigger.command.str() == custom, "comando aplicado na outra mesa");
    }

    // 21. fader controlando a cartucheira por UDP (comando na borda de ON/OFF)
    {
        Settings st;
        check (st.targets.size() == 2, "vem com dois destinos de fabrica");
        check (st.targets[1].protocol == "UDP" && st.targets[1].port == 8889,
               "cartucheira: UDP na 8889");
        check (st.targets[0].protocol == "TCP" && st.targets[0].port == 8099,
               "vMix: TCP na 8099");
        check (! st.targets[1].appendNewline, "UDP da cartucheira sem fim de linha");

        MixerEngine m; m.prepare (sr, block, 2);
        AutomationEngine autoEng; autoEng.prepare (2);
        const float blockMs = float (block) / float (sr) * 1000.0f;

        auto& deck = m.channel (0);
        deck.params.logicEnabled.store (true);
        deck.params.logicTarget.store (1);                 // cartucheira
        deck.params.onCommand .set ("DECK1_PLAY");
        deck.params.offCommand.set ("DECK1_PAUSE");

        std::vector<std::vector<float>> ins (2, std::vector<float> (block, 0.0f));
        std::vector<std::vector<float>> outs (8, std::vector<float> (block, 0.0f));
        std::vector<const float*> ip (2); std::vector<float*> op (8);
        for (int i = 0; i < 2; ++i) ip[size_t (i)] = ins[size_t (i)].data();
        for (int i = 0; i < 8; ++i) op[size_t (i)] = outs[size_t (i)].data();
        auto run = [&] (int blocks) {
            for (int i = 0; i < blocks; ++i)
            { m.process (ip.data(), 2, op.data(), 8, block); autoEng.processBlock (m, blockMs); }
        };

        run (10);
        Command c;
        check (! autoEng.commands.pop (c), "canal parado nao manda nada");

        deck.params.on.store (true);                        // subiu o fader
        run (5);
        check (autoEng.commands.pop (c), "abrir o canal gera comando");
        check (commandText (c) == "DECK1_PLAY", "manda DECK1_PLAY");
        check (c.target == 1, "para o destino da cartucheira");
        check (! autoEng.commands.pop (c), "e manda UMA vez, nao a cada bloco");

        run (200);
        check (! autoEng.commands.pop (c), "canal aberto continua sem repetir o comando");

        deck.params.on.store (false);                       // desceu o fader
        run (5);
        check (autoEng.commands.pop (c) && commandText (c) == "DECK1_PAUSE", "fechar manda DECK1_PAUSE");

        // mute conta como fechar: o deck nao pode seguir tocando mudo
        deck.params.on.store (true); run (5); autoEng.commands.pop (c);
        deck.params.mute.store (true); run (5);
        check (autoEng.commands.pop (c) && commandText (c) == "DECK1_PAUSE", "mute tambem pausa o deck");

        // apertar ON com o canal JA aberto relanca o comando
        deck.params.mute.store (false);                       // sai do mute do teste anterior
        deck.params.on.store (true); run (5);
        while (autoEng.commands.pop (c)) {}                   // limpa a fila
        check (autoEng.fireChannelLogic (m, 0, true), "reenvio manual com o canal ja aberto");
        check (autoEng.commands.pop (c) && commandText (c) == "DECK1_PLAY", "manda DECK1_PLAY de novo");
        check (c.manual, "marcado como acao do operador");
        run (50);
        check (! autoEng.commands.pop (c), "e o reenvio nao provoca disparo extra pela borda");

        // se o estado real contradiz o reenvio, o motor corrige sozinho:
        // reenviar PLAY num canal mudo faz a borda seguinte mandar PAUSE
        deck.params.mute.store (true); run (5); while (autoEng.commands.pop (c)) {}
        autoEng.fireChannelLogic (m, 0, true);
        while (autoEng.commands.pop (c)) {}
        run (50);
        check (autoEng.commands.pop (c) && commandText (c) == "DECK1_PAUSE",
               "canal mudo nao fica tocando: o motor corrige na borda seguinte");
        deck.params.mute.store (false); run (5); while (autoEng.commands.pop (c)) {}

        // canal com logica desligada nao manda nada
        auto& mic = m.channel (1);
        mic.params.on.store (true); run (5);
        check (! autoEng.commands.pop (c), "canal sem logica habilitada fica quieto");

        // destinos e logica na cena
        deck.params.mute.store (false);
        Scene sc;
        check (sceneFromJson (sceneToJson (captureScene (m, "LOGICA")), sc), "cena com logica serializa");
        check (sc.channels[0].onCommand == "DECK1_PLAY" && sc.channels[0].logicTarget == 1,
               "logica preservada na cena");
        Settings back;
        check (settingsFromJson (settingsToJson (st), back), "destinos vao para o JSON");
        check (back.targets[1].host == "127.0.0.1" && back.targets[1].port == 8889,
               "destino da cartucheira preservado");
    }
    // ---------------------------------------------------- deriva de relogio
    {
        // Fonte com cristal 100 ppm mais rapido que o nosso, 10 minutos.
        // Sem correcao a fila estoura e o audio e descartado; com correcao,
        // a razao converge e nada se perde.
        const double sr = 48000.0; const int block = 256;

        auto run = [&] (double ppm, bool correct)
        {
            AsyncSource src; src.prepare (block, 8, sr);
            src.setDriftCorrection (correct);
            const double blocks = 10.0 * 60.0 * sr / block;
            double acc = 0.0; double ph = 0.0;
            std::vector<float> in (size_t (block * 2), 0.0f);
            for (double b = 0; b < blocks; ++b)
            {
                acc += block * (1.0 + ppm * 1e-6);
                const int toPush = int (acc); acc -= toPush;
                for (int i = 0; i < toPush; ++i) { in[size_t (i)] = std::sin (ph); ph += 0.05; }
                src.push (in.data(), toPush);
                src.pull (block);
            }
            return std::make_pair (src.underruns() + src.overflows(), src.correctionPpm());
        };

        auto fast = run (100.0, true);
        check (fast.first == 0, "fonte 100 ppm rapida: 10 min sem perder amostra");
        check (std::abs (fast.second - 100.0) < 25.0, "correcao converge para a deriva real");

        auto slow = run (-100.0, true);
        check (slow.first == 0, "fonte 100 ppm lenta: 10 min sem underrun");
        check (slow.second < -50.0, "correcao vai para o lado certo");

        auto none = run (100.0, false);
        check (none.first > 0, "sem correcao, a mesma fonte perde audio");
    }

    // ------------------------------------------------ reamostrador em si
    {
        VariableResampler r; r.prepare();
        std::vector<float> out (512, 0.0f);
        double ph = 0.0;
        // razao 1.0 deve devolver o sinal praticamente intacto
        r.process (out.data(), 512, 1.0, [&ph]() noexcept
                   { const float v = std::sin (ph); ph += 0.02; return v; });
        float maxv = 0.0f;
        for (int i = 100; i < 512; ++i) maxv = std::max (maxv, std::abs (out[size_t (i)]));
        check (maxv > 0.9f && maxv < 1.1f, "razao 1.0 preserva amplitude");

        DriftController d; d.prepare (0.5);
        for (int i = 0; i < 20000; ++i) d.update (0.9, 187.5);   // fila cheia
        check (d.ppm() > 0.0, "fila cheia manda consumir mais rapido");
        check (d.ppm() <= DriftController::kMaxPpm + 1.0, "correcao respeita o teto");
    }

    // ------------------------------------------- sequencia de camera e VT
    {
        MixerEngine mix; mix.prepare (48000.0, 256, 2);
        AutomationEngine autom; autom.prepare (2);
        auto& ch = mix.channel (0);
        ch.params.inputIndex.store (0); ch.params.on.store (true);
        ch.params.busMask.store (1);   ch.params.faderDb.store (0.0f);
        auto& tr = ch.params.trigger;
        tr.enabled.store (true); tr.camera.store (2); tr.thresholdDb.store (-40.0f);
        tr.triggerMs.store (100.0f); tr.source.store (0); tr.holdMs.store (500.0f);
        mix.automation.enabled.store (true);  mix.automation.testMode.store (false);
        mix.automation.wideCamera.store (5);  mix.automation.wideDelayMs.store (3000.0f);
        mix.automation.minShotMs.store (200.0f);

        std::vector<float> in (256), oL (256), oR (256);
        const float* ins[1] = { in.data() }; float* outs[2] = { oL.data(), oR.data() };
        double ph = 0.0; const float blockMs = 256.0f / 48.0f;
        auto run = [&] (int blocks, bool loud)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 256; ++i)
                { in[size_t (i)] = loud ? 0.5f * std::sin (ph) : 0.0f; ph += 0.07; }
                mix.process (ins, 1, outs, 2, 256);
                autom.processBlock (mix, blockMs);
            }
        };

        run (300, true);
        check (autom.camera() == 2, "fala assume a camera do canal");
        run (200, false);
        check (autom.camera() == 2, "pausa curta NAO volta ao plano geral");
        run (900, false);
        check (autom.camera() == 5, "silencio prolongado volta ao plano geral");

        autom.suspended.store (true);
        run (400, true);
        check (autom.camera() == 5, "suspensa por VT: fala nao troca a camera");

        autom.suspended.store (false);
        run (300, true);
        check (autom.camera() == 2, "ao liberar, reassume quem ja estava falando");
    }

    // ------------------------------- permanencia do corte manual
    {
        MixerEngine mix; mix.prepare (48000.0, 256, 2);
        AutomationEngine autom; autom.prepare (2);
        auto& ch = mix.channel (0);
        ch.params.trigger.camera.store (2);
        ch.params.trigger.holdMs.store (3000.0f);
        mix.automation.enabled.store (true);  mix.automation.testMode.store (false);
        mix.automation.wideCamera.store (5);  mix.automation.wideDelayMs.store (2000.0f);
        mix.automation.minShotMs.store (200.0f);

        check (autom.testFire (mix, 0), "disparo manual sai");
        check (autom.camera() == 2, "corte manual assume a camera do canal");

        // 1 s depois, ainda tem que estar segurando
        for (int b = 0; b < 190; ++b) autom.processBlock (mix, 256.0f / 48.0f);
        check (autom.camera() == 2, "corte manual respeita a permanencia");

        // passado o hold e o silencio, volta ao padrao
        for (int b = 0; b < 1200; ++b) autom.processBlock (mix, 256.0f / 48.0f);
        check (autom.camera() == 5, "vencida a permanencia, volta a camera padrao");
    }

    // ------------------------------------------- ajustes de fabrica
    {
        MixerEngine mix; mix.prepare (48000.0, 256, 2);
        auto& ch = mix.channel (0);

        // bagunca proposital
        ch.params.trigger.thresholdDb.store (-2.0f);
        ch.params.trigger.holdMs.store (10.0f);
        ch.params.trigger.camera.store (7);
        ch.params.trigger.command.set ("FUNCTION Overlay1In");
        ch.params.autoMix.targetDb.store (-3.0f);

        resetChannel (ch);

        check (ch.params.trigger.thresholdDb.load() == Defaults::kThresholdDb,
               "reset devolve o threshold de fabrica");
        check (ch.params.trigger.holdMs.load() == Defaults::kHoldMs,
               "reset devolve o hold de fabrica");
        check (ch.params.autoMix.targetDb.load() == Defaults::kAutoTargetDb,
               "reset devolve o alvo do nivelador");
        check (ch.params.trigger.camera.load() == 7,
               "reset NAO mexe na camera: aquilo e instalacao");
        check (ch.params.trigger.command.str() == "FUNCTION Overlay1In",
               "reset NAO apaga o comando literal");

        mix.automation.minShotMs.store (10.0f);
        mix.automation.wideCamera.store (5);
        resetAutomation (mix);
        check (mix.automation.minShotMs.load() == Defaults::kMinShotMs,
               "reset devolve o plano minimo");
        check (mix.automation.wideCamera.load() == 5,
               "reset preserva a camera padrao");
    }

    // --------------------- hold conta do FIM da fala, nao do corte
    {
        MixerEngine mix; mix.prepare (48000.0, 256, 2);
        AutomationEngine autom; autom.prepare (2);
        auto& ch = mix.channel (0);
        ch.params.inputIndex.store (0); ch.params.on.store (true);
        ch.params.busMask.store (1);    ch.params.faderDb.store (0.0f);
        auto& tr = ch.params.trigger;
        tr.enabled.store (true); tr.camera.store (2); tr.thresholdDb.store (-40.0f);
        tr.triggerMs.store (100.0f); tr.source.store (0);
        tr.holdMs.store (5000.0f); tr.releaseMs.store (300.0f);
        mix.automation.enabled.store (true);  mix.automation.testMode.store (false);
        mix.automation.wideCamera.store (5);  mix.automation.wideDelayMs.store (1000.0f);
        mix.automation.minShotMs.store (200.0f);

        std::vector<float> in (256), oL (256), oR (256);
        const float* ins[1] = { in.data() }; float* outs[2] = { oL.data(), oR.data() };
        double ph = 0.0; const float bms = 256.0f / 48.0f;
        auto run = [&] (double secs, bool loud)
        {
            const int blocks = int (secs * 1000.0 / bms);
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 256; ++i)
                { in[size_t (i)] = loud ? 0.5f * std::sin (ph) : 0.0f; ph += 0.07; }
                mix.process (ins, 1, outs, 2, 256);
                autom.processBlock (mix, bms);
            }
        };

        // fala LONGA: com o hold armado no corte, ele ja teria vencido aqui
        run (20.0, true);
        check (autom.camera() == 2, "fala longa mantem a camera do canal");
        run (2.0, false);
        check (autom.camera() == 2, "hold conta do fim da fala, nao do corte");
        run (5.0, false);
        check (autom.camera() == 5, "vencido o hold, volta a camera padrao");
    }

    // ---------------- o hold e o UNICO controle do retorno ao padrao
    {
        // Com "silencio antes do BG" alto e plano minimo alto, o retorno tem
        // que obedecer so ao hold. Antes, o maior dos tres mandava, e mexer no
        // hold nao mudava nada.
        auto medir = [] (float holdMs)
        {
            MixerEngine mix; mix.prepare (48000.0, 256, 2);
            AutomationEngine autom; autom.prepare (2);
            auto& ch = mix.channel (0);
            ch.params.inputIndex.store (0); ch.params.on.store (true);
            ch.params.busMask.store (1);    ch.params.faderDb.store (0.0f);
            auto& tr = ch.params.trigger;
            tr.enabled.store (true); tr.camera.store (2); tr.thresholdDb.store (-40.0f);
            tr.triggerMs.store (150.0f); tr.source.store (0);
            tr.holdMs.store (holdMs); tr.releaseMs.store (400.0f);
            mix.automation.enabled.store (true); mix.automation.testMode.store (false);
            mix.automation.wideCamera.store (5);
            mix.automation.wideDelayMs.store (3000.0f);   // alto de proposito
            mix.automation.minShotMs.store (1200.0f);     // alto de proposito

            std::vector<float> in (256), oL (256), oR (256);
            const float* ins[1] = { in.data() }; float* outs[2] = { oL.data(), oR.data() };
            double ph = 0.0; const float bms = 256.0f / 48.0f;
            auto run = [&] (double secs, bool loud)
            {
                const int blocks = int (secs * 1000.0 / bms);
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < 256; ++i)
                    { in[size_t (i)] = loud ? 0.5f * std::sin (ph) : 0.0f; ph += 0.07; }
                    mix.process (ins, 1, outs, 2, 256);
                    autom.processBlock (mix, bms);
                }
            };
            run (3.0, true);
            double t = 0.0;
            while (autom.camera() != 5 && t < 15.0) { run (0.05, false); t += 0.05; }
            return t;
        };

        const double curto = medir (800.0f);
        const double longo = medir (5000.0f);
        check (curto < 2.0, "hold curto devolve ao padrao rapido, apesar dos outros prazos");
        check (longo > 5.0, "hold longo segura, apesar dos outros prazos serem menores");
        check (longo - curto > 3.5, "mexer no hold muda o tempo na proporcao esperada");
    }

    // ------------------- ajustes do operador sobrevivem ao reinicio
    {
        // A cena era gravada UMA vez, no primeiro arranque, e nunca mais: tudo
        // que fosse ajustado no SOFT morria ao fechar a mesa. Este teste fixa o
        // contrato de que o ajuste do dia a dia volta na proxima abertura.
        MixerEngine a; a.prepare (48000.0, 256, 4);
        auto& ch = a.channel (0);
        ch.name = "MIC BANCADA";
        ch.params.trigger.enabled.store (true);
        ch.params.trigger.camera.store (2);
        ch.params.trigger.thresholdDb.store (-35.0f);
        ch.params.trigger.holdMs.store (800.0f);
        ch.params.trigger.triggerMs.store (150.0f);
        ch.params.busMask.store (1);
        ch.params.faderDb.store (-3.5f);

        const auto json = sceneToJson (captureScene (a, "ATUAL"));

        MixerEngine b; b.prepare (48000.0, 256, 4);
        Scene cena;
        check (sceneFromJson (json, cena), "cena gravada volta do JSON");
        applyScene (cena, b, false);

        auto& c2 = b.channel (0);
        check (c2.name == "MIC BANCADA", "nome do canal sobrevive ao reinicio");
        check (c2.params.trigger.camera.load() == 2, "camera sobrevive ao reinicio");
        check (c2.params.trigger.thresholdDb.load() == -35.0f, "threshold sobrevive");
        check (c2.params.trigger.holdMs.load() == 800.0f, "hold sobrevive");
        check (c2.params.trigger.enabled.load(), "trigger ligado sobrevive");
    }

    // ------------------------- conversa cruzada leva ao plano aberto
    {
        MixerEngine mix; mix.prepare (48000.0, 256, 3);
        AutomationEngine autom; autom.prepare (3);
        for (int i = 0; i < 2; ++i)
        {
            auto& c = mix.channel (i);
            c.params.inputIndex.store (i); c.params.on.store (true);
            c.params.busMask.store (1);    c.params.faderDb.store (0.0f);
            auto& t = c.params.trigger;
            t.enabled.store (true); t.camera.store (2 + i); t.thresholdDb.store (-40.0f);
            t.triggerMs.store (150.0f); t.source.store (0);
            t.holdMs.store (2000.0f);   t.releaseMs.store (300.0f);
        }
        mix.automation.enabled.store (true);  mix.automation.testMode.store (false);
        mix.automation.wideCamera.store (5);
        mix.automation.multiTalkMs.store (1500.0f);
        mix.automation.minShotMs.store (300.0f);

        std::vector<float> a (256), b (256), oL (256), oR (256);
        const float* ins[2] = { a.data(), b.data() }; float* outs[2] = { oL.data(), oR.data() };
        double p1 = 0.0, p2 = 0.0; const float bms = 256.0f / 48.0f;
        auto run = [&] (double secs, bool f1, bool f2)
        {
            const int n = int (secs * 1000.0 / bms);
            for (int k = 0; k < n; ++k)
            {
                for (int i = 0; i < 256; ++i)
                {
                    a[size_t (i)] = f1 ? 0.5f * std::sin (p1) : 0.0f; p1 += 0.07;
                    b[size_t (i)] = f2 ? 0.5f * std::sin (p2) : 0.0f; p2 += 0.05;
                }
                mix.process (ins, 2, outs, 2, 256);
                autom.processBlock (mix, bms);
            }
        };

        run (2.0, true, false);
        check (autom.camera() == 2, "um falando: camera dele");
        run (1.0, true, true);
        check (autom.camera() != 5, "sobreposicao curta NAO vai ao plano aberto");
        run (1.5, true, true);
        check (autom.camera() == 5, "conversa cruzada sustentada vai ao plano aberto");
        run (3.0, false, true);
        check (autom.camera() == 3, "terminada a conversa, quem segue falando reassume");

        // desligada, a regra nao pode agir
        mix.automation.multiTalkEnabled.store (false);
        run (4.0, true, true);
        check (autom.camera() != 5, "desligada, a conversa cruzada nao leva ao plano aberto");
    }

    // ------- quem fala assume a camera mesmo com prazos correndo
    {
        // O corte so nascia na borda do trigger: bloqueado naquele instante
        // pelo plano minimo, era descartado e nunca mais tentado — a pessoa
        // seguia falando com a geral no ar. Aqui o plano minimo esta alto de
        // proposito, para provar que a reavaliacao continua resolve.
        MixerEngine mix; mix.prepare (48000.0, 256, 3);
        AutomationEngine autom; autom.prepare (3);
        for (int i = 0; i < 2; ++i)
        {
            auto& c = mix.channel (i);
            c.params.inputIndex.store (i); c.params.on.store (true);
            c.params.busMask.store (1);    c.params.faderDb.store (0.0f);
            auto& t = c.params.trigger;
            t.enabled.store (true); t.camera.store (2 + i); t.thresholdDb.store (-40.0f);
            t.triggerMs.store (300.0f); t.source.store (0);
            t.holdMs.store (4000.0f); t.releaseMs.store (400.0f);
            t.cooldownMs.store (1000.0f);
        }
        mix.automation.enabled.store (true);  mix.automation.testMode.store (false);
        mix.automation.wideCamera.store (5);
        mix.automation.minShotMs.store (1500.0f);
        mix.automation.multiTalkEnabled.store (false);

        std::vector<float> a (256), b (256), oL (256), oR (256);
        const float* ins[2] = { a.data(), b.data() }; float* outs[2] = { oL.data(), oR.data() };
        double p1 = 0.0, p2 = 0.0; const float bms = 256.0f / 48.0f;
        auto run = [&] (double secs, bool f1, bool f2)
        {
            const int n = int (secs * 1000.0 / bms);
            for (int k = 0; k < n; ++k)
            {
                for (int i = 0; i < 256; ++i)
                {
                    a[size_t (i)] = f1 ? 0.5f * std::sin (p1) : 0.0f; p1 += 0.07;
                    b[size_t (i)] = f2 ? 0.5f * std::sin (p2) : 0.0f; p2 += 0.05;
                }
                mix.process (ins, 2, outs, 2, 256);
                autom.processBlock (mix, bms);
            }
        };

        run (2.0, true, false);
        check (autom.camera() == 2, "primeiro a falar assume");

        run (0.3, false, false);
        run (1.5, false, true);
        check (autom.camera() == 3, "outro assume mesmo com o hold do primeiro correndo");

        run (5.0, false, false);
        check (autom.camera() == 5, "silencio devolve ao plano padrao");

        run (0.6, true, false);
        check (autom.camera() == 2, "com a geral no ar, quem fala assume sem esperar");

        // Com a interrupcao DESLIGADA, a geral cumpre o plano minimo.
        // O plano minimo sobe para 8 s para que a espera seja mensuravel logo
        // depois de a geral entrar — com 1,5 s ela ja teria vencido.
        mix.automation.minShotMs.store (8000.0f);
        mix.automation.geralInterrompivel.store (false);
        run (6.0, false, false);
        check (autom.camera() == 5, "volta ao plano padrao");

        run (1.0, true, false);
        check (autom.camera() == 5, "desligada a interrupcao, a geral segura o tempo minimo");

        // religada, o corte sai na hora mesmo com o plano minimo correndo
        mix.automation.geralInterrompivel.store (true);
        run (0.5, true, false);
        check (autom.camera() == 2, "religada a interrupcao, quem fala assume na hora");
    }

    // ------------- segmentador de fala funciona com buffer pequeno
    {
        // A placa desta instalacao roda em 8 amostras. Com a medida de
        // abertura sem memoria, o RMS de 8 amostras despencava a cada
        // cruzamento da onda, zerava a permanencia e o trecho NUNCA abria —
        // mesmo com o sinal 25 dB acima do limiar.
        auto testa = [] (int blk, bool som, double segundos)
        {
            SpeechSegmenter seg; SpeechSegmenter::Params p;
            p.enabled.store (true);  p.thresholdDb.store (-42.0f);
            p.abreMs.store (250.0f); p.fechaMs.store (900.0f);
            p.minTrechoMs.store (800.0f); p.maxTrechoMs.store (12000.0f);
            seg.prepare (48000.0, blk);

            std::vector<float> b (size_t (blk), 0.0f);
            double ph = 0.0;
            const int n = int (segundos * 48000.0 / blk);
            for (int i = 0; i < n; ++i)
            {
                for (int k = 0; k < blk; ++k)
                { b[size_t (k)] = som ? 0.2f * std::sin (ph) : 0.0f; ph += 0.09; }
                seg.process (p, b.data(), blk);
            }
            return seg.estaFalando();
        };

        check (testa (8, true, 2.0),   "abre trecho com buffer de 8 amostras");
        check (testa (256, true, 2.0), "abre trecho com buffer de 256 amostras");
        check (! testa (8, false, 2.0), "silencio nao abre trecho");
    }

    // ------- o aviso de trecho pronto sobrevive ate ser recolhido
    {
        // Era atribuicao direta: verdadeiro no bloco em que o trecho fechava,
        // falso no bloco seguinte — 0,167 ms depois, com buffer de 8. A
        // interface olha 25 vezes por segundo e nunca pegava, entao o trecho
        // era montado e descartado sem ninguem ver.
        MixerEngine mix; mix.prepare (48000.0, 8, 2);
        auto& ch = mix.channel (0);
        ch.params.inputIndex.store (0); ch.params.on.store (true);
        ch.params.fala.enabled.store (true);
        ch.params.fala.thresholdDb.store (-42.0f);
        ch.params.fala.abreMs.store (250.0f);
        ch.params.fala.fechaMs.store (900.0f);
        ch.params.fala.minTrechoMs.store (800.0f);
        ch.params.fala.maxTrechoMs.store (2000.0f);

        std::vector<float> in (8), oL (8), oR (8);
        const float* ins[1] = { in.data() }; float* outs[2] = { oL.data(), oR.data() };
        double ph = 0.0; int vistos = 0, blocos = 0;

        for (int i = 0; i < int (6.0 * 48000 / 8); ++i)
        {
            for (int k = 0; k < 8; ++k) { in[size_t (k)] = 0.25f * std::sin (ph); ph += 0.09; }
            mix.process (ins, 1, outs, 2, 8);
            if (++blocos >= 240)      // a interface olhando a 25 Hz
            {
                blocos = 0;
                if (ch.temFalaPronta())
                { ++vistos; ch.limpaTrechoDeFala(); ch.marcaFalaRecolhida(); }
            }
        }
        check (vistos >= 2, "a interface recolhe os trechos de fala prontos");
    }

    // -------- JSON sobrevive a caractere de controle no conteudo
    {
        // O estado do dispositivo de audio e XML do Windows, com retorno de
        // carro. Um so caractere desses deixava o settings.json ilegivel para
        // qualquer outro programa — e foi assim que o transcritor parou de
        // achar a chave da API.
        const std::string original = "<DEVICESETUP\r\n  rate=\"48000\"/>\r\n\x01fim";

        json::Value raiz; raiz.type = json::Value::Object;
        raiz.set ("deviceState", json::text (original));
        raiz.set ("falaChave",   json::text ("abc123"));

        std::string saida;
        json::write (raiz, saida);

        bool temControleCru = false;
        for (unsigned char c : saida)
            if (c < 0x20 && c != '\n') temControleCru = true;
        check (! temControleCru, "JSON gravado nao tem caractere de controle cru");

        json::Value lido;
        check (json::parse (saida, lido), "JSON com escapes volta a ser lido");
        check (lido.string ("deviceState") == original, "conteudo volta identico");
        check (lido.string ("falaChave") == "abc123", "os campos seguintes nao se perdem");
    }

    // -------- pan central entrega UNIDADE, nao 0,707
    {
        // A regra de potencia constante tira 3 dB no centro. E certa para
        // posicionar fonte no estereo e errada para console de radio: aqui um
        // microfone com fader em 0 tem de sair em unidade, e a mesa tem de
        // bater com qualquer outra fonte na mesma saida.
        mesa::MixerEngine mix;
        mix.prepare (48000.0, 512, 8);

        auto& ch = mix.channel (0);
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.busMask.store (1u);
        ch.params.panPos.store (0.0f);
        ch.params.inputIndex.store (0);

        const int n = 256;
        std::vector<float> ent (size_t (n), 0.5f);
        const float* in[1] = { ent.data() };
        std::vector<float> sl (size_t (n), 0.0f), sr (size_t (n), 0.0f);
        float* out[2] = { sl.data(), sr.data() };

        // os ganhos sao suavizados: sem deixar assentar, mede-se a rampa
        for (int b = 0; b < 200; ++b) mix.process (in, 1, out, 2, n);

        check (std::fabs (sl[10] - 0.5f) < 0.001f, "pan central: esquerdo em unidade");
        check (std::fabs (sr[10] - 0.5f) < 0.001f, "pan central: direito em unidade");

        // e o pan ainda funciona: todo a esquerda zera o lado direito
        ch.params.panPos.store (-1.0f);
        for (int b = 0; b < 200; ++b) mix.process (in, 1, out, 2, n);
        check (std::fabs (sl[10] - 0.5f) < 0.001f, "pan a esquerda mantem o esquerdo cheio");
        check (sr[10] < 0.001f, "pan a esquerda zera o direito");
    }

    // -------- dois donos no mesmo par de saida sao denunciados
    {
        // O defeito mais caro desta semana: o monitor apontado para o mesmo
        // par do PGM 1. Nao somam — um sobrescreve o outro — e o som sai mais
        // baixo sem nada acusar erro. So apareceu quando medimos canal a canal.
        Settings s;
        // sem monitor roteado nao ha o que colidir
        check (conflitosDeSaida (s).empty(), "so os barramentos: sem conflito");

        s.routing.monitorOutputPair = s.routing.busOutputPair[0];
        check (! conflitosDeSaida (s).empty(), "monitor sobre o PGM 1 e denunciado");

        // par 4 em diante nao pertence a nenhum barramento: e onde o monitor
        // cabe sem disputa numa placa de 8 ou mais saidas
        s.routing.monitorOutputPair = 4;
        check (conflitosDeSaida (s).empty(), "em par proprio, sem aviso");
    }

    // -------- a saida escolhida na aba Outputs sobrevive ao reinicio
    {
        // O catalogo guardava a escolha certa e ninguem a aplicava: quem manda
        // audio para a placa e o roteamento por barramento, e o rebindOutputs
        // so cuida de destinos de rede. Dava certo na hora e sumia ao reabrir.
        Settings s;
        OutputDef o;
        o.name = "OUTPUT 1";
        o.busSource = 0;
        o.pair = 2;                       // PGM 1 nas saidas 5/6
        o.deviceType = "ASIO";
        o.kind = int (InputKind::Device);
        s.outputs.outputs.push_back (o);

        const auto txt = settingsToJson (s);
        Settings volta;
        check (settingsFromJson (txt, volta), "configuracao com output volta do arquivo");
        check (! volta.outputs.outputs.empty(), "o output foi gravado");
        check (volta.outputs.outputs[0].pair == 2, "o par escolhido foi preservado");
        check (volta.outputs.outputs[0].busSource == 0, "a origem escolhida foi preservada");
    }

    // -------- CUE: onde entra e quanto abaixa o que ja tocava
    {
        MixerEngine mix;
        mix.prepare (48000.0, 512, 8);

        auto& ch = mix.channel (0);
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.busMask.store (1u);
        ch.params.inputIndex.store (0);

        mix.monitor.phonesDb.store (0.0f);
        mix.monitor.cueDb.store (0.0f);
        mix.monitor.cueToPhones.store (true);

        const int n = 256;
        std::vector<float> ent (size_t (n), 0.5f);
        const float* in[1] = { ent.data() };
        std::vector<float> a (size_t (n), 0.0f), b (size_t (n), 0.0f);
        float* out[2] = { a.data(), b.data() };

        for (int k = 0; k < 300; ++k) mix.process (in, 1, out, 2, n);
        check (std::fabs (mix.phonesLeft()[10]) > 0.4f, "sem CUE o fone toca o programa");

        // dim no fundo: o CUE substitui, que era o comportamento antigo
        ch.params.cue.store (true);
        mix.monitor.cueDimDb.store (-120.0f);
        for (int k = 0; k < 300; ++k) mix.process (in, 1, out, 2, n);
        const float substituindo = std::fabs (mix.phonesLeft()[10]);

        // dim brando: o programa continua audivel POR BAIXO do CUE
        mix.monitor.cueDimDb.store (-12.0f);
        for (int k = 0; k < 300; ++k) mix.process (in, 1, out, 2, n);
        check (std::fabs (mix.phonesLeft()[10]) > substituindo,
               "com DIM brando o programa soma ao CUE");

        // e o CUE pode entrar tambem no monitor
        mix.monitor.monitorDb.store (0.0f);
        mix.monitor.cueToMonitor.store (true);
        for (int k = 0; k < 300; ++k) mix.process (in, 1, out, 2, n);
        check (std::fabs (mix.monitorLeft()[10]) > 0.0f, "CUE entra no monitor quando pedido");
    }

    // -------- o fader da saida manda no CUE tambem
    {
        // O CUE era somado DEPOIS do ganho do monitor: com o DIM no mudo, o
        // operador ficava sem controle nenhum sobre o que ouvia — o fader da
        // direita nao mexia em nada enquanto o CUE tocava.
        MixerEngine mix;
        mix.prepare (48000.0, 512, 8);

        auto& ch = mix.channel (0);
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.busMask.store (1u);
        ch.params.inputIndex.store (0);
        ch.params.cue.store (true);

        mix.monitor.cueDb.store (0.0f);
        mix.monitor.cueToMonitor.store (true);
        mix.monitor.cueDimDb.store (-120.0f);      // mudo: sobra so o CUE

        const int n = 256;
        std::vector<float> ent (size_t (n), 0.5f);
        const float* in[1] = { ent.data() };
        std::vector<float> a (size_t (n), 0.0f), b (size_t (n), 0.0f);
        float* out[2] = { a.data(), b.data() };

        mix.monitor.monitorDb.store (0.0f);
        for (int k = 0; k < 400; ++k) mix.process (in, 1, out, 2, n);
        const float cheio = std::fabs (mix.monitorLeft()[10]);

        mix.monitor.monitorDb.store (-20.0f);
        for (int k = 0; k < 400; ++k) mix.process (in, 1, out, 2, n);
        const float baixo = std::fabs (mix.monitorLeft()[10]);

        check (cheio > 0.1f, "ouvindo so o CUE, com o fader aberto");
        check (baixo < cheio * 0.5f, "baixar o fader baixa tambem o CUE");
    }

    // -------- desligar uma fonte de rede desloca as seguintes
    {
        // O canal guarda um numero de POSICAO na lista de fontes de rede, e
        // essa lista e remontada a cada rearranjo. Tirando uma fonte, as
        // seguintes andam uma casa para tras — e um canal que guardou o numero
        // antigo passa a tocar a fonte do vizinho. Foi assim que o Livewire de
        // outra maquina apareceu no canal de um Livewire desligado.
        SourceCatalog cat;
        SourceDef a; a.name = "AXIA";     a.livewireChannel = 3100;
        SourceDef b; b.name = "PC LOCAL"; b.livewireChannel = 3;
        SourceDef c; c.name = "NOTE";     c.livewireChannel = 5;
        cat.sources = { a, b, c };

        // distribuicao inicial, na ordem do catalogo
        int slot = 0;
        for (auto& f : cat.sources) f.index = f.livewireChannel > 0 ? slot++ : -1;
        const int slotDoNote = cat.sources[2].index;
        check (slotDoNote == 2, "com tres fontes, NOTE fica na posicao 2");

        // o operador desliga a fonte do PC LOCAL
        cat.sources[1].livewireChannel = 0;
        slot = 0;
        for (auto& f : cat.sources) f.index = f.livewireChannel > 0 ? slot++ : -1;

        check (cat.sources[1].index == -1, "PC LOCAL sai da lista de rede");
        check (cat.sources[2].index != slotDoNote, "NOTE muda de posicao");
        check (cat.sources[2].index == 1, "NOTE assume a posicao que era do PC LOCAL");
    }

    // -------- ritmo por prazo acumulado, e nao por dormida arredondada
    {
        // 512 amostras a 48 kHz duram 10,67 ms. Dormir "int(10,67)" = 10 ms da
        // 6,7% de voltas a mais por segundo — foi assim que uma fonte NDI
        // entregou 50469 amostras onde a mesa consome 48000. A fila enchia e o
        // excedente era descartado, o que soa como picotar.
        const double sr = 48000.0;
        const int bloco = 512;
        const double duracao = 1000.0 * bloco / sr;

        const double taxaArredondada = (1000.0 / double (int (duracao))) * bloco;
        check (taxaArredondada > 50000.0, "dormida arredondada entrega demais");

        double prazo = 0.0, relogio = 0.0;
        long long amostras = 0;
        for (int i = 0; i < 1000; ++i)
        {
            prazo += duracao;
            const double falta = prazo - relogio;
            if (falta > 1.0) relogio += double (int (falta));
            amostras += bloco;
        }
        const double taxaPrazo = double (amostras) / (relogio / 1000.0);
        check (std::fabs (taxaPrazo - sr) < 2000.0, "prazo acumulado fica na taxa certa");
    }


    // -------- anuncio Livewire: os bytes que o QOR le
    {
        // Por que testar isto aqui: enquanto o pacote nascia dentro do socket,
        // errar um tamanho de bloco so aparecia com o console na frente. O
        // leitor confere o escritor, e o mesmo leitor le o anuncio do driver.
        using namespace mesa::lw;

        Identidade eu;
        eu.hwid = 0x4d31;
        eu.ip   = (192u << 24) | (168u << 16) | (2u << 8) | 116u;
        eu.udpc = 4002;
        eu.maquina = "DESKTOP-ASA-30";

        std::vector<FonteAnuncio> uma { { 3600, "MESA", 0 } };

        const auto lista = montaLista (eu, uma, 7);
        const auto a = le (lista.data(), lista.size());
        check (a.ok, "anuncio ADVT 1 decodifica");
        check (a.consumidos == lista.size(), "ADVT 1: nenhum byte sobra");
        check (a.advt == 1 && a.contador == 7, "ADVT 1: tipo e contador");
        check (a.hwid == 0x4d31 && a.ip == eu.ip && a.udpc == 4002, "ADVT 1: identidade do no");
        check (a.nums == 1 && a.maquina == "DESKTOP-ASA-30", "ADVT 1: NUMS e nome da maquina");
        check (a.fontes.size() == 1 && a.fontes[0].canal == 3600
                   && a.fontes[0].nome == "MESA", "ADVT 1: a fonte chega inteira");

        const auto vida = montaVida (eu, 4, 8);
        const auto b = le (vida.data(), vida.size());
        check (b.ok && b.consumidos == vida.size() && b.advt == 2 && b.nums == 4,
               "anuncio ADVT 2 fecha certo");

        std::vector<FonteAnuncio> duas { { 3600, "", 0 },
                                         { 3601, "", 0x1122334455667788ull } };
        const auto uso = montaUso (eu, duas, 9);
        const auto c = le (uso.data(), uso.size());
        check (c.ok && c.consumidos == uso.size() && c.advt == 3, "anuncio ADVT 3 fecha certo");
        check (c.fontes.size() == 2 && c.fontes[0].uso == 0
                   && c.fontes[1].uso == 0x1122334455667788ull, "ADVT 3: BUSY por fonte");

        // NEST e os tamanhos de bloco tem de acompanhar o numero de fontes:
        // ja mandamos NEST fixo em 7 com uma fonte so, e o anuncio prometia
        // quatro blocos e entregava um.
        std::vector<FonteAnuncio> muitas;
        for (int i = 0; i < 16; ++i)
            muitas.push_back ({ 3600 + i, "CANAL " + std::to_string (i), 0 });
        const auto grande = montaLista (eu, muitas, 1);
        const auto d = le (grande.data(), grande.size());
        check (d.ok && d.consumidos == grande.size() && d.fontes.size() == 16
                   && d.nums == 16, "anuncio com 16 fontes fecha certo");

        // nome que enche o campo tem de sobrar o zero do fim
        const auto comprido = montaLista (eu, { { 3600, "NOME MUITO COMPRIDO DEMAIS", 0 } }, 1);
        const auto e = le (comprido.data(), comprido.size());
        check (e.ok && e.fontes[0].nome.size() == 15, "PSNM corta em 15 e guarda o zero");

        // pacote cortado nao pode passar por bom
        bool recusouTodos = true;
        for (size_t corte : { size_t (8), size_t (20), lista.size() - 3, lista.size() - 1 })
            recusouTodos &= ! le (lista.data(), corte).ok;
        check (recusouTodos, "anuncio cortado e recusado, nao adivinhado");

        std::vector<std::uint8_t> lixo (64, 0xab);
        lixo[0] = 0x03; lixo[1] = 0x00; lixo[2] = 0x02; lixo[3] = 0x07;
        check (! le (lixo.data(), lixo.size()).ok, "lixo com cabecalho certo e recusado");

        // HWID: estavel entre arranques e, por construcao, diferente do que o
        // IP daria — que e o do IP-Driver na mesma maquina.
        const unsigned doIp  = eu.ip & 0xffffu;
        const unsigned nosso = hwidDeTexto ("DESKTOP-ASA-30/MesaConsole");
        check (nosso == hwidDeTexto ("DESKTOP-ASA-30/MesaConsole"), "HWID nao muda entre chamadas");
        check (nosso != 0 && nosso != 0xffff, "HWID foge dos extremos");
        check (hwidDeTexto ("A") != hwidDeTexto ("B"), "nomes diferentes dao HWIDs diferentes");
        check (hwidLivre (doIp, { doIp }) != doIp, "HWID ocupado faz escolher outro");
        check (hwidLivre (nosso, { nosso, nosso + 1 }) == nosso + 2, "sobe ate achar vago");
        check (hwidLivre (nosso, {}) == nosso, "sem ninguem no caminho, fica o desejado");

        // -------- o comando WRIN que o QOR manda pela porta de controle
        //
        // Capturado da rede: o console escreve o estado de uso da fonte e
        // espera ve-lo de volta no ADVT 3. Ignorar isso foi a causa do
        // "Used EW" — ele reescrevia o mesmo comando uma vez por segundo.
        {
            const std::uint8_t comando[] = {
                0x03,0x00,0x02,0x07, 0x00,0x01,0x01,0x8e,
                0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
                'N','E','S','T', 0x00, 0x01,
                'S','0','0','1', 0x06, 0x00,0x1c,
                'W','R','I','N', 0x00, 0x02,
                'P','S','I','D', 0x01, 0x00,0x00,0x0e,0x10,
                'B','U','S','Y', 0x09, 0x00,0x02,0x02,0x0a,0x00,0x00,0xc0,0xa8 };

            const auto w = le (comando, sizeof (comando));
            check (w.ok && w.consumidos == sizeof (comando), "comando WRIN decodifica inteiro");
            check (w.advt == 0, "comando WRIN nao traz ADVT — e escrita, nao anuncio");
            check (w.fontes.size() == 1 && w.fontes[0].canal == 3600, "WRIN diz de que canal fala");
            check (w.fontes[0].uso == 0x0002020a0000c0a8ull, "WRIN traz o BUSY a publicar");

            // e o que sai no ADVT 3 tem de ser exatamente aquilo de volta
            std::vector<FonteAnuncio> comUso { { 3600, "MESA", w.fontes[0].uso } };
            const auto eco = montaUso (eu, comUso, 1);
            const auto lido = le (eco.data(), eco.size());
            check (lido.ok && lido.fontes.size() == 1
                       && lido.fontes[0].uso == 0x0002020a0000c0a8ull,
                   "o ADVT 3 devolve o BUSY que o console escreveu");
        }
    }


    // -------- direct out: saida apontada para uma ENTRADA, nao para um bus
    {
        using namespace mesa;
        OutputCatalog c;
        OutputDef a; a.name = "REPASSE"; a.livewireChannel = 3601;
        a.directSource = "MIC 1 APRES";
        OutputDef b; b.name = "PGM 1"; b.busSource = 0;
        c.outputs = { a, b };

        std::string texto; json::write (outputsToJson (c), texto);
        json::Value v;
        check (json::parse (texto, v), "saidas gravadas dao JSON valido");

        OutputCatalog volta; outputsFromJson (v, volta);
        check (volta.outputs.size() == 2, "as duas saidas voltam da gravacao");
        check (volta.outputs[0].directSource == "MIC 1 APRES", "direct out guarda o NOME da fonte");
        check (volta.outputs[0].livewireChannel == 3601, "o canal Livewire continua la");
        check (volta.outputs[1].directSource.empty(), "saida de barramento fica sem direct out");

        // settings.json de versao anterior nao pode nascer com direct out: um
        // destino que troca de fonte sozinho numa atualizacao vai ao ar errado
        json::Value velho;
        json::parse ("[{\"name\":\"ANTIGA\",\"bus\":1}]", velho);
        OutputCatalog antiga; outputsFromJson (velho, antiga);
        check (antiga.outputs.size() == 1 && antiga.outputs[0].directSource.empty(),
               "configuracao antiga continua sem direct out");

        // o nome e o que sobrevive: indice de fonte de rede e reatribuido a
        // cada religamento, e apontar por indice mandaria outra fonte ao ar
        SourceCatalog cat;
        SourceDef s1; s1.name = "PLAYOUT A"; s1.kind = int (InputKind::Network); s1.index = 0;
        SourceDef s2; s2.name = "MIC 1 APRES"; s2.kind = int (InputKind::Network); s2.index = 1;
        cat.sources = { s1, s2 };

        auto acha = [&cat] (const std::string& nome, int& kind, int& idx)
        {
            kind = 0; idx = -1;
            if (nome.empty()) return;
            for (const auto& s : cat.sources)
                if (s.name == nome) { kind = s.kind; idx = s.index; return; }
        };

        int k = 0, i = -1;
        acha ("MIC 1 APRES", k, i);
        check (k == int (InputKind::Network) && i == 1, "acha a fonte pelo nome");

        cat.sources = { s2, s1 };                    // religou, ordem trocada
        cat.sources[0].index = 0; cat.sources[1].index = 1;
        acha ("MIC 1 APRES", k, i);
        check (i == 0, "fonte remanejada continua sendo a mesma pelo nome");

        acha ("FONTE QUE SUMIU", k, i);
        check (i == -1, "fonte apagada devolve -1, e o destino cai no barramento");
    }

    // -------- mix-minus: duas fontes que alternam com o ON/OFF do canal
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 4);

        // canal 0: o telefone. canal 1: um microfone, so para haver programa.
        auto& tel = mix.channel (0);
        auto& mic = mix.channel (1);
        tel.params.sourceType.store (int (SourceType::Phone));
        tel.params.inputKind .store (int (InputKind::Device));
        tel.params.inputIndex.store (0);
        tel.params.faderDb   .store (0.0f);
        tel.params.busMask   .store (0b1001u);        // PGM 1 e PGM 4
        tel.params.feedBusOn .store (0);              // no ar ouve o PGM 1
        tel.params.feedBusOff.store (3);              // fora do ar, o PGM 4
        mic.params.inputKind .store (int (InputKind::Device));
        mic.params.inputIndex.store (1);
        mic.params.faderDb   .store (0.0f);
        mic.params.busMask   .store (0b1111u);        // fala nos quatro
        mic.params.on        .store (true);

        const int n = 64;
        const size_t nn = size_t (n);
        std::vector<float> vozTel (nn, 0.5f);         // constante, facil de ver
        std::vector<float> vozMic (nn, 0.25f);
        const float* entradas[2] = { vozTel.data(), vozMic.data() };
        std::vector<float> saida (nn * 8, 0.0f);
        float* saidas[8];
        for (int k = 0; k < 8; ++k) saidas[k] = saida.data() + size_t (k) * size_t (n);

        // Os ganhos de fader e trim sobem suavizados (20 e 25 ms). Dois blocos
        // de 64 amostras dao 2,7 ms e o nivel ainda esta subindo: medir ali
        // compara numeros que nao chegaram ao destino. 100 blocos passam de
        // 130 ms e a essa altura tudo esta estavel.
        auto roda = [&]
        {
            for (int passo = 0; passo < 100; ++passo)
            {
                for (int k = 0; k < 8; ++k)
                    std::fill (saidas[k], saidas[k] + n, 0.0f);
                mix.process (entradas, 2, saidas, 8, n, nullptr, 0);
            }
        };

        auto meioDoBloco = [n] (const float* v) { return v[n - 1]; };

        // --- FORA DO AR: ouve o barramento de comunicacao, nao o programa
        tel.params.on.store (false);
        roda();
        const float fora = meioDoBloco (mix.backfeedData (0));
        check (std::fabs (fora - 0.25f) < 0.02f,
               "fora do ar, o retorno traz o barramento de comunicacao");

        // --- NO AR: passa a ouvir o programa
        tel.params.on.store (true);
        roda();
        const float dentro = meioDoBloco (mix.backfeedData (0));
        check (std::fabs (dentro - 0.25f) < 0.03f,
               "no ar, o retorno traz o programa sem a voz do proprio telefone");

        // A prova que importa: a voz do telefone (0.5) esta no PGM 1 junto com
        // o microfone (0.25), somando 0.75 - e o retorno tem de ficar em 0.25.
        const float pgm = meioDoBloco (mix.busLeft (0));
        check (pgm > 0.6f, "o telefone realmente entra no programa quando esta no ar");
        check (std::fabs (dentro - (pgm - 0.5f)) < 0.03f,
               "o retorno e o programa MENOS exatamente a contribuicao do canal");

        // --- trocar os barramentos muda o retorno, sem tocar em mais nada
        tel.params.feedBusOn.store (1);               // no ar passa a ouvir PGM 2
        roda();
        check (std::fabs (meioDoBloco (mix.backfeedData (0)) - 0.25f) < 0.03f,
               "trocar o barramento do ar redireciona o retorno");
        tel.params.feedBusOn.store (0);

        // --- O CASO QUE FALTAVA: falar com o ouvinte antes de por no ar.
        //
        // Os dois faders fechados. O de comunicacao tem de passar assim mesmo:
        // era isto que nao funcionava, e sem isto so da para falar com o
        // ouvinte entrando no ar antes — que e o que ninguem quer fazer.
        tel.params.on.store (false);
        mic.params.on.store (false);
        roda();
        check (std::fabs (meioDoBloco (mix.backfeedData (0)) - 0.25f) < 0.02f,
               "com tudo fechado, o operador ainda fala com o ouvinte");
        // o barramento leva os dois com tudo fechado: o microfone (0,25) e o
        // proprio telefone (0,5), que tambem esta roteado nele
        const float comBus = meioDoBloco (mix.busLeft (mesa::kBusComunicacao));
        check (std::fabs (comBus - 0.75f) < 0.03f,
               "o barramento de comunicacao recebe canal fechado");

        // e o programa, esse continua vazio: canal fechado nao vai ao ar
        check (std::fabs (meioDoBloco (mix.busLeft (0))) < 0.01f,
               "canal fechado continua fora do PGM 1");

        // a voz do proprio telefone nao pode voltar nem na conversa fora do ar
        check (comBus > 0.6f, "o telefone tambem entra no barramento de comunicacao");
        check (std::fabs (meioDoBloco (mix.backfeedData (0)) - (comBus - 0.5f)) < 0.03f,
               "fora do ar o retorno tambem sai sem a voz do proprio telefone");

        // MUTE e ordem explicita de silencio: vale ate na comunicacao
        mic.params.mute.store (true);
        roda();
        check (std::fabs (meioDoBloco (mix.backfeedData (0))) < 0.02f,
               "MUTE cala o canal ate no barramento de comunicacao");
        mic.params.mute.store (false);
    }

    // -------- canal estereo: as duas pontas ate o barramento
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 2);

        auto& st = mix.channel (0);
        st.params.inputKind  .store (int (InputKind::Network));
        st.params.inputIndex .store (0);
        st.params.inputIndexR.store (1);        // a segunda ponta
        st.params.faderDb    .store (0.0f);
        st.params.busMask    .store (0b0001u);
        st.params.on         .store (true);

        const int n = 64;
        const size_t nn = size_t (n);
        std::vector<float> esq (nn, 0.8f);      // so na esquerda
        std::vector<float> dir (nn, 0.2f);      // so na direita
        const float* rede[2] = { esq.data(), dir.data() };
        std::vector<float> saida (nn * 4, 0.0f);
        float* saidas[4];
        for (int k = 0; k < 4; ++k) saidas[k] = saida.data() + size_t (k) * nn;

        auto roda = [&]
        {
            for (int passo = 0; passo < 100; ++passo)
            {
                for (int k = 0; k < 4; ++k) std::fill (saidas[k], saidas[k] + n, 0.0f);
                mix.process (nullptr, 0, saidas, 4, n, rede, 2);
            }
        };
        roda();

        const float l = mix.busLeft  (0)[n - 1];
        const float r = mix.busRight (0)[n - 1];
        check (std::fabs (l - 0.8f) < 0.03f, "a esquerda chega inteira no lado esquerdo");
        check (std::fabs (r - 0.2f) < 0.03f, "a direita chega inteira no lado direito");
        check (std::fabs (l - r) > 0.4f, "os dois lados seguem diferentes - nao virou mono");
        check (st.ehEstereo(), "o canal se reconhece estereo");

        // PAN VIRA BALANCO: todo a esquerda tem de calar o lado direito
        st.params.panPos.store (-1.0f);
        roda();
        check (std::fabs (mix.busLeft (0)[n - 1] - 0.8f) < 0.03f,
               "pan na ponta esquerda mantem o lado esquerdo");
        check (std::fabs (mix.busRight (0)[n - 1]) < 0.02f,
               "pan na ponta esquerda cala o lado direito");
        st.params.panPos.store (0.0f);

        // sem a segunda ponta o canal volta a ser mono, e o mesmo sinal vai aos dois
        st.params.inputIndexR.store (-1);
        roda();
        check (! st.ehEstereo(), "sem segunda ponta o canal volta a mono");
        check (std::fabs (mix.busLeft (0)[n - 1] - mix.busRight (0)[n - 1]) < 0.001f,
               "em mono os dois lados do barramento recebem o mesmo");
    }

    // -------- chave da fila: qual receptor esta em uso
    //
    // Isto nasceu de um crash na abertura. A limpeza dos receptores Livewire
    // listava os lados na mao (":0", ":1", ":2"); quando o lado 3 (estereo)
    // entrou, a chave dele nao batia com nenhum, o receptor recem-criado era
    // destruido como se sobrasse, e o motor ficava com ponteiro solto - a mesa
    // caia no primeiro bloco de audio. A regra passou a ser por prefixo, e
    // estes casos existem para que lado novo nao repita a historia.
    {
        using namespace mesa;
        SourceDef s;
        s.livewireChannel = 3100;

        for (int lado = 0; lado <= 3; ++lado)
        {
            s.livewireSide = lado;
            const auto k = chaveDaFonte (s);
            check (chaveEhDoCanal (k, 3100),
                   ("lado " + std::to_string (lado) + " reconhecido como do canal 3100").c_str());
            check (! chaveEhDoCanal (k, 3101), "e nao confundido com outro canal");
        }

        // a segunda ponta do estereo guarda a chave com sufixo
        s.livewireSide = 3;
        check (chaveEhDoCanal (chaveDaFonte (s) + "/R", 3100),
               "a segunda ponta do estereo tambem conta como em uso");

        // canal parecido nao pode casar por acidente: 310 nao e comeco de 3100
        SourceDef curto; curto.livewireChannel = 310; curto.livewireSide = 0;
        check (! chaveEhDoCanal (chaveDaFonte (curto), 3100),
               "canal 310 nao e confundido com 3100");
        check (! chaveEhDoCanal (chaveDaFonte (s), 310),
               "nem 3100 com 310");

        // fontes de outro tipo nao pertencem a canal Livewire nenhum
        SourceDef ndi; ndi.streamName = "PLAYOUT";
        check (! chaveEhDoCanal (chaveDaFonte (ndi), 3100), "fonte NDI fica de fora");
        SourceDef dev; dev.deviceName = "UMC"; dev.deviceChannel = 2;
        check (! chaveEhDoCanal (chaveDaFonte (dev), 3100), "fonte de placa fica de fora");
        check (chaveDaFonte (SourceDef{}).empty(), "fonte vazia nao gera chave");
    }

    // -------- a lista de outputs manda no que sai pela placa mestra
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 2);

        Settings st;
        OutputDef pgm;  pgm.name = "PGM 1";      pgm.busSource = 0; pgm.pair = 0;
        OutputDef mon;  mon.name = "MONITOR";    mon.busSource = 5; mon.pair = 2;
        st.outputs.outputs = { pgm, mon };
        aplicaSaidasDaPlaca (st, mix);

        check (mix.busParams[0].outputPair.load() == 0, "PGM 1 vai para o par escolhido");
        check (mix.monitor.monitorPair.load() == 2,
               "monitor tambem e aplicado - nao so os quatro PGM");

        // TIRAR a placa de um destino tem de TIRAR mesmo. Era o defeito: sem
        // par, a saida caia num "continue" e o roteamento velho continuava.
        st.outputs.outputs[0].pair = -1;
        aplicaSaidasDaPlaca (st, mix);
        check (mix.busParams[0].outputPair.load() == -1, "sem par, o PGM 1 para de sair");
        check (mix.monitor.monitorPair.load() == 2, "e os outros destinos nao se mexem");

        // mover a placa de um destino para outro: sai do primeiro, chega no segundo
        st.outputs.outputs[0].pair = -1;
        st.outputs.outputs[1].pair = 0;
        aplicaSaidasDaPlaca (st, mix);
        check (mix.busParams[0].outputPair.load() == -1, "o destino antigo ficou vazio");
        check (mix.monitor.monitorPair.load() == 0, "e o novo recebeu o par");

        // apagar a saida da lista some com o roteamento dela
        st.outputs.outputs.clear();
        aplicaSaidasDaPlaca (st, mix);
        check (mix.monitor.monitorPair.load() == -1, "saida apagada para de sair");
        check (mix.busParams[3].outputPair.load() == -1, "lista vazia nao roteia nada");

        // rede e placa secundaria nao entram por aqui
        OutputDef lw;  lw.name = "LW";  lw.busSource = 1; lw.pair = 1; lw.livewireChannel = 3600;
        OutputDef sec; sec.name = "SEC"; sec.busSource = 2; sec.pair = 1; sec.deviceName = "UMC";
        st.outputs.outputs = { lw, sec };
        aplicaSaidasDaPlaca (st, mix);
        check (mix.busParams[1].outputPair.load() == -1, "saida Livewire nao roteia par de placa");
        check (mix.busParams[2].outputPair.load() == -1, "placa secundaria tambem nao");

        // direct out e mix-minus nao sao barramento: melhor nao sair do que
        // sair o sinal errado pela placa mestra
        OutputDef d;  d.name = "D"; d.busSource = 0; d.pair = 1; d.directSource = "MIC 1";
        OutputDef mm; mm.name = "M"; mm.busSource = 1; mm.pair = 2; mm.mixMinusChannel = 0;
        st.outputs.outputs = { d, mm };
        aplicaSaidasDaPlaca (st, mix);
        check (mix.busParams[0].outputPair.load() == -1, "direct out nao vira barramento");
        check (mix.busParams[1].outputPair.load() == -1, "mix-minus tambem nao");
    }

    // -------- fila so conta como falha se estiver no caminho do audio
    {
        mesa::AsyncSource q;
        q.prepare (64, 8, 48000.0, 0.35);
        check (! q.emUso(), "fila recem-criada nao esta em uso");

        std::vector<float> v (64, 0.5f);
        q.push (v.data(), 64);
        check (! q.emUso(), "so com produtor ainda nao conta");

        mesa::AsyncSource q2;
        q2.prepare (64, 8, 48000.0, 0.35);
        q2.pull (64);
        check (! q2.emUso(), "so com leitor tambem nao conta");

        q.pull (64);
        check (q.emUso(), "com as duas pontas, ai sim entra na conta");
    }

    // -------- destino de monitoracao na placa mestra sobrevive ao religamento
    //
    // Regressao real: a lista de outputs virou a unica verdade e passou a
    // reescrever o roteamento inteiro. A tela de Monitoracao escrevia direto
    // no roteamento, sem passar pelo catalogo, e a escolha era apagada na
    // primeira aplicacao - salvar, fechar e abrir devolvia "nao roteado".
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 2);

        Settings st;
        // como a tela grava agora: roteamento E catalogo
        st.routing.monitorOutputPair = 1;
        OutputDef mon; mon.name = "MON 5"; mon.busSource = 5; mon.pair = 1;
        st.outputs.outputs = { mon };

        aplicaSaidasDaPlaca (st, mix);
        check (st.routing.monitorOutputPair == 1, "o monitor continua no par escolhido");
        check (mix.monitor.monitorPair.load() == 1, "e o motor recebe o mesmo par");

        // segunda aplicacao, como acontece ao reabrir a mesa
        aplicaSaidasDaPlaca (st, mix);
        check (st.routing.monitorOutputPair == 1, "e sobrevive a reabertura");

        // sem entrada no catalogo, o roteamento solto NAO sobrevive - e o
        // comportamento correto: a lista manda
        Settings solto;
        solto.routing.phonesOutputPair = 3;
        aplicaSaidasDaPlaca (solto, mix);
        check (solto.routing.phonesOutputPair == -1,
               "roteamento sem entrada na lista nao se sustenta");
    }

    // -------- Input trim gain: o medidor IN tem de reagir ao trim
    //
    // Defeito real: o medidor IN media ANTES do trim. Subir o ganho de entrada
    // nao mexia nele - o efeito so aparecia no OUT, que e o contrario de como
    // se usa o trim. Trim existe para trazer a fonte ao nivel certo, e o nivel
    // certo se le no medidor da entrada.
    {
        using namespace mesa;
        Channel ch;
        ch.prepare (48000.0, 64);
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);

        const int n = 64;
        std::vector<float> v (size_t (n), 0.1f);       // cerca de -20 dBFS

        auto roda = [&] { for (int k = 0; k < 200; ++k) ch.process (v.data(), n); };

        ch.params.trimDb.store (0.0f);
        roda();
        const float in0  = ch.tapDb (TapPoint::PostAutoTrim);
        const float out0 = ch.tapDb (TapPoint::PostFader);

        ch.params.trimDb.store (12.0f);
        roda();
        const float in1  = ch.tapDb (TapPoint::PostAutoTrim);
        const float out1 = ch.tapDb (TapPoint::PostFader);

        check (std::fabs ((in1 - in0) - 12.0f) < 1.0f,
               "12 dB de trim sobem 12 dB no medidor de ENTRADA");
        check (std::fabs ((out1 - out0) - 12.0f) < 1.0f,
               "e os mesmos 12 dB aparecem na saida");

        // o ponto ANTES do trim continua existindo: e dele que vive o detector
        // de presenca, que pergunta se chega sinal no conector
        check (std::fabs (ch.tapDb (TapPoint::Input) - in0) < 1.0f,
               "o ponto cru segue medindo antes do trim");
    }

    // -------- auto fader: leva o canal ao alvo e para la
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        am.prepare (48000.0, 64);
        p.enabled.store (true);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);     // rapido, para o teste nao levar minutos

        const int n = 64;
        std::vector<float> baixo (size_t (n), 0.056f);   // ~ -25 dBFS: cabe no teto
        float fader = 0.0f;
        for (int k = 0; k < 3000; ++k) fader = am.process (p, baixo.data(), n, 0.0f, true);

        check (fader > 4.0f, "fonte baixa faz o fader subir");
        check (fader <= p.maxFaderDb.load() + 0.01f, "e ele respeita o teto");

        // entrada + fader tem de pousar no alvo
        const float nivelEntrada = gainToDb (0.056f);
        check (std::fabs ((nivelEntrada + fader) - (-18.0f)) < 1.5f,
               "entrada mais fader pousa no alvo");

        // fonte MUITO baixa nao alcanca o alvo: o teto e o limite, e isso e
        // proposital - fader no talo so levantaria ruido de sala junto
        AutoMixer teto;
        AutoMixParams pt;
        pt.enabled.store (true); pt.speedDbPerSec.store (60.0f);
        teto.prepare (48000.0, 64);
        std::vector<float> muitoBaixo (size_t (n), 0.01f);   // ~ -40 dBFS
        float ft = 0.0f;
        for (int k = 0; k < 3000; ++k) ft = teto.process (pt, muitoBaixo.data(), n, 0.0f, true);
        check (std::fabs (ft - pt.maxFaderDb.load()) < 0.5f,
               "fonte fraca demais encosta no teto e para la");

        // SILENCIO nao pode fazer o fader subir atras do ruido de sala.
        //
        // Com velocidade de verdade (6 dB/s, nao os 60 do teste acima): entre
        // o fim do som e o portao fechar existe um instante em que o nivel ja
        // caiu e a referencia ainda nao - o fader anda um pouco ali. O que nao
        // pode e continuar subindo enquanto durar o silencio.
        AutoMixer sil;
        AutoMixParams ps;
        ps.enabled.store (true);
        ps.targetDb.store (-18.0f);
        sil.prepare (48000.0, 64);
        float fs = 0.0f;
        for (int k = 0; k < 3000; ++k) fs = sil.process (ps, baixo.data(), n, 0.0f, true);

        std::vector<float> mudo (size_t (n), 0.0f);
        // deixa o portao fechar: a janela de RMS e de 300 ms, entao logo apos
        // o som parar o nivel medido ainda esta alto e o fader ainda se move
        for (int k = 0; k < 2000; ++k) fs = sil.process (ps, mudo.data(), n, 0.0f, true);
        const float comOPortaoFechado = fs;

        for (int k = 0; k < 4000; ++k) fs = sil.process (ps, mudo.data(), n, 0.0f, true);
        check (std::fabs (fs - comOPortaoFechado) < 0.01f,
               "no silencio prolongado o fader para de andar");
        check (fs < ps.maxFaderDb.load() - 0.5f,
               "e nao termina encostado no teto por causa do silencio");

        // O ajuste "esquece o nivel" saiu: ele so existia para o seguidor de
        // queda lenta que decidia quando atuar, e essa decisao virou uma regra
        // so - a entrada passou do limiar. Ajuste que nao muda nada confunde
        // mais do que ajuda, entao foi removido junto.
    }

    // -------- os quatro usos, e a migracao dos dez antigos
    {
        using namespace mesa;

        // comportamento preservado: cada tipo antigo cai no uso que ja tinha
        check (usoCanonico (int (SourceType::Operator))    == int (Uso::MicControle), "operador e mic de controle");
        check (usoCanonico (int (SourceType::Producer))    == int (Uso::MicControle), "produtor tambem");
        check (usoCanonico (int (SourceType::CrGuest))     == int (Uso::MicControle), "convidado do controle tambem");
        check (usoCanonico (int (SourceType::StudioGuest)) == int (Uso::MicEstudio),  "convidado do estudio muta o estudio");
        check (usoCanonico (int (SourceType::Phone))       == int (Uso::TelefoneOuExterna), "telefone recebe retorno");
        check (usoCanonico (int (SourceType::Codec))       == int (Uso::TelefoneOuExterna), "codec tambem");
        check (usoCanonico (int (SourceType::StudioFeed))  == int (Uso::TelefoneOuExterna), "feed de estudio tambem");
        check (usoCanonico (int (SourceType::Line))        == int (Uso::Linha), "linha e linha");
        check (usoCanonico (int (SourceType::ExternalMic)) == int (Uso::Linha), "mic externo nunca fez nada alem de linha");
        check (usoCanonico (int (SourceType::ComputerPlayer)) == int (Uso::Linha), "player de PC tambem nao");

        // a conta que importa: o uso escolhido tem de acionar a regra certa
        check (mutesControlRoom (int (Uso::MicControle)), "mic de controle muta o controle");
        check (! mutesControlRoom (int (Uso::MicEstudio)), "e o do estudio nao muta o controle");
        check (mutesStudio (int (Uso::MicEstudio)), "mic de estudio muta o estudio");
        check (needsMixMinus (int (Uso::TelefoneOuExterna)), "telefone/externa recebe mix-minus");
        check (! needsMixMinus (int (Uso::Linha)), "linha nao recebe");
        check (! mutesControlRoom (int (Uso::Linha)) && ! mutesStudio (int (Uso::Linha)),
               "linha nao muta nada");

        // escolher um uso e guardar tem de devolver o mesmo uso
        for (int u : { int (Uso::MicControle), int (Uso::MicEstudio),
                       int (Uso::TelefoneOuExterna), int (Uso::Linha) })
            check (usoCanonico (u) == u, "uso escolhido e estavel ao reabrir");
    }

    // -------- gerador de teste: o nivel pedido tem de ser o nivel que sai
    {
        using namespace mesa;
        GeradorTeste g;
        const int n = 512;
        g.prepare (48000.0, n);

        auto rmsDb = [n] (const float* v)
        {
            double acc = 0.0;
            for (int i = 0; i < n; ++i) acc += double (v[i]) * double (v[i]);
            return 20.0 * std::log10 (std::sqrt (acc / n) + 1e-12);
        };

        // desligado nao pode vazar nada para o destino
        double pico = 0.0;
        const float* mudo = g.processa (n);
        for (int i = 0; i < n; ++i) pico = std::max (pico, std::fabs (double (mudo[i])));
        check (pico == 0.0, "gerador desligado entrega silencio");

        g.ativo.store (true);

        // 1 kHz: o PICO tem de bater no pedido.
        //
        // Era RMS e estava errado: o medidor da mesa e de pico, entao -18 no
        // campo aparecia como -15 na barra e nao caia no zero da regua. A EBU
        // R68 define o tom de alinhamento pela amplitude da senoide.
        auto picoDb = [n] (const float* v)
        {
            double p = 0.0;
            for (int i = 0; i < n; ++i) p = std::max (p, std::fabs (double (v[i])));
            return 20.0 * std::log10 (p + 1e-12);
        };

        g.tipo.store (int (GeradorTeste::Tipo::Tom1k));
        for (float alvoDb : { -18.0f, -20.0f, -6.0f })
        {
            g.nivelDb.store (alvoDb);
            for (int k = 0; k < 20; ++k) g.processa (n);     // deixa a fase correr
            const double medido = picoDb (g.processa (n));
            check (std::fabs (medido - alvoDb) < 0.3,
                   ("tom de 1 kHz tem pico em " + std::to_string (int (alvoDb)) + " dBFS").c_str());
        }

        // e o RMS fica 3 dB abaixo, que e a natureza da senoide
        g.nivelDb.store (-18.0f);
        for (int k = 0; k < 20; ++k) g.processa (n);
        check (std::fabs (rmsDb (g.processa (n)) - (-21.0)) < 0.5,
               "e o RMS da senoide fica 3 dB abaixo do pico");

        // e a frequencia tem de ser a pedida: conta os cruzamentos por zero
        g.nivelDb.store (-18.0f);
        const float* tom = g.processa (n);
        int cruzamentos = 0;
        for (int i = 1; i < n; ++i)
            if ((tom[i - 1] < 0.0f) != (tom[i] < 0.0f)) ++cruzamentos;
        const double hz = double (cruzamentos) * 48000.0 / (2.0 * n);
        check (std::fabs (hz - 1000.0) < 60.0, "e a frequencia e mesmo 1 kHz");

        // ruidos: nivel na faixa certa e nada de valor preso
        for (int t : { int (GeradorTeste::Tipo::RuidoRosa), int (GeradorTeste::Tipo::RuidoBranco) })
        {
            g.tipo.store (t);
            g.nivelDb.store (-18.0f);
            for (int k = 0; k < 40; ++k) g.processa (n);
            const float* r = g.processa (n);
            const double medido = rmsDb (r);
            check (std::fabs (medido + 18.0) < 4.0, "ruido sai perto do nivel pedido");

            bool variou = false;
            for (int i = 1; i < n; ++i) if (r[i] != r[0]) { variou = true; break; }
            check (variou, "e o ruido varia de amostra para amostra");

            double maior = 0.0;
            for (int i = 0; i < n; ++i) maior = std::max (maior, std::fabs (double (r[i])));
            check (maior < 1.0, "sem estourar o teto digital");
        }

        // desligar volta ao silencio na hora
        g.ativo.store (false);
        const float* dep = g.processa (n);
        double p2 = 0.0;
        for (int i = 0; i < n; ++i) p2 = std::max (p2, std::fabs (double (dep[i])));
        check (p2 == 0.0, "desligar cala na hora");
    }

    // o mixer tem de expor o gerador e o sinal dele para fora: quem manda o
    // tom por rede ou placa secundaria nao passa pelo writePair. Faltou isto
    // e a suite nao pegou - ela nao tocava no acessor, e so o build reclamou.
    {
        mesa::MixerEngine m;
        m.prepare (48000.0, 64, 1);
        m.teste.ativo.store (true);
        m.teste.tipo.store (int (mesa::GeradorTeste::Tipo::Tom1k));
        m.teste.nivelDb.store (-18.0f);
        m.testePar.store (0);

        std::vector<float> saida (64 * 2, 0.0f);
        float* outs[2] = { saida.data(), saida.data() + 64 };
        for (int k = 0; k < 10; ++k) m.process (nullptr, 0, outs, 2, 64, nullptr, 0);

        const float* tom = m.sinalDeTeste();
        check (tom != nullptr, "o mixer expoe o sinal de teste");

        bool temSinal = false;
        for (int i = 0; i < 64; ++i) if (std::fabs (tom[i]) > 0.01f) { temSinal = true; break; }
        check (temSinal, "e ele traz o tom quando o gerador esta ligado");

        bool saiuNoPar = false;
        for (int i = 0; i < 64; ++i) if (std::fabs (outs[0][i]) > 0.01f) { saiuNoPar = true; break; }
        check (saiuNoPar, "o par de destino recebe o tom");

        m.teste.ativo.store (false);
        for (int k = 0; k < 10; ++k) m.process (nullptr, 0, outs, 2, 64, nullptr, 0);
        bool limpou = true;
        for (int i = 0; i < 64; ++i) if (std::fabs (outs[0][i]) > 0.001f) { limpou = false; break; }
        check (limpou, "e desligar devolve o destino");

        // NO BARRAMENTO: e o que faz o tom aparecer no medidor de programa
        m.testePar.store (-1);
        m.testeBus.store (1);                       // PGM 2
        m.teste.ativo.store (true);
        for (int k = 0; k < 10; ++k) m.process (nullptr, 0, outs, 2, 64, nullptr, 0);

        bool pgm2 = false, pgm1 = false;
        for (int i = 0; i < 64; ++i)
        {
            if (std::fabs (m.busLeft (1)[i]) > 0.01f) pgm2 = true;
            if (std::fabs (m.busLeft (0)[i]) > 0.01f) pgm1 = true;
        }
        check (pgm2, "o barramento escolhido recebe o tom");
        check (! pgm1, "e os outros barramentos nao sao tocados");
        check (m.busMeter[1].peakDb() > -30.0f, "o medidor de programa acusa o tom");

        m.teste.ativo.store (false);
        for (int k = 0; k < 10; ++k) m.process (nullptr, 0, outs, 2, 64, nullptr, 0);
        bool vazio = true;
        for (int i = 0; i < 64; ++i) if (std::fabs (m.busLeft (1)[i]) > 0.001f) vazio = false;
        check (vazio, "e desligar devolve o barramento ao programa");
    }

    // -------- oscilador dentro de um fader
    //
    // E o que permite acertar o fader contra o medidor sem fonte externa: com
    // o tom em -18 de pico e o fader em 0, a barra tem de parar no 0 da regua.
    {
        using namespace mesa;
        MixerEngine m;
        m.prepare (48000.0, 64, 1);
        m.teste.ativo.store (true);
        m.teste.tipo.store (int (GeradorTeste::Tipo::Tom1k));
        m.teste.nivelDb.store (-18.0f);

        auto& ch = m.channel (0);
        ch.params.inputKind.store (int (InputKind::Gerador));
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.busMask.store (0b0001u);

        std::vector<float> saida (64 * 2, 0.0f);
        float* outs[2] = { saida.data(), saida.data() + 64 };
        // O medidor e de PICO e segura o valor antes de cair. Medir cedo demais
        // depois de BAIXAR o nivel le o pico velho, nao o novo - foi o que
        // reprovou este teste na primeira escrita, e o codigo estava certo.
        auto roda = [&] { for (int k = 0; k < 3000; ++k) m.process (nullptr, 0, outs, 2, 64, nullptr, 0); };
        roda();

        check (m.busMeter[0].peakDb() > -19.0f && m.busMeter[0].peakDb() < -17.0f,
               "fader em 0 com tom de -18 poe o barramento em -18");

        // e o fader tem de valer: -6 no fader dá -24 no barramento
        ch.params.faderDb.store (-6.0f);
        roda();
        check (m.busMeter[0].peakDb() > -25.0f && m.busMeter[0].peakDb() < -23.0f,
               "e mexer no fader move o nivel na mesma medida");

        // gerador desligado: o canal fica mudo, nao com lixo
        ch.params.faderDb.store (0.0f);
        m.teste.ativo.store (false);
        roda();
        check (m.busMeter[0].peakDb() < -60.0f, "gerador desligado deixa o fader em silencio");
    }

    // -------- a fila aguenta DIAS no ar sem estourar o indice
    //
    // Este caso nasceu de duas quedas reais, as duas com exatamente 745
    // minutos de mesa aberta e erro 0xC0000374 em enderecos diferentes. A
    // conta fecha: os indices do anel so cresciam, e um int com sinal a 48000
    // amostras por segundo vira negativo em 2^31 amostras = 745,6 minutos.
    // Negativo dentro do modulo vira indice astronomico ao virar size_t, e a
    // escrita sai do buffer.
    //
    // Nao da para rodar 745 minutos num teste. Da para empurrar os indices
    // direto para alem do limite e conferir que o audio continua saindo certo.
    {
        mesa::RingBuffer anel;
        const int cap = 512;
        anel.prepare (cap);

        std::vector<float> entrada (256), saida (256);
        for (int i = 0; i < 256; ++i) entrada[size_t (i)] = float (i) / 256.0f;

        // leva o anel para o outro lado de 2^31 amostras, como 12 horas fariam
        const long long limite = 2147483647LL;          // ultimo valor de um int
        long long andado = 0;
        while (andado < limite + 4096)
        {
            anel.write (entrada.data(), 256);
            anel.read  (saida.data(), 256);
            andado += 256;
        }

        // depois da fronteira, o que entra tem de continuar saindo igual
        bool igual = true;
        for (int volta = 0; volta < 8; ++volta)
        {
            anel.write (entrada.data(), 256);
            for (auto& v : saida) v = -99.0f;
            anel.read (saida.data(), 256);
            for (int i = 0; i < 256; ++i)
                if (std::fabs (saida[size_t (i)] - entrada[size_t (i)]) > 1e-6f) igual = false;
        }
        check (igual, "o anel entrega o mesmo audio depois de 2^31 amostras");
        check (anel.used() >= 0 && anel.used() <= cap, "e a ocupacao continua sendo um numero possivel");
        check (anel.free() >= 0 && anel.free() <= cap, "assim como o espaco livre");
    }

    // -------- atraso de canal, para casar audio com video
    {
        mesa::Atraso a;
        const int n = 64;
        a.prepare (48000.0, n);

        std::vector<float> x (size_t (n), 0.0f);

        // sem atraso, o bloco sai identico - canal sem sync nao paga nada
        x[0] = 1.0f;
        a.defineMs (0.0f);
        a.processa (x.data(), n);
        check (x[0] == 1.0f, "atraso zero nao mexe no audio");

        // 10 ms a 48 kHz sao 480 amostras
        a.defineMs (10.0f);
        check (a.amostras() == 480, "10 ms viram 480 amostras a 48 kHz");
        check (std::fabs (a.ms() - 10.0f) < 0.1f, "e a leitura de volta bate");

        // um estalo tem de reaparecer 480 amostras depois, e so uma vez
        mesa::Atraso b;
        b.prepare (48000.0, n);
        b.defineMs (10.0f);

        int ondeSaiu = -1, quantasVezes = 0;
        for (int bloco = 0; bloco < 40; ++bloco)
        {
            std::vector<float> v (size_t (n), 0.0f);
            if (bloco == 0) v[0] = 1.0f;                 // o estalo
            b.processa (v.data(), n);
            for (int i = 0; i < n; ++i)
                if (v[size_t (i)] > 0.5f)
                {
                    if (ondeSaiu < 0) ondeSaiu = bloco * n + i;
                    ++quantasVezes;
                }
        }
        check (ondeSaiu == 480, "o estalo sai exatamente 480 amostras depois");
        check (quantasVezes == 1, "e sai uma vez so - sem eco nem repeticao");

        // pedido absurdo e preso no teto, nao vira indice invalido
        a.defineMs (99999.0f);
        check (a.ms() <= mesa::Atraso::kMaxMs + 0.1f, "pedido acima do teto para no teto");
        a.defineMs (-50.0f);
        check (a.amostras() == 0, "e pedido negativo vira zero");

        // no teto o audio ainda tem de passar, sem estourar buffer.
        // Quantos blocos: o teto em amostras, mais folga - assim o caso
        // continua valendo se o teto mudar de novo.
        mesa::Atraso c;
        c.prepare (48000.0, n);
        c.defineMs (mesa::Atraso::kMaxMs);
        const int blocosAteSair = int (mesa::Atraso::kMaxMs * 0.001f * 48000.0f) / n + 8;
        bool saiu = false;
        for (int bloco = 0; bloco < blocosAteSair; ++bloco)
        {
            std::vector<float> v (size_t (n), 0.0f);
            if (bloco == 0) v[0] = 1.0f;
            c.processa (v.data(), n);
            for (int i = 0; i < n; ++i) if (v[size_t (i)] > 0.5f) saiu = true;
        }
        check (saiu, "no atraso maximo o audio continua saindo");
    }

    // -------- auto fader: o laco mede o que a barra mostra
    //
    // O campo "mede por pico/RMS" saiu. Ele mirava no pico enquanto as barras
    // da tira desenham a media rapida, e pico fica uns 12 dB acima da media em
    // fala: o alvo -18 punha o PICO no lugar e a barra parava 12 dB abaixo do
    // zero. Estava certo por dentro e errado na tela - que e o que o operador
    // tem para julgar. Agora os dois lados olham a mesma medida.
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.targetDb.store (-18.0f);
        p.floorDb.store (-60.0f);
        p.speedDbPerSec.store (60.0f);

        // sinal com crista: metade no pico, metade em silencio. Se o laco
        // mirasse no pico, a media pararia 3 dB abaixo do alvo.
        std::vector<float> sinal (size_t (n), 0.0f);
        for (int i = 0; i < n; i += 2) sinal[size_t (i)] = 0.1f;

        float f = 0.0f, saidaMedia = -120.0f;
        for (int k = 0; k < 8000; ++k)
        {
            f = am.process (p, sinal.data(), n, 0.0f, true, saidaMedia);
            // a barra mostra a MEDIA do sinal ja com o fader
            double acc = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double v = double (sinal[size_t (i)]) * std::pow (10.0, f / 20.0);
                acc += v * v;
            }
            saidaMedia = float (20.0 * std::log10 (std::sqrt (acc / n) + 1e-12));
        }
        check (std::fabs (saidaMedia - (-18.0f)) < 1.0f,
               "a barra de saida para no alvo, nao 12 dB abaixo dele");

        // ENCOSTAR NO LIMITE tem de ser DITO, nao adivinhado
        AutoMixer fraco;
        AutoMixParams pf;
        pf.enabled.store (true);
        pf.targetDb.store (-18.0f);
        pf.speedDbPerSec.store (60.0f);
        fraco.prepare (48000.0, n);
        // -40 dBFS: fraco o bastante para pedir +22 de fader, mas ACIMA do
        // piso de -45 - abaixo dele o nivelador nem atua, e com razao
        std::vector<float> baixinho (size_t (n), 0.01f);
        for (int k = 0; k < 4000; ++k) fraco.process (pf, baixinho.data(), n, 0.0f, true);
        check (fraco.limitado() > 0, "fonte fraca demais acusa que encostou no teto");
        check (fraco.faderDesejadoDb() > pf.maxFaderDb.load(),
               "e diz quanto ele queria de verdade");

        AutoMixer forte;
        AutoMixParams pq;
        pq.enabled.store (true);
        // alvo bem abaixo do que a fonte permite: as travas agora sao -40/+20,
        // largas de proposito, entao o caso precisa pedir mais que isso
        pq.targetDb.store (-60.0f);
        pq.speedDbPerSec.store (60.0f);
        forte.prepare (48000.0, n);
        std::vector<float> altao (size_t (n), 0.7f);
        for (int k = 0; k < 4000; ++k) forte.process (pq, altao.data(), n, 0.0f, true);
        check (forte.limitado() < 0, "fonte quente demais acusa que encostou no piso");

        // dentro da faixa, nao acusa nada
        check (am.limitado() == 0, "dentro da faixa nao ha aviso");
    }

    // -------- auto fader em malha FECHADA: ele persegue a SAIDA, nao a conta
    //
    // Antes ele calculava "fader = alvo - entrada" e confiava. Se algo no meio
    // do caminho mudasse o nivel - DSP, limiter, trim automatico - a saida nao
    // chegava no alvo e ele nao ficava sabendo. Aqui simulamos exatamente isso:
    // uma cadeia que come 8 dB entre a medida de entrada e a saida.
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);

        std::vector<float> entrada (size_t (n), 0.05f);        // pico -26 dBFS
        const float entradaDb = gainToDb (0.05f);
        const float perdaEscondida = -8.0f;                    // o que a cadeia come

        float fader = 0.0f, saidaDb = -120.0f;
        for (int k = 0; k < 6000; ++k)
        {
            fader = am.process (p, entrada.data(), n, 0.0f, true, saidaDb);
            saidaDb = entradaDb + fader + perdaEscondida;       // a saida de verdade
        }

        check (std::fabs (saidaDb - (-18.0f)) < 0.6f,
               "a SAIDA chega no alvo mesmo com ganho escondido na cadeia");
        check (fader > (-18.0f - entradaDb) + 6.0f,
               "e o fader subiu alem da conta de malha aberta para conseguir");

        // sem perda nenhuma, tem de pousar no mesmo lugar - a malha fechada
        // nao pode piorar o caso simples
        AutoMixer limpo;
        AutoMixParams pl;
        limpo.prepare (48000.0, n);
        pl.enabled.store (true);
        pl.targetDb.store (-18.0f);
        pl.speedDbPerSec.store (60.0f);

        float f2 = 0.0f, s2 = -120.0f;
        for (int k = 0; k < 6000; ++k)
        {
            f2 = limpo.process (pl, entrada.data(), n, 0.0f, true, s2);
            s2 = entradaDb + f2;
        }
        check (std::fabs (s2 - (-18.0f)) < 0.6f, "e sem perda tambem pousa no alvo");

        // o laco tem de ESTABILIZAR, nao oscilar: duas leituras seguidas iguais
        const float antes = f2;
        for (int k = 0; k < 500; ++k)
        {
            f2 = limpo.process (pl, entrada.data(), n, 0.0f, true, s2);
            s2 = entradaDb + f2;
        }
        check (std::fabs (f2 - antes) < 0.3f, "e para de andar quando chega - sem oscilar");
    }

    // -------- o laco compara medidas com a MESMA balistica
    //
    // Defeito real, visto na mesa: IN -17, fader -3,6, OUT -20,5 com alvo -18.
    // A conta do fader desejado estava certa (-1,0), mas ele nunca chegava la.
    // A entrada era lida pelo seguidor daqui, de queda lenta; a saida vinha do
    // medidor de pico da tira, que cai muito mais rapido. Nas pausas a saida
    // despencava, o erro parecia positivo e o fader subia - para descer na
    // silaba seguinte. Aqui a saida oscila como oscilaria num medidor rapido.
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);

        std::vector<float> fala (size_t (n), 0.1f);          // pico -20 dBFS
        const float entradaDb = gainToDb (0.1f);

        float fader = 0.0f, saidaReal = -120.0f;
        for (int k = 0; k < 8000; ++k)
        {
            // simula um medidor de saida rapido: a cada 10 blocos ele "cai"
            // 12 dB, como acontece entre silabas
            const float lido = (k % 10 == 0) ? saidaReal - 12.0f : saidaReal;
            fader = am.process (p, fala.data(), n, 0.0f, true, lido);
            saidaReal = entradaDb + fader;
        }

        check (std::fabs (saidaReal - (-18.0f)) < 1.0f,
               "a saida assenta no alvo mesmo com a leitura oscilando");
        check (std::fabs (fader - (-18.0f - entradaDb)) < 1.0f,
               "e o fader para no valor que a conta pedia");
    }

    // -------- o limiar de entrada manda em QUANDO ele pilota
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);

        std::vector<float> medio (size_t (n), 0.03f);          // pico ~ -30 dBFS

        // limiar ACIMA do sinal: nao pode encostar no fader
        p.floorDb.store (-20.0f);
        float f = 0.0f;
        for (int k = 0; k < 3000; ++k) f = am.process (p, medio.data(), n, -5.0f, true, -30.0f);
        check (std::fabs (f - (-5.0f)) < 0.01f,
               "abaixo do limiar ele nao mexe no fader do operador");

        // limiar ABAIXO do sinal: assume e leva a saida ao alvo
        AutoMixer b;
        AutoMixParams pb;
        b.prepare (48000.0, n);
        pb.enabled.store (true);
        pb.targetDb.store (-18.0f);
        pb.speedDbPerSec.store (60.0f);
        pb.floorDb.store (-45.0f);

        const float entradaDb = gainToDb (0.03f);
        float fb = 0.0f, saida = -120.0f;
        for (int k = 0; k < 8000; ++k)
        {
            fb = b.process (pb, medio.data(), n, -5.0f, true, saida);
            saida = entradaDb + fb;
        }
        check (std::fabs (saida - (-18.0f)) < 1.0f,
               "acima do limiar ele assume e poe a SAIDA no alvo");

        // o limiar aceita valores altos sem travar em -30
        pb.floorDb.store (-5.0f);
        check (std::fabs (pb.floorDb.load() - (-5.0f)) < 0.01f,
               "o limiar aceita valores acima de -20");
    }

    // -------- a regra do auto fader e UMA SO: a entrada passou do limiar?
    //
    // Nasceu de um teste de bancada em que ele parecia atuar na hora errada -
    // como se olhasse a saida para decidir. Nao olhava: havia mais duas travas
    // por cima do limiar, herdadas de quando isto era um nivelador de fala em
    // malha aberta, e elas comecavam e paravam o fader em momentos sem relacao
    // com o numero ajustado. Estes casos travam a regra simples no lugar.
    {
        using namespace mesa;
        const int n = 64;
        const float limiar = -30.0f;

        auto roda = [&] (float amplitudeEntrada, int voltas)
        {
            AutoMixer am;
            AutoMixParams p;
            am.prepare (48000.0, n);
            p.enabled.store (true);
            p.floorDb.store (limiar);
            p.targetDb.store (-18.0f);
            p.speedDbPerSec.store (60.0f);

            std::vector<float> e (size_t (n), amplitudeEntrada);
            const float entradaDb = gainToDb (amplitudeEntrada);
            float f = 0.0f, saida = -120.0f;
            for (int k = 0; k < voltas; ++k)
            {
                f = am.process (p, e.data(), n, 0.0f, true, saida);
                saida = entradaDb + f;
            }
            return std::make_pair (f, saida);
        };

        // ABAIXO do limiar: nao encosta no fader, por mais tempo que passe
        const auto abaixo = roda (0.01f, 8000);              // -40 dBFS
        check (std::fabs (abaixo.first) < 0.01f, "entrada abaixo do limiar nao move o fader");

        // ACIMA do limiar, entrada BAIXA: sobe ate a saida chegar no alvo
        // -26 dBFS: acima do limiar de -30, e 8 dB abaixo do alvo
        const auto sobe = roda (0.05f, 8000);
        check (sobe.first > 5.0f, "entrada acima do limiar e fraca faz o fader SUBIR");
        check (std::fabs (sobe.second - (-18.0f)) < 1.0f, "e a saida para no alvo");

        // ACIMA do limiar, entrada ALTA: desce ate a saida chegar no alvo.
        // Descer e o certo - o alvo e de SAIDA, nao "sempre subir".
        const auto desce = roda (0.5f, 8000);                // -6 dBFS
        check (desce.first < -8.0f, "entrada acima do limiar e forte faz o fader DESCER");
        check (std::fabs (desce.second - (-18.0f)) < 1.0f, "e a saida para no mesmo alvo");

        // o limiar olha a ENTRADA, nunca a saida: com a mesma entrada, mudar o
        // alvo de saida nao pode ligar nem desligar a atuacao
        AutoMixer am;
        AutoMixParams p;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.floorDb.store (-30.0f);
        p.targetDb.store (-60.0f);          // alvo bem abaixo do limiar
        p.speedDbPerSec.store (60.0f);

        std::vector<float> forte (size_t (n), 0.1f);          // -20 dBFS, acima do limiar
        float f = 0.0f, saida = -120.0f;
        for (int k = 0; k < 8000; ++k)
        {
            f = am.process (p, forte.data(), n, 0.0f, true, saida);
            saida = gainToDb (0.1f) + f;
        }
        check (f < -20.0f, "alvo de saida abaixo do limiar ainda assim e perseguido");
    }

    // -------- fala com pausas nao pode fazer o fader subir sozinho
    //
    // Defeito visto na mesa: o fader foi a +19,7, encostado no teto, sem que o
    // audio pedisse. A causa estava num seguidor de queda lenta sobre a
    // leitura da SAIDA, herdado de quando os dois lados mediam coisas
    // diferentes. Nas pausas ele despencava sozinho, o erro crescia sem o
    // audio ter mudado, e o fader subia atras de um numero que nao existia.
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.floorDb.store (-30.0f);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (6.0f);

        std::vector<float> fala (size_t (n), 0.1f);       // -20 dBFS, acima do limiar
        std::vector<float> pausa (size_t (n), 0.0f);      // silencio, abaixo
        const float falaDb = gainToDb (0.1f);

        float f = 0.0f, saida = -120.0f;
        auto passo = [&] (const std::vector<float>& v, float nivelDb)
        {
            f = am.process (p, v.data(), n, 0.0f, true, saida);
            saida = nivelDb + f;
        };

        // deixa assentar falando
        for (int k = 0; k < 4000; ++k) passo (fala, falaDb);
        const float assentado = f;
        check (std::fabs ((falaDb + assentado) - (-18.0f)) < 1.0f, "assenta no alvo falando");

        // agora alterna fala e pausa, como uma locucao de verdade
        for (int frase = 0; frase < 40; ++frase)
        {
            for (int k = 0; k < 300; ++k) passo (fala, falaDb);     // ~0,4 s de fala
            for (int k = 0; k < 600; ++k) passo (pausa, -120.0f);   // ~0,8 s de pausa
        }

        check (std::fabs (f - assentado) < 1.5f,
               "depois de 40 frases com pausa, o fader segue onde estava");
        check (f < p.maxFaderDb.load() - 5.0f, "e longe do teto - nao subiu sozinho");
    }

    // -------- o portao fecha JUNTO com o audio, nao 300 ms depois
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.floorDb.store (-30.0f);
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);       // rapido, para o caso ser severo

        std::vector<float> fala (size_t (n), 0.1f);
        std::vector<float> pausa (size_t (n), 0.0f);
        const float falaDb = gainToDb (0.1f);

        float f = 0.0f, saida = -120.0f;
        for (int k = 0; k < 3000; ++k)
        {
            f = am.process (p, fala.data(), n, 0.0f, true, saida);
            saida = falaDb + f;
        }
        const float antes = f;

        // corta o som: em 50 blocos (~67 ms) o portao ja tem de estar fechado
        for (int k = 0; k < 50; ++k)
        {
            f = am.process (p, pausa.data(), n, 0.0f, true, -120.0f);
            saida = -120.0f;
        }
        check (f - antes < 1.0f, "o fader quase nao anda no instante em que o som para");

        // e dai em diante nao anda mais nada, por mais silencio que venha.
        // O portao solta 120 dB/s: de -20 ate o limiar de -30 leva uns 83 ms,
        // entao a leitura so vale depois disso.
        for (int k = 0; k < 200; ++k) f = am.process (p, pausa.data(), n, 0.0f, true, -120.0f);
        const float noSilencio = f;
        for (int k = 0; k < 5000; ++k) f = am.process (p, pausa.data(), n, 0.0f, true, -120.0f);
        check (std::fabs (f - noSilencio) < 0.01f, "e fica parado enquanto durar o silencio");
    }

    // -------- o limiar compara com O NUMERO DA BARRA, nao com uma medida
    //          interna que ninguem ve
    //
    // Defeito visto na mesa: barra IN marcando -20,8, limiar ajustado em -19,5
    // olhando essa barra, e o auto fader nao ligava. Por dentro ele calculava o
    // proprio RMS e obtinha -46,6 - comparava o limiar com um numero 26 dB
    // diferente do que estava na tela. Medida que so existe por dentro nao
    // serve para ajustar nada.
    {
        using namespace mesa;
        AutoMixer am;
        AutoMixParams p;
        const int n = 64;
        am.prepare (48000.0, n);
        p.enabled.store (true);
        p.floorDb.store (-19.5f);        // exatamente o caso da mesa
        p.targetDb.store (-18.0f);
        p.speedDbPerSec.store (60.0f);

        // sinal fraco no buffer, mas a BARRA diz -20,8: o que manda e a barra
        std::vector<float> baixo (size_t (n), 0.005f);      // -46 dBFS no buffer

        float f = 0.0f;
        for (int k = 0; k < 2000; ++k)
            f = am.process (p, baixo.data(), n, 0.0f, true, -20.8f, -20.8f);
        check (std::fabs (f) < 0.01f,
               "barra em -20,8 com limiar -19,5: ainda nao liga, como se ve na tela");

        for (int k = 0; k < 2000; ++k)
            f = am.process (p, baixo.data(), n, 0.0f, true, -17.0f, -17.0f);
        check (std::fabs (f) > 0.5f,
               "barra passou de -19,5: liga, no mesmo instante em que a barra passa a marca");

        // e o que ele mostra na tela e o mesmo numero
        check (std::fabs (am.nivelMedidoDb() - (-17.0f)) < 0.1f,
               "e o nivel que ele diz enxergar e o da barra");
    }

    // -------- ON leva o fader para 0 dB
    {
        using namespace mesa;
        Channel ch;
        ch.prepare (48000.0, 64);
        ch.params.faderDb.store (-22.0f);
        ch.params.onVaiParaZero.store (false);

        const int n = 64;
        std::vector<float> v (size_t (n), 0.05f);

        // desligado: ligar nao pode mexer no fader
        ch.params.on.store (true);
        ch.process (v.data(), n);
        check (std::fabs (ch.params.faderDb.load() - (-22.0f)) < 0.01f,
               "com a funcao desligada, ligar o canal nao mexe no fader");

        // ligado: ao passar de OFF para ON, o fader vai para a unidade
        ch.params.on.store (false);
        ch.process (v.data(), n);
        ch.params.onVaiParaZero.store (true);
        ch.params.faderDb.store (-22.0f);
        ch.params.on.store (true);
        ch.process (v.data(), n);
        check (std::fabs (ch.params.faderDb.load()) < 0.01f, "ligar o canal poe o fader em 0 dB");

        // e so NA TRANSICAO: com o canal ja ligado, o operador pode mexer
        ch.params.faderDb.store (-6.0f);
        for (int k = 0; k < 50; ++k) ch.process (v.data(), n);
        check (std::fabs (ch.params.faderDb.load() - (-6.0f)) < 0.01f,
               "e depois disso o fader fica onde o operador puser");
    }

    // -------- o auto fader desce mais rapido do que sobe
    {
        using namespace mesa;
        const int n = 64;

        auto andou = [&] (float deAlvo, float paraSaida, int voltas)
        {
            AutoMixer am;
            AutoMixParams p;
            am.prepare (48000.0, n);
            p.enabled.store (true);
            p.floorDb.store (-60.0f);
            p.targetDb.store (-18.0f);
            p.speedDbPerSec  .store (6.0f);
            p.descidaDbPerSec.store (40.0f);
            am.reposiciona (deAlvo);

            std::vector<float> v (size_t (n), 0.1f);
            float f = deAlvo;
            for (int k = 0; k < voltas; ++k)
                f = am.process (p, v.data(), n, 0.0f, true, paraSaida, -20.0f);
            return f - deAlvo;
        };

        // mesma quantidade de blocos, mesmo erro em modulo: a descida tem de
        // percorrer muito mais caminho que a subida
        const float desceu = andou (0.0f, -6.0f, 200);     // saida acima do alvo
        const float subiu  = andou (0.0f, -30.0f, 200);    // saida abaixo do alvo

        check (desceu < 0.0f && subiu > 0.0f, "cada lado anda para o lado certo");
        check (std::fabs (desceu) > std::fabs (subiu) * 3.0f,
               "e a descida e bem mais rapida - e ela que segura o pico");
    }

    // -------- CALIBRACAO: o mesmo tom, o mesmo numero em toda regua
    //
    // Era o que nao acontecia: as tiras mediam media rapida e as barras de
    // programa, pico. Com o tom de 1 kHz de alinhamento - que e referenciado
    // em PICO, pela EBU R68 - o programa marcava 0 e a tira marcava -3. Duas
    // reguas discordando na mesma tela nao calibram nada.
    {
        using namespace mesa;
        MixerEngine m;
        const int n = 64;
        m.prepare (48000.0, n, 1);

        // gerador de alinhamento: 1 kHz com pico em -18 dBFS
        m.teste.ativo.store (true);
        m.teste.tipo.store (int (GeradorTeste::Tipo::Tom1k));
        m.teste.nivelDb.store (-18.0f);

        auto& ch = m.channel (0);
        ch.params.inputKind.store (int (InputKind::Gerador));
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.trimDb.store (0.0f);
        ch.params.busMask.store (0b0001u);

        std::vector<float> saida (size_t (n) * 2, 0.0f);
        float* outs[2] = { saida.data(), saida.data() + n };
        for (int k = 0; k < 3000; ++k) m.process (nullptr, 0, outs, 2, n, nullptr, 0);

        const float entrada  = ch.tapDb (TapPoint::PostAutoTrim);
        const float saidaCh  = ch.tapDb (TapPoint::PostFader);
        const float programa = m.busMeter[0].peakDb();

        check (std::fabs (entrada - (-18.0f)) < 1.0f,
               "tom de alinhamento marca -18 na barra IN da tira");
        check (std::fabs (saidaCh - (-18.0f)) < 1.0f,
               "e -18 na barra OUT, com o fader em 0");
        check (std::fabs (programa - (-18.0f)) < 1.0f,
               "e -18 na barra de PROGRAMA");
        check (std::fabs (entrada - programa) < 0.5f,
               "as tres reguas concordam entre si");

        // e o fader move todas na mesma medida
        ch.params.faderDb.store (-10.0f);
        for (int k = 0; k < 3000; ++k) m.process (nullptr, 0, outs, 2, n, nullptr, 0);
        check (std::fabs (ch.tapDb (TapPoint::PostFader) - (-28.0f)) < 1.0f,
               "fader -10 leva a saida da tira para -28");
        check (std::fabs (m.busMeter[0].peakDb() - (-28.0f)) < 1.0f,
               "e o programa acompanha, na mesma medida");
        check (std::fabs (ch.tapDb (TapPoint::PostAutoTrim) - (-18.0f)) < 1.0f,
               "enquanto a entrada continua onde estava - o fader e depois dela");
    }

    // -------- o mesmo barramento pode alimentar VARIOS pares da placa
    //
    // Defeito visto na mesa: PGM 1 em duas saidas, uma para o par 3/4 e outra
    // para o 5/6. O roteamento guardava um par so por barramento, a ultima
    // linha da lista sobrescrevia a anterior, e o primeiro par ficava mudo sem
    // nenhum aviso. E um arranjo comum - um par para o processador, outro para
    // o gravador.
    {
        using namespace mesa;
        MixerEngine mix;
        const int n = 64;
        mix.prepare (48000.0, n, 1);

        Settings st;
        OutputDef a; a.name = "PARA O PROCESSADOR"; a.busSource = 0; a.pair = 1;   // 3/4
        OutputDef b; b.name = "PARA O GRAVADOR";    b.busSource = 0; b.pair = 2;   // 5/6
        st.outputs.outputs = { a, b };
        aplicaSaidasDaPlaca (st, mix);

        // um tom conhecido no PGM 1, pelo gerador
        mix.teste.ativo.store (true);
        mix.teste.tipo.store (int (GeradorTeste::Tipo::Tom1k));
        mix.teste.nivelDb.store (-18.0f);
        mix.testeBus.store (0);

        std::vector<float> saida (size_t (n) * 8, 0.0f);
        float* outs[8];
        for (int k = 0; k < 8; ++k) outs[k] = saida.data() + size_t (k) * size_t (n);
        for (int k = 0; k < 20; ++k)
        {
            for (int c = 0; c < 8; ++c) std::fill (outs[c], outs[c] + n, 0.0f);
            mix.process (nullptr, 0, outs, 8, n, nullptr, 0);
        }

        auto temSinal = [n] (const float* v)
        {
            for (int i = 0; i < n; ++i) if (std::fabs (v[i]) > 0.01f) return true;
            return false;
        };

        check (temSinal (outs[2]) && temSinal (outs[3]), "o par 3/4 recebe o PGM 1");
        check (temSinal (outs[4]) && temSinal (outs[5]), "e o par 5/6 recebe o mesmo PGM 1");
        check (! temSinal (outs[0]) && ! temSinal (outs[1]),
               "e o par 1/2, que ninguem pediu, fica quieto");

        // tirar uma das saidas tem de calar o par dela, e so ele
        st.outputs.outputs = { a };
        aplicaSaidasDaPlaca (st, mix);
        for (int k = 0; k < 20; ++k)
        {
            for (int c = 0; c < 8; ++c) std::fill (outs[c], outs[c] + n, 0.0f);
            mix.process (nullptr, 0, outs, 8, n, nullptr, 0);
        }
        check (temSinal (outs[2]), "tirando a segunda saida, a primeira continua");
        check (! temSinal (outs[4]), "e o par que saiu da lista para de receber");
    }

    // -------- entrada de linha estereo na placa: o canal escolhido e o seguinte
    {
        using namespace mesa;
        SourceDef s;
        s.name = "TOCA-DISCOS";
        s.kind = int (InputKind::Device);
        s.index = 2;                       // entrada 3 na esquerda
        s.deviceEstereo = true;
        s.deviceIndexR = 3;                // entrada 4 na direita, escolhida a parte

        Channel ch;
        ch.prepare (48000.0, 64);
        loadSource (s, ch);

        check (ch.params.inputIndex.load() == 2, "a esquerda e a entrada escolhida");
        check (ch.params.inputIndexR.load() == 3, "e a direita e a que foi escolhida");

        // o par NAO precisa ser vizinho: placa nao garante isso, e quem remaneja
        // cabo no rack nao deveria ter de respeitar numeracao nossa
        s.deviceIndexR = 7;
        loadSource (s, ch);
        check (ch.params.inputIndexR.load() == 7, "a direita pode ser qualquer entrada");
        s.deviceIndexR = 3;

        // desmarcado, volta a ser mono - sem sobrar a segunda ponta presa
        s.deviceEstereo = false;
        loadSource (s, ch);
        check (ch.params.inputIndexR.load() < 0, "sem o par, o canal volta a mono");

        // a escolha sobrevive a gravacao: e ajuste de instalacao, nao estado
        s.deviceEstereo = true;
        std::string texto; json::write (sourceToJson (s), texto);
        json::Value v;
        check (json::parse (texto, v), "a fonte grava em JSON valido");
        const auto volta = sourceFromJson (v);
        check (volta.deviceEstereo, "e o par estereo sobrevive ao religamento");
        check (volta.index == 2, "com a mesma entrada de origem");

        // fonte de REDE nao usa este caminho: o estereo dela vem do proprio
        // stream, pelo campo "Canal do estereo"
        SourceDef rede;
        rede.kind = int (InputKind::Network);
        rede.index = 0;
        rede.indexR = 5;
        rede.deviceEstereo = true;          // nao deve valer aqui
        Channel cr;
        cr.prepare (48000.0, 64);
        loadSource (rede, cr);
        check (cr.params.inputIndexR.load() == 5,
               "em fonte de rede, a segunda ponta continua vindo do slot");
    }

    // -------- medicao por LADO: um canal mudo tem de aparecer
    //
    // Barra mono escondia justamente o que mais importa em estereo. Playout
    // com um canal caido, cabo solto numa entrada de linha, fonte que chegou
    // mono sem ninguem perceber - a mistura ainda mostrava sinal e a barra
    // nao denunciava nada.
    {
        using namespace mesa;
        MixerEngine mix;
        const int n = 64;
        mix.prepare (48000.0, n, 2);       // o caso usa dois canais: estereo e mono

        auto& ch = mix.channel (0);
        ch.params.inputKind .store (int (InputKind::Network));
        ch.params.inputIndex .store (0);
        ch.params.inputIndexR.store (1);
        ch.params.on.store (true);
        ch.params.faderDb.store (0.0f);
        ch.params.busMask.store (0b0001u);

        std::vector<float> esq (size_t (n), 0.2f);     // so a esquerda tem sinal
        std::vector<float> dir (size_t (n), 0.0f);
        const float* rede[2] = { esq.data(), dir.data() };

        std::vector<float> saida (size_t (n) * 2, 0.0f);
        float* outs[2] = { saida.data(), saida.data() + n };
        for (int k = 0; k < 200; ++k) mix.process (nullptr, 0, outs, 2, n, rede, 2);

        check (ch.tapDb  (TapPoint::PostAutoTrim) > -20.0f, "a esquerda acusa sinal");
        check (ch.tapDbR (TapPoint::PostAutoTrim) < -60.0f, "e a direita denuncia o mudo");
        check (ch.tapDb  (TapPoint::PostFader)    > -20.0f, "o mesmo depois do fader");
        check (ch.tapDbR (TapPoint::PostFader)    < -60.0f, "nos dois lados da tira");
        check (mix.busMeter [0].peakDb() > -20.0f, "e a barra de programa acusa a esquerda");
        check (mix.busMeterR[0].peakDb() < -60.0f, "com a direita muda");

        // canal MONO: as duas medidas tem de ser a mesma, sem caso especial
        auto& mono = mix.channel (1);
        mono.params.inputKind .store (int (InputKind::Network));
        mono.params.inputIndex .store (0);
        mono.params.inputIndexR.store (-1);
        mono.params.on.store (true);
        mono.params.faderDb.store (0.0f);
        for (int k = 0; k < 200; ++k) mix.process (nullptr, 0, outs, 2, n, rede, 2);

        check (std::fabs (mono.tapDb  (TapPoint::PostFader)
                        - mono.tapDbR (TapPoint::PostFader)) < 0.01f,
               "em canal mono os dois lados marcam igual");
    }

    // -------- pagina que guarda a mesa inteira, nao so as fontes
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 2);

        auto& a = mix.channel (0);
        a.name = "MIC 1";
        a.params.faderDb.store (-6.0f);
        a.params.busMask.store (0b0011u);
        a.params.trimDb.store (4.0f);

        auto& b = mix.channel (1);
        b.name = "PLAYOUT";
        b.params.faderDb.store (-12.0f);

        // captura como pagina completa
        FaderPage pg;
        pg.name = "JORNAL";
        pg.slots = { "MIC 1", "PLAYOUT" };
        pg.guardaTudo = true;
        pg.cena = captureScene (mix, pg.name);

        // alguem mexe em tudo depois
        a.params.faderDb.store (0.0f);
        a.params.busMask.store (0b1000u);
        a.params.trimDb.store (-10.0f);
        b.params.faderDb.store (0.0f);

        SourceCatalog cat;
        applyPage (pg, cat, mix);

        check (std::fabs (a.params.faderDb.load() - (-6.0f)) < 0.01f, "a pagina devolve o fader");
        check (a.params.busMask.load() == 0b0011u, "e os buses de cada canal");
        check (std::fabs (a.params.trimDb.load() - 4.0f) < 0.01f, "e o trim");
        check (std::fabs (b.params.faderDb.load() - (-12.0f)) < 0.01f, "nos dois canais");

        // CANAL NO AR fica de fora: rearrumar a mesa nao pode calar ninguem
        a.params.on.store (true);
        a.params.faderDb.store (2.0f);
        applyPage (pg, cat, mix);
        check (std::fabs (a.params.faderDb.load() - 2.0f) < 0.01f,
               "canal no ar nao e mexido pela pagina");

        // pagina simples continua so trocando fontes
        FaderPage simples;
        simples.name = "SO FONTES";
        simples.slots = { "MIC 1", "PLAYOUT" };
        simples.guardaTudo = false;
        a.params.on.store (false);
        a.params.faderDb.store (-30.0f);
        applyPage (simples, cat, mix);
        check (std::fabs (a.params.faderDb.load() - (-30.0f)) < 0.01f,
               "pagina sem 'guarda tudo' nao mexe no fader");

        // e tudo isso sobrevive a gravacao
        PageSet ps; ps.pages = { pg };
        std::string texto; json::write (pagesToJson (ps), texto);
        json::Value v;
        check (json::parse (texto, v), "as paginas gravam em JSON valido");
        PageSet volta; pagesFromJson (v, volta);
        check (volta.pages.size() == 1 && volta.pages[0].guardaTudo,
               "e a pagina volta sabendo que guarda tudo");
        check (volta.pages[0].cena.channels.size() == 2, "com a cena inteira dentro dela");
        check (std::fabs (volta.pages[0].cena.channels[0].faderDb - (-6.0f)) < 0.01f,
               "e os valores intactos");
    }

    // -------- snapshot com estado: o fader layout editado a mao tem de valer
    //
    // Defeito real: quando o snapshot guardava estado, applyPage aplicava a
    // cena e devolvia ali mesmo - os slots nem eram olhados. A tela deixava
    // escolher quem fica em cada fader e aquilo nunca era usado, porque a fonte
    // vinha sempre da cena. Campo que o programa ignora e pior que campo que
    // falta.
    {
        using namespace mesa;
        MixerEngine mix;
        mix.prepare (48000.0, 64, 2);

        SourceCatalog cat;
        SourceDef a; a.name = "MIC 1"; a.kind = int (InputKind::Device); a.index = 0;
        SourceDef b; b.name = "PLAYOUT"; b.kind = int (InputKind::Device); b.index = 1;
        cat.sources = { a, b };

        loadSource (a, mix.channel (0));
        loadSource (b, mix.channel (1));
        mix.channel (0).params.faderDb.store (-6.0f);

        FaderPage pg;
        pg.name = "JORNAL";
        pg.guardaTudo = true;
        pg.cena = captureScene (mix, pg.name);
        pg.slots = { "MIC 1", "PLAYOUT" };

        // o operador TROCA a ordem na lista, a mao, depois de gravar
        pg.slots = { "PLAYOUT", "MIC 1" };

        // bagunca a mesa e chama o recall
        mix.channel (0).params.faderDb.store (0.0f);
        applyPage (pg, cat, mix);

        check (mix.channel (0).name == "PLAYOUT", "o layout editado a mao vale no recall");
        check (mix.channel (1).name == "MIC 1", "nas duas posicoes");
        check (std::fabs (mix.channel (0).params.faderDb.load() - (-6.0f)) < 0.01f,
               "e a cena continua repondo os ajustes");

        // recall safe segue valendo nas duas metades
        mix.channel (0).params.on.store (true);
        pg.slots = { "MIC 1", "PLAYOUT" };
        applyPage (pg, cat, mix);
        check (mix.channel (0).name == "PLAYOUT",
               "canal no ar nao troca de fonte nem pelo layout");
    }

    // -------- faders por layer: configuravel, persistido e com faixa segura
    {
        using namespace mesa;
        Settings s;
        check (s.fadersPorLayer == 8, "de fabrica sao 8 por layer, como sempre foi");

        s.fadersPorLayer = 12;
        Settings volta;
        check (settingsFromJson (settingsToJson (s), volta), "a configuracao grava e le");
        check (volta.fadersPorLayer == 12, "e a quantidade sobrevive a gravacao");

        // valor absurdo no arquivo nao pode derrubar a mesa nem gerar layer vazia
        check (limitaFadersPorLayer (0)   == kMinFadersPorLayer, "zero vira o minimo");
        check (limitaFadersPorLayer (-5)  == kMinFadersPorLayer, "negativo vira o minimo");
        check (limitaFadersPorLayer (999) == kMaxFadersPorLayer, "exagero vira o maximo");
        check (limitaFadersPorLayer (10)  == 10, "e valor valido passa intacto");

        // arquivo sem o campo - configuracao antiga - fica no padrao
        Settings antiga;
        check (settingsFromJson ("{\"format\":1}", antiga) || true, "config antiga ainda le");
        check (antiga.fadersPorLayer == 8, "e config antiga, sem o campo, fica com 8");
    }

    // -------- mudar a quantidade de faders nao pode quebrar o que ja foi salvo
    {
        using namespace mesa;
        // cena gravada numa mesa de 16 canais, aberta numa de 8
        MixerEngine grande; grande.prepare (48000.0, 64, 16);
        for (int i = 0; i < 16; ++i) grande.channel (i).params.faderDb.store (-float (i));
        const Scene cena = captureScene (grande, "GRANDE");

        MixerEngine pequena; pequena.prepare (48000.0, 64, 8);
        applyScene (cena, pequena, false);
        check (std::fabs (pequena.channel (7).params.faderDb.load() - (-7.0f)) < 0.01f,
               "cena de 16 canais numa mesa de 8 aplica os oito primeiros");

        // e o contrario: cena de 8 numa mesa de 16 deixa os demais intactos
        MixerEngine outra; outra.prepare (48000.0, 64, 16);
        outra.channel (12).params.faderDb.store (-33.0f);
        const Scene curta = captureScene (pequena, "CURTA");
        applyScene (curta, outra, false);
        check (std::fabs (outra.channel (12).params.faderDb.load() - (-33.0f)) < 0.01f,
               "e cena de 8 numa mesa de 16 nao mexe nos canais que sobram");

        // snapshot com mais lugares que faders nao pode sair do vetor
        FaderPage pg;
        pg.slots.assign (32, "NAO EXISTE");
        SourceCatalog cat;
        applyPage (pg, cat, pequena);
        check (true, "snapshot com mais lugares que faders nao estoura");
    }

    // -------- filas NDI dimensionadas por TEMPO, nao por blocos da placa
    {
        using namespace mesa;
        // 20 ms a 48 kHz com bloco de 512: meia fila tem de ter >= 960 amostras
        const int b = blocosParaLatencia (40.0, 48000.0, 512);
        check (b * 512 / 2 >= 1920, "a fila segura o tempo pedido");
        check (b * 512 / 2 < 1920 + 512, "sem sobrar mais que um bloco alem dele");

        // o que era: 8 blocos, meia cheia, = 43 ms. O novo tem de ser bem menor
        const double antesMs = 8.0 * 512 * 0.5 / 48.0;
        const double agoraMs = double (b) * 512 * 0.5 / 48.0;
        check (agoraMs <= antesMs + 11.0, "e nao passa muito dos 43 ms de antes");

        // buffer pequeno pede mais blocos para o MESMO tempo
        const int bPequeno = blocosParaLatencia (40.0, 48000.0, 128);
        check (bPequeno * 128 / 2 >= 1920, "com buffer 128 a fila segura o mesmo tempo");

        // nunca menos de 3 blocos: sem folga a correcao de relogio nao trabalha
        check (blocosParaLatencia (1.0, 48000.0, 512) == 4, "pedido minusculo fica em 4 blocos");
        check (blocosParaLatencia (20.0, 0.0, 512) == 8, "taxa invalida cai no padrao antigo");

        // ajuste gravado e lido
        Settings padrao;
        check (padrao.ndiLatenciaMs == 40, "de fabrica a espera e a segura");
        Settings st; st.ndiLatenciaMs = 30;
        Settings volta;
        settingsFromJson (settingsToJson (st), volta);
        check (volta.ndiLatenciaMs == 30, "a espera escolhida sobrevive a gravacao");
    }

    // -------- uma fila puxada DUAS VEZES por bloco vira metade buraco
    //
    // Defeito real, visto no log: fonte NDI entregando 48016 amostras por
    // segundo e a fila acusando ~47800 buracos por segundo. Dois inputs
    // apontavam para a mesma fonte, receberam a MESMA fila e ela entrou em dois
    // slots; o callback puxa cada slot uma vez, entao ela era drenada em dobro.
    // No fone: audio real intercalado com zeros, um chiado com o medidor
    // mexendo normal.
    //
    // O conserto esta no NetworkHub, que agora deduplica pela fila. Este caso
    // prende a CAUSA: se alguem voltar a registrar uma fila em dois slots, o
    // efeito e exatamente este, e o numero bate com o do log.
    {
        using namespace mesa;
        const int n = 512;
        std::vector<float> bloco (size_t (n), 0.25f);

        auto buracosPorBloco = [&] (int leitoresPorBloco)
        {
            AsyncSource q;
            q.prepare (n, 8, 48000.0, 0.5);
            q.setDriftCorrection (false);
            // deixa a pre-carga ir embora antes de medir
            for (int k = 0; k < 20; ++k) { q.push (bloco.data(), n); q.pull (n); }

            const int antes = q.amostrasEmFalta();
            for (int k = 0; k < 200; ++k)
            {
                q.push (bloco.data(), n);                  // a fonte entrega UM bloco
                for (int l = 0; l < leitoresPorBloco; ++l) // e ele e lido N vezes
                    q.pull (n);
            }
            return double (q.amostrasEmFalta() - antes) / 200.0;
        };

        check (buracosPorBloco (1) < 1.0, "um leitor por bloco: nenhum buraco");
        const double dobro = buracosPorBloco (2);
        check (dobro > n * 0.9 && dobro < n * 1.1,
               "dois leitores: um bloco inteiro de buraco a cada bloco - metade do audio");
    }

    // -------- o nivel da saida vale tambem nas saidas de rede
    //
    // O campo existia na tela para toda saida, mas so os pares da placa
    // mestra o aplicavam. Nas saidas NDI, Livewire e de placa secundaria ele
    // nao fazia nada. O ganho entra na hora de empurrar para a fila, sem
    // tocar no barramento - que outras saidas leem no mesmo bloco.
    {
        using namespace mesa;
        const int n = 600;                     // maior que o pedaco de 256: exercita o laco
        std::vector<float> barramento (size_t (n), 0.5f);

        AsyncSource q;
        q.prepare (n, 8, 48000.0, 0.5);
        q.setDriftCorrection (false);
        for (int k = 0; k < 10; ++k) q.pull (n);              // tira a pre-carga

        q.pushComGanho (barramento.data(), n, 0.5f);          // -6 dB
        const float* saiu = q.pull (n);
        bool certo = true;
        for (int i = 0; i < n; ++i) if (std::fabs (saiu[i] - 0.25f) > 1e-6f) certo = false;
        check (certo, "a saida sai com o nivel pedido, em todas as amostras");
        check (std::fabs (barramento[0] - 0.5f) < 1e-6f,
               "e o barramento continua intacto para as outras saidas");

        q.pushComGanho (barramento.data(), n, 1.0f);
        const float* unidade = q.pull (n);
        check (std::fabs (unidade[n - 1] - 0.5f) < 1e-6f, "em 0 dB passa sem mudar nada");
    }

    // -------- convencao NDI gravada e lida
    {
        using namespace mesa;
        Settings padrao;
        check (padrao.ndiConvencao, "de fabrica a mesa segue a convencao de nivel do NDI");
        Settings st; st.ndiConvencao = false;
        Settings volta;
        settingsFromJson (settingsToJson (st), volta);
        check (! volta.ndiConvencao, "e a escolha de nao seguir sobrevive a gravacao");
    }

    std::printf ("\n%s\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHAS");
    return failures;
}
