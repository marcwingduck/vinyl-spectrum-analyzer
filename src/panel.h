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
