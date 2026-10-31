#include "cassette.h"

#include "panel.h"

#define REEL_STEP_MS 500
#define FADE_FROM 0.2f  // brightness the cassette fades in from, at 8 bits per channel

// boot screen: '.' is off, letters index cassettePalette; lit from the top left
static const char* const cassette[LED_ROWS] = {
    ".....BBBBBBBBBBBBBBBBBBBBBB.....",
    "....BGAAAAAAAAAAAAAAAAAAAAGC....",
    "....BAADDDDDDDDDDDDDDDDDDAAC....",
    "....BADDDDDDDDDDDDDDDDDDDDAC....",
    "....BADDDDDDDDDDDDDDDDDDDDAC....",
    "....BADDDAAAAAAAAAAAAAADDDAC....",
    "....BADDDAFFAEEEEEEAFFADDDAC....",
    "....BADDDAFFAEEEEEEAFFADDDAC....",
    "....BADDDAAAAAAAAAAAAAADDDAC....",
    "....BADDDDDDDDDDDDDDDDDDDDAC....",
    "....BADDDDDDDDDDDDDDDDDDDDAC....",
    "....BAAAABBBBBBBBBBBBBBAAAAC....",
    "....BAAABAAAAAAAAAAAAAACAAAC....",
    "....BAAABAAA.AAAAAA.AAACAAAC....",
    "....BGABAA.AAAAAAAAAA.AACAGC....",
    ".....CCBCCCCCCCCCCCCCCCCCCC.....",
};
static const Rgb cassettePalette[] = {
    {128, 128, 128},  // A shell
    {255, 255, 255},  // B shell, lit edge
    {64, 64, 64},     // C shell, shadow edge
    {229, 181, 255},  // D label
    {44, 40, 36},     // E window, smoked plastic
    {255, 255, 255},  // F reel hubs
    {32, 32, 32},     // G screws
};
static const int cassetteColorCount = sizeof(cassettePalette) / sizeof(Rgb);
static const float glintShine[] = {0.0f, 0.45f, 0.85f};  // blend toward white: none, edge, core
static const int reelX[] = {10, 20}, reelY = 6;          // top left of the 2x2 reel hubs
static const int loadX = 13, loadW = 6;                  // the D window between the hubs
static const Rgb tapeColor = {180, 145, 72};             // amber; LEDs push warm tones red
// full 8 bits per channel: through rgb()'s 5 bits the fade crept up in visible steps
static Rgb cassetteColor(Rgb color, float level, float shine)
{
    return scaled(mix(color, {255, 255, 255}, shine), level);
}

// how much of pixel (x, y) a tape pack of radius r around (cx, cy) covers, 0..1;
// the partly covered edge pixel lets the pack grow smoothly
static float packCoverage(int x, int y, float cx, float cy, float r)
{
    const int n = 4;
    int inside = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
        {
            float px = x + (i + 0.5f) / n - cx, py = y + (j + 0.5f) / n - cy;
            if (px * px + py * py <= r * r) inside++;
        }
    return (float)inside / (n * n);
}

// draws the cassette shifted down by dy rows, its reels turning counterclockwise
// as they do when the tape runs left to right past the head
void drawCassette(uint32_t elapsed, int dy)
{
    // fades in smoothly, scaled before the library's curve so it looks even; the reels
    // turn in steps
    int step = elapsed / REEL_STEP_MS;
    float level = FADE_FROM + (1.0f - FADE_FROM) * min(elapsed, (uint32_t)BOOT_FADE_MS) / BOOT_FADE_MS;
    Rgb colors[3][cassetteColorCount];
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < cassetteColorCount; i++)
            colors[s][i] = cassetteColor(cassettePalette[i], level, glintShine[s]);

    // tape winds from the left reel to the right; its length is the packs' area,
    // so one radius shrinks as the other grows. Empty, a pack just reaches the window
    // a full pack fills 3.5 of the window's 6 px: the most that still leaves one
    // column black between the packs halfway, when both are partly lit
    const float hubY = reelY + 1.0f, rMin = loadX - (reelX[0] + 1.0f), rMax = rMin + 3.5f;
    float wound = min(elapsed, (uint32_t)BOOT_MS) / (float)BOOT_MS;
    float radius[2] = {sqrtf(rMax * rMax - (rMax * rMax - rMin * rMin) * wound),
                       sqrtf(rMin * rMin + (rMax * rMax - rMin * rMin) * wound)};

    // once loaded, a "/" band of light runs across, measured along 2x + y
    int glint = -100;
    if (elapsed >= BOOT_FADE_MS && elapsed - BOOT_FADE_MS < BOOT_GLINT_MS)
    {
        const int from = 2 * 4 - 4, to = 2 * 27 + LED_ROWS - 1 + 4;  // just off the shell
        glint = from + (to - from) * (int)(elapsed - BOOT_FADE_MS) / BOOT_GLINT_MS;
    }

    for (int y = 0; y < LED_ROWS; y++)
        for (int x = 0; x < LED_COLS; x++)
        {
            char c = cassette[y][x];
            if (c == '.') continue;
            int d = abs(2 * x + y - glint), shine = d < 2 ? 2 : d < 4 ? 1
                                                                      : 0;
            if (c == 'E')
            {
                float cover = 0.0f;
                for (int r = 0; r < 2; r++) cover = max(cover, packCoverage(x, y, reelX[r] + 1.0f, hubY, radius[r]));
                if (cover > 0.15f)
                {
                    // the pack's edge pixel blends into the window by how much it covers
                    Rgb window = mix(cassettePalette['E' - 'A'], tapeColor, cover);
                    drawRgb(x, y + dy, cassetteColor(window, level, glintShine[shine]));
                    continue;
                }
            }
            drawRgb(x, y + dy, colors[c == 'G' ? 0 : shine][c - 'A']);
        }

    // one dark pixel circling each hub reads as rotation: top left, bottom left, ...
    static const uint8_t dx[] = {0, 0, 1, 1}, dyHub[] = {0, 1, 1, 0};
    int phase = step % 4;
    for (int x : reelX) drawRgb(x + dx[phase], reelY + dyHub[phase] + dy, {0, 0, 0});
}
