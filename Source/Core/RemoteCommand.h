#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

namespace mesa {

/** Comando vindo de fora (cartucheira, playout, automacao).

    Formato de texto, uma linha por comando, sem cerimonia — quem manda e
    software de terceiro e precisa ser trivial de gerar:

        CH1 ON              liga o canal 1
        CH1 PLAY            o mesmo, no vocabulario da cartucheira
        CH1 OFF             desliga
        CH1 PAUSE           fecha lembrando onde o fader estava
        CH1 FADER -6        poe o fader em -6 dB
        MIC 1 APRES ON      liga o canal que carrega essa fonte, pelo nome
        CH3 CUE ON          liga o CUE

    Comandos globais, sem canal:

        AUTOMACAO OFF       para a automacao de cameras (VT no ar)
        AUTOMACAO ON        volta
        AUTOMACAO HOLD 30   suspende por 30 segundos e volta sozinha
        CH2 MUTE OFF

    O nome pode ter espacos: a ACAO e sempre a ultima palavra (ou as duas
    ultimas, quando ha valor). Por isso o parser le de tras para frente. */
struct RemoteCommand
{
    enum class Action { None, On, Off, Pause, Fader, Cue, Mute, Trim,
                        AutomationOn, AutomationOff, AutomationHold };

    Action action = Action::None;
    int    channel = -1;        // indice 0-based; -1 quando veio por nome
    std::string name;           // nome do canal, quando nao veio "CHn"
    float  value = 0.0f;        // para FADER e TRIM
    bool   flag = false;        // para CUE/MUTE ON|OFF

    bool valid() const noexcept { return action != Action::None; }
};

inline std::string rcUpper (std::string s)
{
    for (auto& ch : s) ch = static_cast<char> (std::toupper (static_cast<unsigned char> (ch)));
    return s;
}

inline std::vector<std::string> rcSplit (const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : line)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (! cur.empty()) { out.push_back (cur); cur.clear(); }
        }
        else cur += c;
    }
    if (! cur.empty()) out.push_back (cur);
    return out;
}

inline RemoteCommand parseRemoteCommand (const std::string& line)
{
    RemoteCommand c;
    auto tok = rcSplit (line);
    if (tok.size() < 2) return c;

    // globais: nao tem canal. O VT precisa poder calar a automacao sem saber
    // nada sobre a estrutura de canais da mesa.
    {
        const std::string first = rcUpper (tok[0]);
        if (first == "AUTOMACAO" || first == "AUTOMATION" || first == "AUTO")
        {
            const std::string what = rcUpper (tok[1]);
            if (what == "ON")   { c.action = RemoteCommand::Action::AutomationOn;  return c; }
            if (what == "OFF")  { c.action = RemoteCommand::Action::AutomationOff; return c; }
            if (what == "HOLD" && tok.size() >= 3)
            {
                c.action = RemoteCommand::Action::AutomationHold;
                c.value = float (std::atof (tok[2].c_str()));
                return c;
            }
        }
    }

    // le de tras para frente: o alvo pode ter espacos no nome
    int consumed = 0;
    const std::string last = rcUpper (tok.back());

    if (last == "PAUSE")
    {
        c.action = RemoteCommand::Action::Pause;
        consumed = 1;
    }
    else if (last == "ON" || last == "OFF" || last == "PLAY" || last == "STOP")
    {
        const bool on = (last == "ON" || last == "PLAY");
        consumed = 1;

        if (tok.size() >= 3)
        {
            const std::string prev = rcUpper (tok[tok.size() - 2]);
            if (prev == "CUE")  { c.action = RemoteCommand::Action::Cue;  c.flag = on; consumed = 2; }
            if (prev == "MUTE") { c.action = RemoteCommand::Action::Mute; c.flag = on; consumed = 2; }
        }
        if (c.action == RemoteCommand::Action::None)
            c.action = on ? RemoteCommand::Action::On : RemoteCommand::Action::Off;
    }
    else if (tok.size() >= 3)
    {
        const std::string prev = rcUpper (tok[tok.size() - 2]);
        if (prev == "FADER") { c.action = RemoteCommand::Action::Fader; consumed = 2; }
        else if (prev == "TRIM") { c.action = RemoteCommand::Action::Trim; consumed = 2; }
        if (c.action != RemoteCommand::Action::None)
            c.value = float (std::atof (tok.back().c_str()));
    }

    if (c.action == RemoteCommand::Action::None) return c;

    const size_t targetCount = tok.size() - size_t (consumed);
    if (targetCount == 0) { c.action = RemoteCommand::Action::None; return c; }

    // alvo: "CH<n>" ou o nome do canal
    if (targetCount == 1)
    {
        const std::string t = rcUpper (tok[0]);
        if (t.size() > 2 && t[0] == 'C' && t[1] == 'H')
        {
            const int n = std::atoi (t.c_str() + 2);
            if (n >= 1) { c.channel = n - 1; return c; }
        }
    }

    for (size_t i = 0; i < targetCount; ++i)
    {
        if (! c.name.empty()) c.name += " ";
        c.name += tok[i];
    }
    return c;
}

} // namespace mesa
