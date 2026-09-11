#pragma once

/** Versao da mesa.

    Um lugar so. Toda fatia entregue sobe kBuild e troca kBuildName. O numero
    aparece na barra de status e no cabecalho do mesa.log — assim um print ou
    uma linha de log dizem exatamente que codigo estava rodando, sem precisar
    conferir arquivo por arquivo. */
namespace mesa {

inline constexpr const char* kVersion   = "1.7.24";
inline constexpr const char* kBuildName = "NDI no ritmo certo e sem silencio de enchimento";
inline constexpr const char* kBuildDate = __DATE__;

} // namespace mesa
