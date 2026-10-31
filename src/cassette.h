#pragma once

// The boot animation: a cassette fades in, its tape winding from reel to reel, then
// light sweeps across it, it stays, and it slides out (or a track pushes it out).

#include <Arduino.h>

// one phase after the other; the reels turn and the tape winds throughout
#define BOOT_FADE_MS 1000                                      // the cassette fades in
#define BOOT_GLINT_MS 1000                                     // then light sweeps across it
#define BOOT_HOLD_MS 1000                                      // then it stays
#define BOOT_MS (BOOT_FADE_MS + BOOT_GLINT_MS + BOOT_HOLD_MS)  // then the tape slides out
#define REEL_STEP_MS 500
#define FADE_FROM 0.2f  // brightness the cassette fades in from, at 8 bits per channel

// the cassette elapsed ms into the boot, shifted down by dy rows
void drawCassette(uint32_t elapsed, int dy);
