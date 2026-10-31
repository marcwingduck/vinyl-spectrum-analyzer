#include "vinyl.h"

// Its label takes the album's color, with a darker print like a real one's: two lines of
// text above the spindle hole and a date below. It turns with the record, and the reflection
// on the grooves swings and slides a little as it goes round, like a slightly warped record.
static const Rgb vinylColor = {40, 36, 44}, rimLit = {96, 92, 104}, rimShadow = {64, 64, 64};

// the label's print, in its own coordinates: px from the center, y down, before turning
static bool printed(float x, float y)
{
    return (y >= -3 && y < -2 && fabsf(x) < 2)     // first line of text
           || (y >= -2 && y < -1 && fabsf(x) < 3)  // second line
           || (y >= 1 && y < 2 && fabsf(x) < 1);   // the date
}

void drawRecord(int x0, Rgb label, float turnDeg, float level)
{
    static float dist[RECORD_SIZE][RECORD_SIZE], angle[RECORD_SIZE][RECORD_SIZE];  // from the center
    static bool ready = false;
    if (!ready)
    {
        for (int y = 0; y < RECORD_SIZE; y++)
            for (int x = 0; x < RECORD_SIZE; x++)
            {
                float dx = x + 0.5f - RECORD_SIZE / 2, dy = y + 0.5f - RECORD_SIZE / 2;
                dist[y][x] = hypotf(dx, dy);
                angle[y][x] = atan2f(dy, dx) * RAD_TO_DEG;  // -90 is up, clockwise
            }
        ready = true;
    }

    const float r = RECORD_SIZE / 2.0f, phase = turnDeg * DEG_TO_RAD;
    const float sheen = -135 + 10 * sinf(phase), shift = 0.5f * cosf(phase);
    const Rgb print = scaled(label, 0.55f);
    const float turnCos = cosf(-phase), turnSin = sinf(-phase);  // back into the label's coordinates
    for (int y = 0; y < RECORD_SIZE; y++)
        for (int x = 0; x < RECORD_SIZE; x++)
        {
            float d = dist[y][x], a = angle[y][x];
            if (d > r || d < 1.0f) continue;  // outside, or the spindle hole
            Rgb c;
            if (d < 3.2f)
            {
                float dx = x + 0.5f - RECORD_SIZE / 2, dy = y + 0.5f - RECORD_SIZE / 2;
                c = printed(dx * turnCos - dy * turnSin, dx * turnSin + dy * turnCos) ? print : label;
            }
            else if (d > r - 1.05f)
                c = x + y < RECORD_SIZE - 2 ? rimLit : rimShadow;
            else if (d > 4.5f + shift && d < 5.5f + shift && fabsf(fmodf(a - sheen + 450, 180) - 90) < 30)
                c = rimLit;  // two arcs of light, opposite each other, as the grooves catch it on both sides
            else
                c = vinylColor;
            drawRgb(x0 + x, y, scaled(c, level));
        }
}
