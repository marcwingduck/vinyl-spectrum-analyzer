#include "vinyl.h"

// Its label takes the album's color, with a darker print: a band along its upper edge and a
// mark at the bottom. It turns with the record, and the reflection on the grooves swings and
// drifts a little as it goes round, like a slightly warped record.
static const Rgb vinylColor = {40, 36, 44}, rimLit = {96, 92, 104}, rimShadow = {64, 64, 64};
static const Rgb sheenColor = {120, 116, 130};  // the reflection on the grooves, a little brighter than the rim

// the label's print, in its own coordinates: px from the center, y down, before turning
static bool printed(float x, float y)
{
    return y < -2                       // a band at the top
           || (y > 2 && fabsf(x) < 1);  // a mark at the bottom
}

// whether a pixel d px from the center, apart degrees from the first arc's middle, is lit:
// the first arc's grooves lie shift px further out, the opposite arc's shift px further in
static bool inSheen(float d, float apart, float shift)
{
    if (apart < 30) return d > 4.5f + shift && d < 5.5f + shift;
    if (apart > 150) return d > 4.5f - shift && d < 5.5f - shift;
    return false;
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
    // Two arcs of light lie on the grooves at the top left and bottom right, as the grooves catch
    // it on both sides. On a slightly warped record they swing a little and drift as it turns:
    // one arc onto outer grooves while the other moves onto inner ones, both following the
    // grooves around the center.
    const float sheen = -135 + 6 * sinf(phase), shift = 0.5f * cosf(phase);
    const Rgb print = scaled(label, 0.55f);
    const float turnCos = cosf(-phase), turnSin = sinf(-phase);  // back into the label's coordinates
    for (int y = 0; y < RECORD_SIZE; y++)
        for (int x = 0; x < RECORD_SIZE; x++)
        {
            float d = dist[y][x], a = angle[y][x];
            if (d > r) continue;
            Rgb c;
            if (d < 1.0f)
                c = {0, 0, 0};  // the spindle hole, dark rather than showing what's behind
            else if (d < 3.2f)
            {
                float dx = x + 0.5f - RECORD_SIZE / 2, dy = y + 0.5f - RECORD_SIZE / 2;
                c = printed(dx * turnCos - dy * turnSin, dx * turnSin + dy * turnCos) ? print : label;
            }
            else if (d > r - 1.05f)
                c = x + y < RECORD_SIZE - 2 ? rimLit : rimShadow;
            else if (inSheen(d, fabsf(fmodf(a - sheen + 540, 360) - 180), shift))
                c = sheenColor;
            else
                c = vinylColor;
            drawRgb(x0 + x, y, scaled(c, level));
        }
}
