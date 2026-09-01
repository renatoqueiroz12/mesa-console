#pragma once
#include <juce_core/juce_core.h>
#include <vector>

/** Le a lista de entradas do vMix pela API em XML.

    O vMix publica o estado inteiro em http://host:8088/api, e de la saem o
    numero e o TITULO de cada entrada. Isso importa porque a mesa chamava tudo
    de "CAM 1, CAM 2" enquanto o vMix mostra "1 - Pulsar BG": dois vocabularios
    para a mesma coisa, e confusao garantida na hora do ar.

    Porta 8088 e a do controlador web, diferente da 8099 que usamos para mandar
    comando. Uma consulta so, sob demanda: manter conexao aberta com o vMix o
    dia todo seria mais uma coisa para falhar de madrugada. */
class VmixClient
{
public:
    struct Input
    {
        int number = 0;             // o mesmo numero que vai em Input=N
        juce::String title;         // como aparece no vMix
        juce::String type;          // Capture, NDI, VideoList...
        juce::String key;
    };

    struct Result
    {
        bool ok = false;
        juce::String error, version, edition;
        std::vector<Input> inputs;
    };

    static Result query (const juce::String& host, int port = 8088, int timeoutMs = 3000)
    {
        Result r;
        const juce::String url = "http://" + host + ":" + juce::String (port) + "/api";

        juce::URL u (url);
        int status = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (timeoutMs)
                        .withStatusCode (&status);

        std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
        if (in == nullptr)
        {
            r.error = "sem resposta em " + host + ":" + juce::String (port)
                    + " (o controlador web do vMix esta ligado?)";
            return r;
        }

        const auto xml = in->readEntireStreamAsString();
        if (xml.isEmpty()) { r.error = "conectou mas veio vazio"; return r; }

        auto doc = juce::XmlDocument::parse (xml);
        if (doc == nullptr) { r.error = "resposta nao e XML valido"; return r; }

        r.version = doc->getChildElementAllSubText ("version", {});
        r.edition = doc->getChildElementAllSubText ("edition", {});

        if (auto* inputs = doc->getChildByName ("inputs"))
        {
            for (auto* e : inputs->getChildWithTagNameIterator ("input"))
            {
                Input i;
                i.number = e->getIntAttribute ("number");
                i.key    = e->getStringAttribute ("key");
                i.type   = e->getStringAttribute ("type");
                // o titulo e o texto do elemento; alguns vMix trazem tambem
                // como atributo, entao aceitamos os dois
                i.title  = e->getAllSubText().trim();
                if (i.title.isEmpty()) i.title = e->getStringAttribute ("title");
                if (i.number > 0) r.inputs.push_back (i);
            }
        }

        r.ok = ! r.inputs.empty();
        if (! r.ok) r.error = "respondeu, mas sem entradas";
        return r;
    }

    /** Como mostrar na lista: "2 - CAM 1", igual ao vMix. */
    static juce::String label (const Input& i)
    {
        return juce::String (i.number) + " - " + i.title;
    }
};
