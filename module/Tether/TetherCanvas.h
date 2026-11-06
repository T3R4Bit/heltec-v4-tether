// 128x64 1-bit framebuffer with blocky text, in XBM layout so the firmware can blit it with one drawXbm() call.
// Pure C++ (no Arduino/Meshtastic headers): the same drawing code renders the PNG previews on a PC.
#pragma once

#include <stdint.h>
#include <string.h>

#include "TetherFont.h"

namespace tether
{

class Canvas
{
  public:
    static constexpr int W = 128;
    static constexpr int H = 64;
    static constexpr int STRIDE = W / 8;

    /// XBM: row-major, STRIDE bytes per row, least significant bit = leftmost pixel.
    uint8_t buf[STRIDE * H];

    Canvas() { clear(); }

    void clear() { memset(buf, 0, sizeof(buf)); }

    void set(int x, int y, bool on = true)
    {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return;
        uint8_t &b = buf[y * STRIDE + (x >> 3)];
        const uint8_t m = (uint8_t)(1u << (x & 7));
        b = on ? (uint8_t)(b | m) : (uint8_t)(b & ~m);
    }

    bool get(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return false;
        return buf[y * STRIDE + (x >> 3)] & (1u << (x & 7));
    }

    void fillRect(int x, int y, int w, int h, bool on = true)
    {
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++)
                set(i, j, on);
    }

    /// Outline with a border t pixels thick.
    void rect(int x, int y, int w, int h, int t = 1, bool on = true)
    {
        fillRect(x, y, w, t, on);
        fillRect(x, y + h - t, w, t, on);
        fillRect(x, y, t, h, on);
        fillRect(x + w - t, y, t, h, on);
    }

    void hline(int x, int y, int w, bool on = true) { fillRect(x, y, w, 1, on); }

    /// Every `step`-th pixel lit, for reference lines that must not hide bars behind them.
    void dottedHline(int x, int y, int w, int step = 3, bool on = true)
    {
        for (int i = x; i < x + w; i += step)
            set(i, y, on);
    }

    static const Glyph &glyph(char c)
    {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c < FONT_FIRST || c > FONT_LAST || FONT[c - FONT_FIRST].width == 0)
            c = '?';
        return FONT[c - FONT_FIRST];
    }

    /// Pixel width of s at the given scale, without the trailing gap.
    static int textWidth(const char *s, int scale = 1)
    {
        int w = 0;
        for (; *s; s++)
            w += (glyph(*s).width + 1) * scale;
        return w ? w - scale : 0;
    }

    static int textHeight(int scale = 1) { return FONT_HEIGHT * scale; }

    /// Draws c with its top-left at (x, y); returns the x where the next character starts.
    int drawChar(int x, int y, char c, int scale = 1, bool on = true)
    {
        const Glyph &g = glyph(c);
        for (int row = 0; row < FONT_HEIGHT; row++)
            for (int col = 0; col < g.width; col++)
                if (g.rows[row] & (1u << (g.width - 1 - col)))
                    fillRect(x + col * scale, y + row * scale, scale, scale, on);
        return x + (g.width + 1) * scale;
    }

    int text(int x, int y, const char *s, int scale = 1, bool on = true)
    {
        for (; *s; s++)
            x = drawChar(x, y, *s, scale, on);
        return x;
    }

    int textRight(int right, int y, const char *s, int scale = 1, bool on = true)
    {
        return text(right - textWidth(s, scale), y, s, scale, on);
    }

    int textCentered(int cx, int y, const char *s, int scale = 1, bool on = true)
    {
        return text(cx - textWidth(s, scale) / 2, y, s, scale, on);
    }
};

} // namespace tether
