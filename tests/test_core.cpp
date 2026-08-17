// Testes do core sem JUCE: g++ -std=c++17 -O2 tests/test_core.cpp -o build/test_core
#include "../Source/Core/MixerEngine.h"
#include "../Source/Core/Scene.h"
#include "../Source/Core/Settings.h"
#include "../Source/Core/AsyncSource.h"
#include "../Source/Core/RateConverter.h"
#include "../Source/Core/AutomationEngine.h"
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
    // dois sinais identicos em fase => +6 dB, e o pan central tira 3 dB por lado
    check (near (mix.masterMeterL.peakDb(), -20.0f + 6.0f - 3.0f, 0.8f),
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


    std::printf ("\n%s\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHAS");
    return failures;
}
