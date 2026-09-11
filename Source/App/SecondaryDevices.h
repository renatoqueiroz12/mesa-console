#pragma once
#include <algorithm>
#include <juce_audio_devices/juce_audio_devices.h>
#include "../Core/AsyncSource.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>

/** Uma placa SECUNDARIA.

    Sobre a restricao que molda tudo aqui: o Windows abre UM driver ASIO por
    vez. A placa mestra fica com o ASIO e roda o callback — ela manda no
    relogio. Qualquer outra placa entra por este caminho, em thread propria,
    empurrando nas filas SPSC. Do ponto de vista do canal, e indistinguivel de
    uma fonte de rede.

    O preco: o WASAPI acrescenta dezenas de milissegundos, e o cristal da
    secundaria nunca corre na taxa da mestra. O primeiro e motivo para nunca
    pendurar microfone aqui; o segundo e resolvido pela correcao de deriva
    dentro da AsyncSource.

    A placa PODE sumir no meio do programa — USB solta, driver reinicia. Nesse
    caso nada trava: as filas passam a entregar silencio, o estado vira perdido,
    e uma thread tenta reabrir periodicamente. */
class SecondaryDevice : private juce::AudioIODeviceCallback,
                        private juce::Timer
{
public:
    /** latencyMode: 0 = minima, 1 = equilibrada, 2 = segura. */
    SecondaryDevice (juce::String typeName, juce::String deviceName,
                     double sampleRate, int blockSize, int latencyMode = 0)
        : type (std::move (typeName)), name (std::move (deviceName)),
          sr (sampleRate), block (blockSize), mode (latencyMode)
    {
        open();
        startTimer (2000);   // vigia de reconexao
    }

    ~SecondaryDevice() override
    {
        stopTimer();
        close();
    }

    const juce::String& deviceName() const noexcept { return name; }
    /** Nome real aberto: com "*PADRAO*" ele muda conforme o Windows. */
    const juce::String& deviceNameReal() const noexcept
    { return resolvido.isEmpty() ? name : resolvido; }
    int numInputs() const noexcept { return channels; }
    bool isLost() const noexcept { return lost.load(); }

    int numOutputs() const noexcept { return outChannels; }

    /** Fila de uma SAIDA desta placa: a mesa empurra aqui, o callback da placa
        puxa com a conversao de taxa. */
    mesa::AsyncSource* sink (int ch) noexcept
    {
        std::lock_guard<std::mutex> g (mutex);
        return (ch >= 0 && ch < int (sinks.size())) ? sinks[size_t (ch)].get() : nullptr;
    }

    /** Fila de uma entrada desta placa. Nula se o indice nao existe. */
    mesa::AsyncSource* source (int ch) noexcept
    {
        std::lock_guard<std::mutex> g (mutex);
        return (ch >= 0 && ch < int (queues.size())) ? queues[size_t (ch)].get() : nullptr;
    }

    /** Quantas vezes esta placa caiu desde que o app abriu, e ha quanto tempo
        esta fora. Serve para o operador saber se e cabo ruim ou caso isolado. */
    int  dropouts() const noexcept { return drops.load(); }
    double sampleRate() const noexcept { return actualRate; }
    int  blocos() const noexcept { return blocosRecebidos.load(); }
    long long amostras() const noexcept { return amostrasRecebidas.load(); }
    bool recebendo() const noexcept { return blocosRecebidos.load() > 0; }

    /** Amostras que CHEGARAM por segundo, medidas de verdade.

        A taxa que o dispositivo declara e a que ele entrega podem divergir —
        e quando divergem, a fila seca e o audio sai picotado sem que nenhum
        contador de erro acuse nada. Comparar este numero com a taxa declarada
        e a forma direta de ver isso. */
    double amostrasPorSegundo() noexcept
    {
        const double agora = juce::Time::getMillisecondCounterHiRes();
        const long long total = amostrasRecebidas.load();

        if (ultimaMedidaMs <= 0.0) { ultimaMedidaMs = agora; ultimoTotal = total; return 0.0; }

        const double dt = (agora - ultimaMedidaMs) / 1000.0;
        if (dt < 0.5) return ultimaTaxa;

        ultimaTaxa = double (total - ultimoTotal) / dt;
        ultimaMedidaMs = agora;
        ultimoTotal = total;
        return ultimaTaxa;
    }

    int canaisAtivos() const noexcept { return channels; }
    int blocoDoDispositivo() const noexcept { return blocoDisp; }

    /** Falhas de fila desde a abertura. No modo de latencia minima este numero
        e o juiz: se sobe durante a operacao, a fila esta rasa demais. */
    int glitches()
    {
        std::lock_guard<std::mutex> g (mutex);
        int t = 0;
        for (auto& q : queues) if (q != nullptr) t += q->faltas() + q->overflows();
        for (auto& q : sinks)  if (q != nullptr) t += q->faltas() + q->overflows();
        return t;
    }

    /** Separado, porque as causas sao OPOSTAS: fila vazia pede mais folga,
        fila cheia pede menos. Somar os dois esconde qual e o problema. */
    int faltas()
    {
        std::lock_guard<std::mutex> g (mutex);
        int t = 0;
        for (auto& q : queues) if (q != nullptr) t += q->faltas();
        for (auto& q : sinks)  if (q != nullptr) t += q->faltas();
        return t;
    }

    /** Amostras que viraram buraco no audio. E o numero que casa com o ouvido. */
    int buracos()
    {
        std::lock_guard<std::mutex> g (mutex);
        int t = 0;
        for (auto& q : queues) if (q != nullptr) t += q->amostrasEmFalta();
        for (auto& q : sinks)  if (q != nullptr) t += q->amostrasEmFalta();
        return t;
    }
    int descartes()
    {
        std::lock_guard<std::mutex> g (mutex);
        int t = 0;
        for (auto& q : queues) if (q != nullptr) t += q->overflows();
        for (auto& q : sinks)  if (q != nullptr) t += q->overflows();
        return t;
    }

    /** Latencia media que esta placa acrescenta, medida nas filas. */
    double latencyMs()
    {
        std::lock_guard<std::mutex> g (mutex);
        double worst = 0.0;
        for (auto& q : queues) if (q != nullptr) worst = std::max (worst, q->latencyMs());
        for (auto& q : sinks)  if (q != nullptr) worst = std::max (worst, q->latencyMs());
        return worst;
    }
    juce::String error() const { return lastError; }
    double secondsLost() const noexcept
    {
        if (! lost.load()) return 0.0;
        return (juce::Time::getMillisecondCounterHiRes() - lostAtMs) / 1000.0;
    }

    /** Avisa quem estiver ouvindo: a superficie usa para alertar sem varrer. */
    std::function<void (const juce::String&, bool)> onStateChange;

private:
    void open()
    {
        std::unique_ptr<juce::AudioIODeviceType> t (makeType());
        if (t == nullptr) { markLost(); return; }

        t->scanForDevices();

        // "*PADRAO*" nao e nome de placa: e um pedido para usar a que o
        // Windows estiver usando agora. Resolvido aqui, na abertura, e nao na
        // configuracao — assim continua valendo depois de trocar o dispositivo
        // padrao no Windows, sem mexer na mesa.
        if (name == "*PADRAO*")
        {
            const auto nomes = t->getDeviceNames (true);
            const int padrao = t->getDefaultDeviceIndex (true);
            if (padrao < 0 || padrao >= nomes.size())
            { lastError = "o Windows nao tem entrada padrao"; markLost(); return; }
            resolvido = nomes[padrao];
        }
        else
        {
            resolvido = name;
            if (! t->getDeviceNames (true).contains (name)) { markLost(); return; }
        }

        std::unique_ptr<juce::AudioIODevice> d (t->createDevice ({}, resolvido));
        if (d == nullptr) { markLost(); return; }

        juce::BigInteger ins, outs;
        ins .setRange (0, d->getInputChannelNames().size(), true);
        outs.setRange (0, d->getOutputChannelNames().size(), true);

        // pede a nossa taxa, mas aceita a que a placa impuser: em WASAPI
        // compartilhado quem manda e a configuracao do Windows
        double wanted = sr;
        const auto rates = d->getAvailableSampleRates();
        if (! rates.isEmpty() && ! rates.contains (wanted))
        {
            double best = rates[0];
            for (auto r : rates) if (std::abs (r - sr) < std::abs (best - sr)) best = r;
            wanted = best;
        }

        auto err = d->open (ins, outs, wanted, block);
        if (err.isNotEmpty() && ! rates.isEmpty())
            err = d->open (ins, outs, rates[0], block);   // ultima tentativa
        if (err.isNotEmpty()) { lastError = err; markLost(); return; }

        actualRate = d->getCurrentSampleRate();
        const int outNames = d->getOutputChannelNames().size();
        lastError.clear();

        {
            std::lock_guard<std::mutex> g (mutex);
            channels = d->getActiveInputChannels().countNumberOfSetBits();

            // A fila tem que caber o bloco DE QUEM ENTREGA, nao o da placa
            // mestra. Com a UMC em 8 amostras e o WASAPI entregando 480 de
            // uma vez, cabiam 32 e chegavam 480: transbordava em todo bloco e
            // o audio passava picotado. Foi o que produziu aqueles milhoes de
            // falhas no batimento.
            const int blocoDoDispositivo = juce::jmax (d->getCurrentBufferSizeSamples(), 64);
            const int blocoFila = juce::jmax (block, blocoDoDispositivo * 2, 512);
            blocoDisp = blocoDoDispositivo;

            queues.clear();
            queues.reserve (size_t (channels));
            for (int i = 0; i < channels; ++i)
            {
                auto q = std::make_unique<mesa::AsyncSource>();
                // Profundidade da fila = LATENCIA desta placa. O ASIO entrega em
                // ritmo regular e aceita fila rasa; o WASAPI compartilhado chega
                // em rajadas e precisa de folga, senao pipoca underrun.
                q->prepare (blocoFila, queueBlocks(), sr, queueFill());
                // ESSENCIAL: a placa pode estar em 44100 com a mesa em 48000.
                // Sem informar isso, a correcao de deriva nao da conta e o
                // audio sai distorcido.
                q->setSourceSampleRate (actualRate);
                q->name = (name + " - " + juce::String (i + 1)).toStdString();
                q->kind = mesa::AsyncSource::Kind::Local;
                queues.push_back (std::move (q));
            }
            // no comeco todas sao alimentadas: so depois da primeira
            // verificacao sabemos quais estao sendo lidas
            filaLida.assign (queues.size(), true);
            contaBlocos = 0;
            // filas de SAIDA. A conversao de taxa mora do lado de quem consome:
            // a mesa empurra na taxa dela, a placa puxa na taxa dela.
            outChannels = int (outNames);
            juce::ignoreUnused (blocoDoDispositivo);
            sinks.clear();
            sinks.reserve (size_t (outChannels));
            for (int i = 0; i < outChannels; ++i)
            {
                auto q = std::make_unique<mesa::AsyncSource>();
                q->prepare (blocoFila, queueBlocks(), actualRate, queueFill());  // taxa de quem PUXA
                q->setSourceSampleRate (sr);               // taxa de quem EMPURRA
                q->name = (name + " out " + juce::String (i + 1)).toStdString();
                sinks.push_back (std::move (q));
            }

            deviceType = std::move (t);
            device = std::move (d);
        }

        blocosRecebidos.store (0);
        amostrasRecebidas.store (0);
        device->start (this);

        const bool was = lost.exchange (false);
        if (was && onStateChange) onStateChange (name, true);
    }

    void close()
    {
        std::unique_ptr<juce::AudioIODevice> d;
        {
            std::lock_guard<std::mutex> g (mutex);
            d = std::move (device);
        }
        if (d != nullptr) { d->stop(); d->close(); }
        std::lock_guard<std::mutex> g (mutex);
        deviceType.reset();
    }

    void markLost()
    {
        if (! lost.exchange (true))
        {
            drops.fetch_add (1);
            lostAtMs = juce::Time::getMillisecondCounterHiRes();
            std::lock_guard<std::mutex> g (mutex);
            for (auto& q : queues) if (q != nullptr) q->setLost (true);
            if (onStateChange) onStateChange (name, false);
        }
    }

    /** Dois drivers ASIO DIFERENTES convivem sem problema — o que nao existe e
        abrir o MESMO driver duas vezes. Como secundaria, o ASIO e muito melhor
        que o WASAPI: taxa exata, latencia baixa e sem remixagem do Windows. */
    bool isAsio() const noexcept { return type == "ASIO"; }

    /** Fila mais rasa = menos latencia e menos margem. No modo minimo ficamos
        com 2 blocos no ASIO: e o menor valor que ainda absorve uma rajada.
        Abaixo disso o underrun deixa de ser eventual e vira constante. */
    int queueBlocks() const noexcept
    {
        if (isAsio()) return mode == 0 ? 2 : mode == 1 ? 4 : 8;
        return mode == 0 ? 12 : mode == 1 ? 16 : 24;
    }

    /** Quanto da fila fica cheia em regime.

        O WASAPI compartilhado nao entrega em ritmo constante: manda uma
        rajada, silencia, manda duas juntas. Segurar so 25% deixava a fila
        secar entre rajadas. Com metade, ela atravessa o intervalo — ao custo
        de alguns milissegundos a mais, que neste caminho nao fazem falta. */
    double queueFill() const noexcept
    {
        if (isAsio()) return mode == 0 ? 0.25 : mode == 1 ? 0.3 : 0.4;

        // WASAPI segura MEIA fila e ainda assim secava entre rajadas: o audio
        // gravado saia com buracos de 6 ms, um por periodo de entrega. O
        // remedio nao e sutil — e folga. Estes milissegundos a mais nao fazem
        // falta neste caminho, e buraco no audio inviabiliza transcricao.
        return mode == 0 ? 0.6 : mode == 1 ? 0.65 : 0.7;
    }

    juce::AudioIODeviceType* makeType() const
    {
       #if JUCE_ASIO
        if (type == "ASIO") return juce::AudioIODeviceType::createAudioIODeviceType_ASIO();
       #endif
        if (type == "Windows Audio (Exclusive Mode)")
            return juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::exclusive);
        if (type == "DirectSound")
            return juce::AudioIODeviceType::createAudioIODeviceType_DirectSound();
        return juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared);
    }

    // ---- callback da placa secundaria: so copia para as filas, nada mais
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut,
                                           int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
        {
            if (out[c] == nullptr) continue;
            if (c < int (sinks.size()) && sinks[size_t (c)] != nullptr)
                juce::FloatVectorOperations::copy (out[c], sinks[size_t (c)]->pull (n), n);
            else
                juce::FloatVectorOperations::clear (out[c], n);
        }

        // So alimenta fila que alguem esta lendo. A verificacao e barata e
        // roda uma vez a cada 100 blocos: fila orfa de dispositivo estereo
        // gerava milhares de descartes por minuto sem servir para nada.
        if (++contaBlocos >= 100)
        {
            contaBlocos = 0;
            for (size_t i = 0; i < queues.size(); ++i)
                if (queues[i] != nullptr)
                    filaLida[i] = queues[i]->temConsumidor();
        }

        for (int c = 0; c < numIn && c < int (queues.size()); ++c)
            if (in[c] != nullptr && queues[size_t (c)] != nullptr
                && (filaLida.size() <= size_t (c) || filaLida[size_t (c)]))
                queues[size_t (c)]->push (in[c], n);

        // Prova de vida do dispositivo. Sem isto, "fila com milhoes de falhas"
        // e ambiguo: pode ser fila mal dimensionada ou dispositivo que abriu e
        // nunca entregou nada. Contando os blocos que CHEGARAM, a diferenca
        // fica obvia.
        blocosRecebidos.fetch_add (1, std::memory_order_relaxed);
        amostrasRecebidas.fetch_add (n, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}

    void audioDeviceStopped() override
    {
        // chamado quando o driver cai sozinho: e aqui que a perda e detectada
        markLost();
    }

    void audioDeviceError (const juce::String&) override { markLost(); }

    void timerCallback() override
    {
        if (! lost.load()) return;
        close();
        open();          // se a placa voltou, o audio volta sozinho
    }

    juce::String type, name, resolvido;
    double sr;
    int block;
    int mode = 0;
    int channels = 0;

    std::mutex mutex;
    std::unique_ptr<juce::AudioIODeviceType> deviceType;
    std::unique_ptr<juce::AudioIODevice> device;
    std::vector<std::unique_ptr<mesa::AsyncSource>> queues;   // entradas
    std::vector<std::unique_ptr<mesa::AsyncSource>> sinks;    // saidas
    int outChannels = 0;
    std::vector<char> filaLida;      // char e nao bool: vector<bool> nao e seguro aqui
    int contaBlocos = 0;

    double actualRate = 0.0;
    juce::String lastError;
    std::atomic<bool> lost { false };
    std::atomic<int> blocosRecebidos { 0 };
    std::atomic<long long> amostrasRecebidas { 0 };
    double ultimaMedidaMs = 0.0, ultimaTaxa = 0.0;
    long long ultimoTotal = 0;
    int blocoDisp = 0;
    std::atomic<int>  drops { 0 };
    double lostAtMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SecondaryDevice)
};

/** Conjunto das secundarias. A mestra continua no AudioDeviceManager do JUCE. */
class SecondaryDevices
{
public:
    /** Abre, ou devolve a que ja existe. */
    void setLatencyMode (int m) noexcept { mode = m; }

    SecondaryDevice* open (const juce::String& type, const juce::String& name,
                           double sr, int block)
    {
        for (auto& d : devices) if (d->deviceName() == name) return d.get();

        auto d = std::make_unique<SecondaryDevice> (type, name, sr, block, mode);
        d->onStateChange = [this] (const juce::String& n, bool ok) { note (n, ok); };
        devices.push_back (std::move (d));
        return devices.back().get();
    }

    SecondaryDevice* find (const juce::String& name)
    {
        for (auto& d : devices) if (d->deviceName() == name) return d.get();
        return nullptr;
    }

    void closeAll() { devices.clear(); }

    /** Fecha as placas que sairam do catalogo.

        So havia closeAll, chamado ao ENCERRAR a mesa. Enquanto ela rodava, uma
        placa tirada de uso continuava aberta: seguia consumindo CPU, seguia
        tentando entregar audio que ninguem lia e seguia acumulando erro no
        batimento — milhoes de buracos de um dispositivo que o operador achava
        que tinha desligado.

        Recebe os nomes que ainda tem dono; o resto e fechado. */
    void fechaAsQueSairam (const std::vector<juce::String>& emUso)
    {
        for (auto it = devices.begin(); it != devices.end(); )
        {
            const auto nome = (*it)->deviceName();
            const bool usada = std::find (emUso.begin(), emUso.end(), nome) != emUso.end();
            if (usada) { ++it; continue; }
            it = devices.erase (it);        // o destrutor para e fecha
        }
    }

    /** Nomes das placas atualmente perdidas — o alerta da barra de status. */
    std::vector<juce::String> lostDevices() const
    {
        std::vector<juce::String> v;
        for (auto& d : devices) if (d->isLost()) v.push_back (d->deviceName());
        return v;
    }

    /** Eventos ainda nao mostrados ao operador. */
    std::vector<juce::String> takeEvents()
    {
        std::lock_guard<std::mutex> g (mutex);
        auto out = std::move (events);
        events.clear();
        return out;
    }

    int count() const noexcept { return int (devices.size()); }
    SecondaryDevice* at (int i) noexcept
    { return (i >= 0 && i < int (devices.size())) ? devices[size_t (i)].get() : nullptr; }

private:
    void note (const juce::String& n, bool ok)
    {
        std::lock_guard<std::mutex> g (mutex);
        events.push_back (ok ? ("placa recuperada: " + n)
                             : ("PLACA PERDIDA: " + n + " - verifique a conexao"));
    }

    std::vector<std::unique_ptr<SecondaryDevice>> devices;
    int mode = 0;
    std::mutex mutex;
    std::vector<juce::String> events;
};
