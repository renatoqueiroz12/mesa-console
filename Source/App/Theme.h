#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/** Paleta e desenho da superficie. Os valores vem direto das variaveis CSS do
    mockup — se um dia o mockup mudar de cor, muda aqui e a mesa inteira segue. */
namespace theme
{
    // ---------------------------------------------------------------- chassi
    // O corpo da mesa e cinza claro de proposito: e o contraste dele com o
    // fader (que continua quase preto) que faz o curso do fader ser lido de
    // relance. Para clarear ou escurecer a mesa inteira, mexa SO nestes seis.
    inline const juce::Colour bg          { 0xff202329 };   // fora do chassi
    inline const juce::Colour chassisTop  { 0xff4a515b };   // topo do chassi
    inline const juce::Colour chassisBot  { 0xff32373f };   // base do chassi
    inline const juce::Colour surfaceHi   { 0xff4e555f };   // topo da tira
    inline const juce::Colour surface     { 0xff3f454e };   // meio da tira
    inline const juce::Colour surfaceLo   { 0xff343a42 };   // base da tira

    // ---------------------------------------------------------------- fenda
    // Preto de verdade: fader, rasgo e campo de leitura. Nao acompanham o
    // clareamento acima — e justamente a diferenca que da a leitura.
    inline const juce::Colour faderWell   { 0xff141619 };
    inline const juce::Colour slot        { 0xff08090b };

    inline const juce::Colour edge      { 0xff191c20 };
    inline const juce::Colour hair      { 0xff5a626c };
    inline const juce::Colour text      { 0xffe4e9ee };
    inline const juce::Colour textDim   { 0xffa8b0ba };
    inline const juce::Colour oledBg    { 0xff04070a };
    inline const juce::Colour oled      { 0xff8fe3ff };
    inline const juce::Colour oledDim   { 0xff2b6d85 };
    inline const juce::Colour onRed     { 0xffff3b30 };
    inline const juce::Colour prev      { 0xffffb020 };
    inline const juce::Colour busGreen  { 0xff54d07f };
    inline const juce::Colour trig      { 0xffff2d78 };
    inline const juce::Colour wide      { 0xff5b8cff };

    // ---------------------------------------------------------------- botoes
    // Sobem um degrau acima da tira para nao sumirem no cinza claro.
    inline const juce::Colour btnTop     { 0xff626a76 };
    inline const juce::Colour btnBot     { 0xff474e58 };
    inline const juce::Colour btnTopDown { 0xff6d7683 };
    inline const juce::Colour btnBotDown { 0xff525a65 };
    inline const juce::Colour btnText    { 0xffdfe5ec };

    // Cap do fader: claro sobre a fenda preta.
    inline const juce::Colour capTop     { 0xffb9c1cb };
    inline const juce::Colour capBot     { 0xff6f7883 };

    inline juce::Font mono (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }
    inline juce::Font sans (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    /** Painel de OLED: fundo quase preto com brilho interno azulado. */
    inline void drawOled (juce::Graphics& g, juce::Rectangle<int> r, float corner = 4.0f)
    {
        auto f = r.toFloat();
        g.setColour (oledBg);          g.fillRoundedRectangle (f, corner);
        g.setColour (juce::Colours::black); g.drawRoundedRectangle (f, corner, 1.0f);
        g.setColour (juce::Colour (0x141e8cb4));
        g.drawRoundedRectangle (f.reduced (1.5f), corner, 3.0f);
    }

    /** Superficie com bisel: claro em cima, escuro embaixo, fio de luz no topo. */
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> r,
                           juce::Colour top, juce::Colour bottom, float corner = 6.0f)
    {
        auto f = r.toFloat();
        g.setGradientFill (juce::ColourGradient (top, f.getX(), f.getY(),
                                                 bottom, f.getX(), f.getBottom(), false));
        g.fillRoundedRectangle (f, corner);
        g.setColour (edge);
        g.drawRoundedRectangle (f, corner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawLine (f.getX() + corner, f.getY() + 1.0f, f.getRight() - corner, f.getY() + 1.0f, 1.0f);
    }

    /** Barra de nivel horizontal com a mesma rampa de cor do mockup. */
    inline void drawBar (juce::Graphics& g, juce::Rectangle<float> r, float norm)
    {
        g.setColour (juce::Colour (0xff0a1116));
        g.fillRect (r);
        if (norm > 0.001f)
        {
            auto fill = r.withWidth (r.getWidth() * juce::jlimit (0.0f, 1.0f, norm));
            juce::ColourGradient grad (juce::Colour (0xff1f8fb0), r.getX(), 0.0f,
                                       juce::Colour (0xffff3b30), r.getRight(), 0.0f, false);
            grad.addColour (0.72, oled);
            grad.addColour (0.88, prev);
            g.setGradientFill (grad);
            g.fillRect (fill);
        }
        // ripado: a serigrafia de segmentos do OLED
        g.setColour (oledBg);
        for (float x = r.getX() + 3.0f; x < r.getRight(); x += 5.0f)
            g.fillRect (x, r.getY(), 2.0f, r.getHeight());
    }

    /** dBFS -> 0..1 na escala de -60 a 0. */
    inline float dbToNorm (float db) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    /** Posicao do fader (0..1) -> dB. Topo = +10, fundo = -infinito. */
    inline float faderPosToDb (float pos) noexcept
    {
        return pos <= 0.0f ? -100.0f : -60.0f + pos * 70.0f;
    }
    inline float faderDbToPos (float db) noexcept
    {
        return db <= -60.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 70.0f);
    }
    inline juce::String fmtDb (float db)
    {
        if (db <= -59.5f) return juce::String::fromUTF8 ("-\xe2\x88\x9e");
        return (db > 0.0f ? "+" : "") + juce::String (db, 1);
    }
}
