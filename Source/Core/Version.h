#pragma once

/** Versao da mesa.

    Um lugar so. Toda fatia entregue sobe kBuild e troca kBuildName. O numero
    aparece na barra de status e no cabecalho do mesa.log — assim um print ou
    uma linha de log dizem exatamente que codigo estava rodando, sem precisar
    conferir arquivo por arquivo. */
namespace mesa {

inline constexpr const char* kVersion   = "1.6.19";
inline constexpr const char* kBuildName = "permanencia no corte manual e camera padrao";
inline constexpr const char* kBuildDate = __DATE__;

} // namespace mesa
