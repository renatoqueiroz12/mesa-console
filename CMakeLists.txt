cmake_minimum_required(VERSION 3.22)
project(MesaConsole VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- testes do core
# O core nao depende de JUCE: compila e roda em qualquer plataforma.
add_executable(test_core tests/test_core.cpp)
enable_testing()
add_test(NAME core COMMAND test_core)

# ---------------------------------------------------------------- app com JUCE
option(BUILD_APP "Compila o aplicativo com JUCE/ASIO" ON)
if(BUILD_APP)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    juce_add_gui_app(MesaConsole
        PRODUCT_NAME "Mesa Console"
        COMPANY_NAME "Metropolitana")

    target_sources(MesaConsole PRIVATE Source/App/Main.cpp)

    target_compile_definitions(MesaConsole PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MesaConsole,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MesaConsole,JUCE_VERSION>")

    # ASIO: so no Windows e so com o SDK da Steinberg no disco.
    # cmake -DASIO_SDK_DIR="C:/SDKs/asiosdk" ...
    if(WIN32 AND ASIO_SDK_DIR)
        message(STATUS "ASIO habilitado: ${ASIO_SDK_DIR}")
        target_compile_definitions(MesaConsole PRIVATE JUCE_ASIO=1)
        target_include_directories(MesaConsole PRIVATE "${ASIO_SDK_DIR}/common")
    else()
        message(WARNING "Sem ASIO_SDK_DIR: o app abrira apenas WASAPI/DirectSound/CoreAudio")
    endif()

    # NDI: so precisa dos HEADERS para compilar. A biblioteca e carregada em
    # tempo de execucao pelo NDI Runtime, entao nao se linka nada aqui.
    # cmake -DNDI_SDK_DIR="C:/Program Files/NDI/NDI 6 SDK" ...
    if(NDI_SDK_DIR)
        message(STATUS "NDI habilitado: ${NDI_SDK_DIR}")
        target_compile_definitions(MesaConsole PRIVATE MESA_HAS_NDI=1)
        target_include_directories(MesaConsole PRIVATE "${NDI_SDK_DIR}/Include")
    else()
        message(STATUS "Sem NDI_SDK_DIR: a camada NDI compila como casca vazia")
    endif()

    target_link_libraries(MesaConsole PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()
