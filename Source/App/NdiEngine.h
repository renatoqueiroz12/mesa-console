#pragma once
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
    static const NDIlib_v5* loadRuntime()
    {
       #if JUCE_WINDOWS
        std::string path;
        if (const char* dir = std::getenv (NDILIB_REDIST_FOLDER))
            path = std::string (dir) + "\\" + NDILIB_LIBRARY_NAME;

        HMODULE h = path.empty() ? nullptr : LoadLibraryA (path.c_str());
        if (h == nullptr) h = LoadLibraryA (NDILIB_LIBRARY_NAME);   // tenta pelo PATH
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
            statusText = "runtime NDI nao encontrado (instale o NDI Runtime)";
            return;
        }
        if (! lib->initialize())
        {
            lib = nullptr;
            statusText = "CPU sem suporte ao NDI";
            return;
        }
        statusText = "NDI pronto (descoberta parada)";
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

    ~NdiEngine()
    {
        stopDiscovery();
        if (lib != nullptr) lib->destroy();
    }

    const NDIlib_v5* api() const noexcept { return lib; }

private:
    void findLoop()
    {
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
    NDIlib_find_instance_t finder = nullptr;
    std::thread findThread;

#else   // sem SDK: casca que nunca acha nada

    NdiEngine() { statusText = "compilado sem o SDK do NDI"; }
    void startDiscovery() {}
    void stopDiscovery() {}
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

        // o framesync do NDI ja entrega no nosso sample rate: corrigir de novo
        // seria dois controladores disputando a mesma fila
        dst.setDriftCorrection (false);

        quit.store (false);
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

private:
#if MESA_HAS_NDI
    void pumpLoop()
    {
        auto* lib = NdiEngine::instance().api();
        std::vector<float> mono (size_t (block), 0.0f);

        while (! quit.load())
        {
            NDIlib_audio_frame_v2_t frame;
            // o framesync devolve SEMPRE block amostras no nosso sample rate,
            // completando com silencio se o emissor atrasar
            lib->framesync_capture_audio (sync, &frame, int (sr), 1, block);

            if (frame.p_data != nullptr && frame.no_samples > 0)
            {
                const int n = frame.no_samples < block ? frame.no_samples : block;
                std::memcpy (mono.data(), frame.p_data, size_t (n) * sizeof (float));
                for (int i = n; i < block; ++i) mono[size_t (i)] = 0.0f;
                dst.push (mono.data(), block);
            }
            lib->framesync_free_audio (sync, &frame);

            // ritmo do bloco: dorme um pouco menos para a fila nao secar
            const int ms = int (1000.0 * double (block) / sr * 0.5);
            std::this_thread::sleep_for (std::chrono::milliseconds (ms > 1 ? ms : 1));
        }
    }

    NDIlib_recv_instance_t recv = nullptr;
    NDIlib_framesync_instance_t sync = nullptr;
#endif

    mesa::AsyncSource& dst;
    double sr;
    int block;
    std::string name;
    std::atomic<bool> quit { true };
    std::thread worker;
};

// ------------------------------------------------------------------ envio

/** Saida NDI. O callback de audio so escreve na fila; a thread envia.
    Nada de socket dentro do callback. */
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
