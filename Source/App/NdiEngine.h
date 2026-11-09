#pragma once
#include <cstring>
#include "../Core/NomeDaThread.h"
#include <juce_core/juce_core.h>
#include "../Core/AsyncSource.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <string>
#include <vector>

#if MESA_HAS_NDI
 #include <Processing.NDI.Lib.h>
 #if JUCE_WINDOWS
  // NOMINMAX: sem isso o windows.h define max/min como MACRO e quebra
  // std::numeric_limits<T>::max() em qualquer header que venha depois.
  #ifndef NOMINMAX
   #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
   #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
 #else
  #include <dlfcn.h>
 #endif
#endif

/** Camada NDI.

    A biblioteca e carregada em TEMPO DE EXECUCAO: o projeto compila sem o
    runtime instalado e simplesmente reporta indisponivel. Sem o SDK nos
    includes, todo este arquivo vira casca vazia (MESA_HAS_NDI desligado).

    Recepcao usa framesync. Ele nao e conveniencia: o relogio do emissor NDI
    nao e o da nossa placa, e o framesync entrega exatamente o numero de
    amostras pedido, no nosso sample rate, absorvendo a diferenca. Sem ele a
    fila esvazia ou transborda num ritmo previsivel e ninguem descobre porque. */
class NdiEngine
{
public:
    struct SourceInfo { std::string name, url; };

    static NdiEngine& instance()
    {
        static NdiEngine e;
        return e;
    }

    bool available() const noexcept { return lib != nullptr; }

    /** CONVENCAO DE NIVEL DO NDI.

        O NDI trabalha em ponto flutuante e define que 1,0 e +4 dBu - o nivel
        de REFERENCIA, nao o teto. A escala cheia fica 20 dB acima, em 10,0.
        A mesa usa 1,0 como escala cheia, que e o normal em audio digital.

        Sem converter, tudo que saia por NDI chegava 20 dB abaixo num receptor
        que segue a convencao - o vMix mostrava a saida NDI bem mais baixa que
        a Livewire, com o mesmo barramento nas duas. E na entrada o contrario:
        emissor na convencao manda picos acima de 1,0, que a mesa lia como
        acima da escala cheia.

        O fator e configuravel porque nem todo emissor segue a convencao; o
        gerador de teste e o jeito de conferir. */
    std::atomic<float> fatorEntrada { 0.1f };    // NDI -> mesa: -20 dB
    std::atomic<float> fatorSaida   { 10.0f };   // mesa -> NDI: +20 dB

    void usaConvencaoNdi (bool padrao) noexcept
    {
        fatorEntrada.store (padrao ? 0.1f : 1.0f);
        fatorSaida  .store (padrao ? 10.0f : 1.0f);
    }
    juce::String status() const { return statusText; }

    /** Fontes vistas na rede. Copia barata — a lista e pequena. */
    std::vector<SourceInfo> sources()
    {
        std::lock_guard<std::mutex> g (listMutex);
        return found;
    }

#if MESA_HAS_NDI

    /** Carrega a DLL do NDI Runtime em tempo de execucao.

        O header declara NDIlib_v5_load como dllimport, o que obrigaria a linkar
        o .lib e a ter o runtime presente para o exe sequer abrir. Buscando o
        simbolo a mao, a mesa roda numa maquina sem NDI instalado e simplesmente
        reporta indisponivel — que e o comportamento certo para um console. */
    /** De onde a biblioteca foi carregada. Na tela, isso transforma "nao
        funciona" em "esta usando a do SDK" numa olhada. */
    static inline std::string ondeAchou;

    static const NDIlib_v5* loadRuntime()
    {
       #if JUCE_WINDOWS
        // ONDE A DLL REALMENTE ESTA.
        //
        // A busca era a variavel de ambiente do redist e o PATH. Numa maquina
        // de desenvolvimento, onde esta instalado o SDK e nao o Runtime, a DLL
        // existe - em Bin\x64 do SDK - e nao esta em nenhum dos dois lugares.
        // A mesa dizia "runtime nao encontrado" com o arquivo ali, a dois
        // diretorios de distancia, e a orientacao na tela mandava instalar
        // algo que ja estava na maquina de outra forma.
        //
        // A ordem tenta o runtime antes do SDK: onde os dois existem, o
        // runtime e o que a NewTek recomenda para producao.
        std::vector<std::string> candidatos;

        auto comVariavel = [&candidatos] (const char* var)
        {
            if (const char* dir = std::getenv (var))
                if (dir[0] != '\0')
                    candidatos.push_back (std::string (dir) + "\\" + NDILIB_LIBRARY_NAME);
        };
        comVariavel (NDILIB_REDIST_FOLDER);
        comVariavel ("NDI_RUNTIME_DIR_V6");
        comVariavel ("NDI_RUNTIME_DIR_V5");

        static const char* pastas[] = {
            "C:\\Program Files\\NDI\\NDI 6 Runtime\\v6",
            "C:\\Program Files\\NDI\\NDI 5 Runtime\\v5",
            "C:\\Program Files\\NDI\\NDI 6 SDK\\Bin\\x64",
            "C:\\Program Files\\NDI\\NDI 5 SDK\\Bin\\x64"
        };
        for (const char* d : pastas)
            candidatos.push_back (std::string (d) + "\\" + NDILIB_LIBRARY_NAME);

        HMODULE h = nullptr;
        for (const auto& c : candidatos)
            if ((h = LoadLibraryA (c.c_str())) != nullptr) { ondeAchou = c; break; }

        if (h == nullptr && (h = LoadLibraryA (NDILIB_LIBRARY_NAME)) != nullptr)
            ondeAchou = std::string (NDILIB_LIBRARY_NAME) + " (pelo PATH)";

        if (h == nullptr) return nullptr;

        using LoadFn = const NDIlib_v5* (*) (void);
        auto fn = reinterpret_cast<LoadFn> (reinterpret_cast<void*> (
                      GetProcAddress (h, "NDIlib_v5_load")));
       #else
        void* h = dlopen (NDILIB_LIBRARY_NAME, RTLD_LOCAL | RTLD_LAZY);
        if (h == nullptr) return nullptr;
        using LoadFn = const NDIlib_v5* (*) (void);
        auto fn = reinterpret_cast<LoadFn> (dlsym (h, "NDIlib_v5_load"));
       #endif
        return fn != nullptr ? fn() : nullptr;
    }

    NdiEngine()
    {
        lib = loadRuntime();
        if (lib == nullptr)
        {
            statusText = "biblioteca do NDI nao encontrada (instale o NDI Runtime)";
            return;
        }
        if (! lib->initialize())
        {
            lib = nullptr;
            statusText = "CPU sem suporte ao NDI";
            return;
        }
        statusText = "NDI pronto (parado)  |  de: " + juce::String (ondeAchou);
    }

    /** Descoberta sob demanda.

        Antes ela subia no arranque e varria a rede para sempre. Uma mesa sem
        nenhuma fonte NDI ficava com uma thread de terceiro criando socket a
        noite toda sem ninguem precisar — e foi exatamente ali que a mesa caiu,
        duas vezes, dentro da biblioteca do NDI.

        Agora so roda quando alguem precisa: com a janela de configuracoes
        aberta, ou quando existe fonte NDI no catalogo. */
    void startDiscovery()
    {
        if (lib == nullptr || findThread.joinable()) return;

        NDIlib_find_create_t fd;
        fd.show_local_sources = true;
        finder = lib->find_create_v2 (&fd);
        if (finder == nullptr) { statusText = "NDI: falha ao criar o localizador"; return; }

        quit.store (false);
        findThread = std::thread ([this] { findLoop(); });
        statusText = "NDI pronto (procurando)";
    }

    void stopDiscovery()
    {
        if (! findThread.joinable()) return;
        quit.store (true);
        findThread.join();
        if (finder != nullptr && lib != nullptr) { lib->find_destroy (finder); finder = nullptr; }
        quit.store (false);
        statusText = "NDI pronto (descoberta parada)";
        std::lock_guard<std::mutex> g (listMutex);
        found.clear();
    }

    bool discovering() const noexcept { return findThread.joinable(); }

    /** Encerramento EXPLICITO, chamado pela mesa antes de o JUCE desmontar.

        O destrutor deste objeto roda na destruicao de estaticos, depois que o
        Windows ja pode ter descarregado a DLL do NDI — chamar destroy() ali
        derruba o processo na saida. Foi o que apareceu no mesa-crash.log:
        queda no mesmo segundo do encerramento normal, com a pilha dentro de
        uma DLL. Encerrando antes, a ordem fica sob nosso controle. */
    void shutdown()
    {
        stopDiscovery();
        if (lib != nullptr && ! jaEncerrado)
        {
            lib->destroy();
            jaEncerrado = true;
            lib = nullptr;
        }
    }

    ~NdiEngine()
    {
        // De proposito NAO chama destroy() aqui: se a mesa nao tiver encerrado
        // explicitamente, e mais seguro vazar a biblioteca do que arriscar
        // tocar numa DLL ja descarregada durante a saida do processo.
        stopDiscovery();
    }

    const NDIlib_v5* api() const noexcept { return lib; }

private:
    void findLoop()
    {
        mesa::batizaThread ("ndi-findLoop");
        while (! quit.load())
        {
            // espera ate 1 s por mudanca na rede; nao queima CPU
            lib->find_wait_for_sources (finder, 1000);
            uint32_t n = 0;
            const NDIlib_source_t* s = lib->find_get_current_sources (finder, &n);

            std::vector<SourceInfo> next;
            next.reserve (n);
            for (uint32_t i = 0; i < n; ++i)
                next.push_back ({ s[i].p_ndi_name != nullptr ? s[i].p_ndi_name : "",
                                  s[i].p_url_address != nullptr ? s[i].p_url_address : "" });

            std::lock_guard<std::mutex> g (listMutex);
            found.swap (next);
        }
    }

    const NDIlib_v5* lib = nullptr;
    bool jaEncerrado = false;
    NDIlib_find_instance_t finder = nullptr;
    std::thread findThread;

#else   // sem SDK: casca que nunca acha nada

    NdiEngine() { statusText = "compilado sem o SDK do NDI"; }
    void startDiscovery() {}
    void stopDiscovery() {}
    void shutdown() {}
    bool discovering() const noexcept { return false; }
    void* api() const noexcept { return nullptr; }
    void* lib = nullptr;

#endif

    std::atomic<bool> quit { false };
    std::mutex listMutex;
    std::vector<SourceInfo> found;
    juce::String statusText;
};

// ---------------------------------------------------------------- recepcao

/** Um receptor por canal de rede. Thread propria: conecta, puxa audio pelo
    framesync e empurra na fila SPSC. O callback de audio nunca ve nada disso. */
class NdiReceiver
{
public:
    NdiReceiver (mesa::AsyncSource& target, double sampleRate, int blockSize)
        : dst (target), sr (sampleRate), block (blockSize) {}

    /** Fila da DIREITA. Com ela, o NDI entra em estereo de verdade: cada lado
        vai para a sua fila, sem mistura. Sem ela, os dois lados sao somados
        em mono, como antes. */
    void defineDireita (mesa::AsyncSource* direita) noexcept { dstR = direita; }

    ~NdiReceiver() { stop(); }

    /** Nome exato vindo da lista de fontes. */
    bool start (const std::string& sourceName)
    {
#if MESA_HAS_NDI
        stop();
        auto* lib = NdiEngine::instance().api();
        if (lib == nullptr) return false;

        NDIlib_source_t src;
        src.p_ndi_name = sourceName.c_str();

        NDIlib_recv_create_v3_t rd;
        rd.source_to_connect_to = src;
        rd.color_format = NDIlib_recv_color_format_fastest;
        rd.bandwidth    = NDIlib_recv_bandwidth_audio_only;   // so audio: nao gasta rede com video
        rd.allow_video_fields = false;

        recv = lib->recv_create_v3 (&rd);
        if (recv == nullptr) return false;

        sync = lib->framesync_create (recv);
        if (sync == nullptr) { lib->recv_destroy (recv); recv = nullptr; return false; }

        // Correcao de relogio LIGADA tambem aqui.
        //
        // Estava desligada com um argumento que parecia solido: o framesync do
        // NDI ja entrega no nosso sample rate, e corrigir de novo seria dois
        // controladores disputando a mesma fila. Na pratica o framesync
        // entregou 50469 amostras por segundo onde a mesa consome 48000 — 5% a
        // mais, todo segundo. A fila enchia, o excedente era descartado, e
        // descarte periodico soa exatamente como picotar.
        //
        // O framesync acerta a TAXA; ele nao acerta o RELOGIO da outra
        // maquina. Sao coisas diferentes, e o segundo e justamente o que esta
        // correcao existe para absorver — e o que mantem o Livewire limpo.
        dst.setDriftCorrection (true);

        quit.store (false);
        abertoMs = juce::Time::getMillisecondCounterHiRes();
        ultimoMs = 0.0;
        recebidos.store (0);
        amostras.store (0);
        worker = std::thread ([this] { pumpLoop(); });
        name = sourceName;
        return true;
#else
        juce::ignoreUnused (sourceName);
        return false;
#endif
    }

    void stop()
    {
#if MESA_HAS_NDI
        quit.store (true);
        if (worker.joinable()) worker.join();
        auto* lib = NdiEngine::instance().api();
        if (lib != nullptr)
        {
            if (sync != nullptr) { lib->framesync_destroy (sync); sync = nullptr; }
            if (recv != nullptr) { lib->recv_destroy (recv);      recv = nullptr; }
        }
#endif
    }

    bool running() const noexcept { return worker.joinable(); }
    const std::string& sourceName() const noexcept { return name; }

    /** Segundos desde o ultimo quadro com audio. Zero quando esta chegando.

        A pergunta certa nao e "ja chegou alguma coisa?", e sim "parou de
        chegar?". A versao anterior acusava perda sempre que o contador ainda
        estava em zero — inclusive nos primeiros instantes depois de conectar,
        e mesmo com a fonte na propria maquina. */
    double segundosSemAudio() const noexcept
    {
        if (ultimoMs <= 0.0) return 0.0;   // ainda nem comecou: nao e perda
        return (juce::Time::getMillisecondCounterHiRes() - ultimoMs) / 1000.0;
    }

    /** Carencia depois de abrir: nem toda fonte manda audio no primeiro
        segundo, e alarmar nesse intervalo so gera ruido. */
    bool emCarencia() const noexcept
    {
        return (juce::Time::getMillisecondCounterHiRes() - abertoMs) < 5000.0;
    }

    int quadros() const noexcept { return recebidos.load(); }

    /** Amostras por segundo que a fonte esta REALMENTE entregando.

        O numero que responde a pergunta mais util sobre uma fonte de rede:
        ela esta no ritmo da mesa? Abaixo da taxa da placa, falta audio e o
        som fica picotado; acima, a fila enche e o excedente e descartado, o
        que soa igual. Sem isto, "esta distorcido" nao tem como virar
        diagnostico. */
    double amostrasPorSegundo() const noexcept
    {
        const double agora = juce::Time::getMillisecondCounterHiRes();
        const double dt = (agora - abertoMs) / 1000.0;
        return dt > 1.0 ? double (amostras.load()) / dt : 0.0;
    }

private:
#if MESA_HAS_NDI
    void pumpLoop()
    {
        mesa::batizaThread ("ndi-pumpLoop");
        auto* lib = NdiEngine::instance().api();
        std::vector<float> mono (size_t (block), 0.0f);
        direita.assign (size_t (block), 0.0f);    // fora do laco: nao aloca por bloco

        while (! quit.load())
        {
            NDIlib_audio_frame_v2_t frame;
            // DOIS CANAIS, e a mistura feita aqui.
            //
            // Pediamos UM canal e deixavamos a conversao do NDI somar o
            // estereo. Ela soma sem atenuar: dois lados parecidos chegam a
            // +6 dB e estouram o teto digital. Fonte com musica saturava o
            // tempo todo, e o som chegava distorcido mesmo com a fonte
            // perfeita - era a mesa que estourava, nao o emissor.
            //
            // Pedindo os dois e misturando com metade de cada, o nivel sai
            // certo. Fonte mono vem com os dois lados iguais e a conta devolve
            // o mesmo sinal, entao nao ha caso especial.
            // PEDIR O BLOCO, sempre.
            //
            // Tentei pedir zero - "me de o que houver" - para o framesync
            // parar de completar com silencio quando o emissor atrasa. Nesta
            // implementacao zero devolve NADA: o canal foi para -100 dBFS e
            // ficou mudo. O contrato aqui e outro - pede-se um numero e ele
            // entrega esse numero, inventando silencio se precisar.
            //
            // Fica registrado para ninguem tentar de novo achando que resolve.
            lib->framesync_capture_audio (sync, &frame, int (sr), 2, block);

            if (frame.p_data != nullptr && frame.no_samples > 0)
            {
                // Empurra SO o que veio, sem completar com silencio.
                //
                // Preencher o resto do bloco com zeros metia um buraco no meio
                // do audio a cada quadro curto — e nenhum contador acusava,
                // porque o bloco ia cheio. O som saia picotado e metalico com
                // a fila reportando saude perfeita. Faltando amostra, quem
                // decide o que fazer e a fila, que sabe o seu proprio estado.
                const int n = frame.no_samples < block ? frame.no_samples : block;

                // O audio do NDI vem em PLANOS por canal, nao intercalado: a
                // direita comeca channel_stride_in_bytes depois da esquerda.
                const float* esq = frame.p_data;
                const float* dir = esq;
                if (frame.no_channels >= 2 && frame.channel_stride_in_bytes > 0)
                    dir = reinterpret_cast<const float*> (
                              reinterpret_cast<const char*> (frame.p_data)
                              + frame.channel_stride_in_bytes);

                // ESTEREO DE VERDADE quando ha fila para a direita.
                //
                // Misturar os dois lados em mono custava nivel: em musica, onde
                // esquerda e direita diferem, a soma com metade de cada perde
                // ate 3 dB - e esse mono ainda saia igual dos dois lados na
                // saida NDI. No vMix, o que passava pela mesa chegava mais
                // baixo e mais estreito que a mesma fonte vinda direto.
                //
                // Com cada lado na sua fila, fader em 0 e pan no centro, o que
                // sai e o que entrou.
                const float fator = NdiEngine::instance().fatorEntrada.load (std::memory_order_relaxed);
                if (dstR != nullptr)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        mono   [size_t (i)] = esq[i] * fator;
                        direita[size_t (i)] = dir[i] * fator;
                    }
                    dst.push (mono.data(), n);
                    dstR->push (direita.data(), n);
                }
                else
                {
                    for (int i = 0; i < n; ++i)
                        mono[size_t (i)] = 0.5f * (esq[i] + dir[i]) * fator;
                    dst.push (mono.data(), n);
                }
                ultimoMs = juce::Time::getMillisecondCounterHiRes();
                recebidos.fetch_add (1);
                amostras.fetch_add (n);
            }
            lib->framesync_free_audio (sync, &frame);

            // Ritmo por PRAZO ACUMULADO, nao por dormida arredondada.
            //
            // Dormir "block/sr milissegundos" parece certo e nao e: 512
            // amostras a 48 kHz duram 10,67 ms, e o arredondamento para
            // inteiro dorme 10. Sao 6,7% de voltas a mais por segundo, todo
            // segundo — foi assim que esta fonte entregou 50469 amostras onde
            // a mesa consome 48000. A fila enchia e o excedente era
            // descartado, o que soa exatamente como picotar.
            //
            // Guardando o prazo do proximo bloco em ponto flutuante, a fracao
            // nao se perde: ela se acumula e a dormida seguinte a absorve.
            prazo += 1000.0 * double (block) / sr;
            const double agora = juce::Time::getMillisecondCounterHiRes();
            const double falta = prazo - agora;

            if (falta > 1.0)
                std::this_thread::sleep_for (
                    std::chrono::milliseconds (int (falta)));
            else if (falta < -100.0)
                prazo = agora;        // atrasou demais: recomeca do relogio
        }
    }

    NDIlib_recv_instance_t recv = nullptr;
    NDIlib_framesync_instance_t sync = nullptr;
#endif

    mesa::AsyncSource& dst;
    mesa::AsyncSource* dstR = nullptr;
    std::vector<float> direita;
    std::atomic<double> ultimoMs { 0.0 };
    double abertoMs = 0.0;
    /** Prazo do proximo bloco, em ponto flutuante: a fracao de milissegundo
        nao se perde entre uma volta e outra. */
    double prazo = 0.0;
    std::atomic<int> recebidos { 0 };
    std::atomic<long long> amostras { 0 };
    double sr;
    int block;
    std::string name;
    std::atomic<bool> quit { true };
    std::thread worker;
};

// ------------------------------------------------------------------ envio

/** Saida NDI. O callback de audio so escreve na fila; a thread envia.
    Nada de socket dentro do callback. */
/** TRANSMISSOR NDI de uma saida da mesa.

    Puxa de duas filas - esquerda e direita - exatamente como o transmissor
    Livewire. Isso nao e detalhe: e o mesmo caminho por onde a mesa manda audio
    para qualquer rede, e por ele passam barramento, direct out, mix-minus e o
    gerador de teste. Pondo o NDI nesse caminho, ele herda tudo isso de uma
    vez, em vez de ganhar uma segunda maneira de decidir o que sai.

    O ritmo e dado por relogio proprio, com prazo acumulado: dormir "10 ms" a
    cada volta arredonda e deriva, e a fila do outro lado sente isso como
    audio picado. Foi o mesmo defeito do receptor NDI, corrigido do mesmo
    jeito. */
class NdiTransmissor
{
public:
    NdiTransmissor (mesa::AsyncSource& esquerda, mesa::AsyncSource& direita)
        : esq (esquerda), dir (direita) {}

    ~NdiTransmissor() { stop(); }

    /** Bloco de 10 ms a 48 kHz: pequeno o bastante para nao pesar na latencia,
        grande o bastante para nao inundar a rede de pacotes minusculos. */
    static constexpr int kBloco = 480;

    bool start (const std::string& nome, double sampleRate)
    {
#if MESA_HAS_NDI
        stop();
        auto* lib = NdiEngine::instance().api();
        if (lib == nullptr) { erro = "biblioteca do NDI nao carregada"; return false; }

        nomePublicado = nome;
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;

        NDIlib_send_create_t sd;
        sd.p_ndi_name = nomePublicado.c_str();
        sd.p_groups   = nullptr;
        // Relogio NOSSO, nao do NDI: o laco abaixo ja anda no tempo certo.
        // Deixar os dois cronometrarem faz cada envio esperar em dobro.
        sd.clock_audio = false;
        sd.clock_video = false;

        sender = lib->send_create (&sd);
        if (sender == nullptr) { erro = "o NDI recusou criar o emissor"; return false; }

        quit.store (false);
        worker = std::thread ([this] { laco(); });
        return true;
#else
        juce::ignoreUnused (nome, sampleRate);
        erro = "mesa compilada sem NDI";
        return false;
#endif
    }

    void stop()
    {
#if MESA_HAS_NDI
        quit.store (true);
        if (worker.joinable()) worker.join();
        auto* lib = NdiEngine::instance().api();
        if (lib != nullptr && sender != nullptr) { lib->send_destroy (sender); sender = nullptr; }
#endif
    }

    long long blocos() const noexcept { return enviados.load(); }
    bool ativo() const noexcept { return ! quit.load(); }
    const std::string& nome() const noexcept { return nomePublicado; }
    const std::string& ultimoErro() const noexcept { return erro; }

private:
#if MESA_HAS_NDI
    void laco()
    {
        mesa::batizaThread ("ndi-transmissor");
        auto* lib = NdiEngine::instance().api();
        std::vector<float> planar (size_t (kBloco) * 2, 0.0f);

        const double passoMs = 1000.0 * double (kBloco) / sr;
        double prazo = juce::Time::getMillisecondCounterHiRes();

        while (! quit.load())
        {
            // NDI quer planar: o canal esquerdo inteiro, depois o direito
            std::memcpy (planar.data(),          esq.pull (kBloco), sizeof (float) * size_t (kBloco));
            std::memcpy (planar.data() + kBloco, dir.pull (kBloco), sizeof (float) * size_t (kBloco));

            // escala cheia da mesa -> convencao do NDI (ver fatorSaida)
            const float fator = NdiEngine::instance().fatorSaida.load (std::memory_order_relaxed);
            if (fator != 1.0f)
                for (auto& v : planar) v *= fator;

            NDIlib_audio_frame_v2_t f;
            f.sample_rate = int (sr);
            f.no_channels = 2;
            f.no_samples  = kBloco;
            f.p_data      = planar.data();
            f.channel_stride_in_bytes = int (sizeof (float)) * kBloco;
            lib->send_send_audio_v2 (sender, &f);
            enviados.fetch_add (1);

            prazo += passoMs;
            const double falta = prazo - juce::Time::getMillisecondCounterHiRes();
            if (falta > 0.0)
                std::this_thread::sleep_for (std::chrono::microseconds (int64_t (falta * 1000.0)));
            else if (falta < -200.0)
                prazo = juce::Time::getMillisecondCounterHiRes();   // perdeu o passo: recomeca
        }
    }

    NDIlib_send_instance_t sender = nullptr;
#endif

    mesa::AsyncSource& esq;
    mesa::AsyncSource& dir;
    std::string nomePublicado, erro;
    double sr = 48000.0;
    std::atomic<bool> quit { true };
    std::atomic<long long> enviados { 0 };
    std::thread worker;
};

class NdiSender
{
public:
    NdiSender (double sampleRate, int blockSize, int channels = 2)
        : sr (sampleRate), block (blockSize), chans (channels)
    {
        ring.prepare (blockSize * chans * 16);
    }

    ~NdiSender() { stop(); }

    bool start (const std::string& senderName)
    {
#if MESA_HAS_NDI
        stop();
        auto* lib = NdiEngine::instance().api();
        if (lib == nullptr) return false;

        NDIlib_send_create_t sd;
        sd.p_ndi_name = senderName.c_str();
        sd.clock_audio = false;   // quem manda no relogio e a placa, nao o NDI
        sd.clock_video = false;

        sender = lib->send_create (&sd);
        if (sender == nullptr) return false;

        quit.store (false);
        worker = std::thread ([this] { sendLoop(); });
        return true;
#else
        juce::ignoreUnused (senderName);
        return false;
#endif
    }

    void stop()
    {
#if MESA_HAS_NDI
        quit.store (true);
        if (worker.joinable()) worker.join();
        auto* lib = NdiEngine::instance().api();
        if (lib != nullptr && sender != nullptr) { lib->send_destroy (sender); sender = nullptr; }
#endif
    }

    /** Chamado do callback de audio. Intercalado L,R,L,R — sem alocar, sem travar. */
    void pushInterleaved (const float* l, const float* r, int n) noexcept
    {
        if (n * 2 > int (staging.size())) return;   // nunca realoca aqui
        for (int i = 0; i < n; ++i) { staging[size_t (i * 2)] = l[i]; staging[size_t (i * 2 + 1)] = r[i]; }
        ring.write (staging.data(), n * 2);
    }

    void prepare (int maxBlockSize) { staging.assign (size_t (maxBlockSize * 2), 0.0f); }

private:
#if MESA_HAS_NDI
    void sendLoop()
    {
        mesa::batizaThread ("ndi-sendLoop");
        auto* lib = NdiEngine::instance().api();
        std::vector<float> inter (size_t (block * chans), 0.0f);
        std::vector<float> planar (size_t (block * chans), 0.0f);

        while (! quit.load())
        {
            if (ring.used() >= block * chans)
            {
                ring.read (inter.data(), block * chans);
                // NDI quer planar: canal 0 inteiro, depois canal 1
                for (int i = 0; i < block; ++i)
                    for (int c = 0; c < chans; ++c)
                        planar[size_t (c * block + i)] = inter[size_t (i * chans + c)];

                NDIlib_audio_frame_v2_t f;
                f.sample_rate = int (sr);
                f.no_channels = chans;
                f.no_samples  = block;
                f.p_data      = planar.data();
                f.channel_stride_in_bytes = int (sizeof (float)) * block;
                lib->send_send_audio_v2 (sender, &f);
            }
            else
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        }
    }

    NDIlib_send_instance_t sender = nullptr;
#endif

    mesa::RingBuffer ring;
    std::vector<float> staging;
    double sr;
    int block, chans;
    std::atomic<bool> quit { true };
    std::thread worker;
};
