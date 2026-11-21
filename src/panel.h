#pragma once

// The LED panel and what everything draws with, shared by main.cpp and the animations.

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#define LED_COLS 32
#define LED_ROWS 16

extern MatrixPanel_I2S_DMA matrix;  // set up in main.cpp

// Everything drawn pixel by pixel is 8 bits per channel, the library's own depth; only
// the text and filled rectangles (Adafruit GFX, U8g2) take RGB565, through rgb().
struct Rgb
{
    uint8_t r, g, b;
};

// RGB565 for the text and rectangles. It gives green one bit more than red and blue,
// so dark grays would light only the green LED; quantizing all three to 5 bits keeps
// darks neutral.
inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (r >> 3 << 11) | (g >> 3 << 6) | (b >> 3);
}
inline uint16_t rgb(Rgb c) { return rgb(c.r, c.g, c.b); }

inline void drawRgb(int x, int y, Rgb c) { matrix.drawPixelRGB888(x, y, c.r, c.g, c.b); }

// The library's fillRect counts its width down before checking it: at 0 it wraps around and
// fills the whole row (the first calibration step flashed a full progress bar), so empty
// rectangles are skipped here.
inline void fillRect(int x, int y, int w, int h, uint16_t color)
{
    if (w > 0 && h > 0) matrix.fillRect(x, y, w, h, color);
}

// double buffered: drawing goes to the hidden buffer, this swaps it in
inline void show() { matrix.flipDMABuffer(); }

inline Rgb scaled(Rgb c, float f)
{
    return {(uint8_t)lroundf(c.r * f), (uint8_t)lroundf(c.g * f), (uint8_t)lroundf(c.b * f)};
}

// from a at 0 to b at 1
inline Rgb mix(Rgb a, Rgb b, float t)
{
    return {(uint8_t)lroundf(a.r + (b.r - a.r) * t), (uint8_t)lroundf(a.g + (b.g - a.g) * t),
            (uint8_t)lroundf(a.b + (b.b - a.b) * t)};
}

// A "/" band of light sweeping left to right, as over the boot cassette, measured along 2x + y.
// How far a pixel blends toward white: none, the band's edge, its core.
static const float GLINT_SHINE[] = {0.0f, 0.45f, 0.85f};
#define GLINT_STEPS_PER_S 69  // the cassette's speed: across its shell in BOOT_GLINT_MS (1 s)
inline int glintLevel(int x, int y, int band)
{
    int d = abs(2 * x + y - band);
    return d < 2 ? 2 : d < 4 ? 1 : 0;
}
// where the band is t ms into its sweep over columns x0..x1, from just off their left to just
// off their right; far off the panel once it's through. Same speed whatever the width.
inline int glintBand(uint32_t t, int x0, int x1)
{
    const int from = 2 * x0 - 4, to = 2 * x1 + LED_ROWS - 1 + 4;
    // 64 bits: in 32, a picture up for 17 hours would wrap around and shine again
    int band = from + (int)((uint64_t)t * GLINT_STEPS_PER_S / 1000);
    return band <= to ? band : -100;
}
// Like light on a glossy surface: bright pixels catch it fully, dark ones less, black not at all.
// Blending everything toward white the same read as a white stripe wandering over dark photos.
#define GLINT_DARK 0.75f  // how much less dark pixels catch: 1 in proportion to their brightness, 0 like bright ones
#define GLINT_TINT 0.15f  // how much of a pixel's own color the light takes on: 0 is white; more and a
                          // bright pixel, already close to its own light, barely shows the band
// c with the band's light on it
inline Rgb glinted(Rgb c, int x, int y, int band)
{
    int level = glintLevel(x, y, band);
    uint8_t top = max(c.r, max(c.g, c.b));
    if (!level || !top) return c;
    Rgb light = mix({255, 255, 255}, scaled(c, 255.0f / top), GLINT_TINT);  // its color at full brightness
    return mix(c, light, GLINT_SHINE[level] * powf(top / 255.0f, GLINT_DARK));
}

// 0..1 -> 0..1, fast start, settles gently
inline float easeOut(float p)
{
    p = 1.0f - constrain(p, 0.0f, 1.0f);
    return 1.0f - p * p;
}

// 0..1 -> 0..1, starts gently, speeds up, settles gently
inline float easeInOut(float p)
{
    p = constrain(p, 0.0f, 1.0f);
    return p * p * (3.0f - 2.0f * p);
}
