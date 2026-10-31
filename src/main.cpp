#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Arduino.h>
#include <Preferences.h>
#include <arduinoFFT.h>
#include <driver/adc.h>

// Optional: with src/secrets.h (WiFi credentials) the board also shows what
// snapserver-lcd is streaming while the turntable is silent. Without it, this
// is a plain spectrum analyzer.
#if __has_include("secrets.h")
#define NOW_PLAYING
#include <ESPmDNS.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "secrets.h"
#endif

// Uncomment to broadcast stray peaks while the turntable is silent (needs secrets.h).
// Off, it costs nothing; see "Stray peaks while idle" in the README.
#define IDLE_LOG

#define LED_COLS 32
#define LED_ROWS 16

// A3 = GPIO10 = ADC1 channel 9. A0 and A4 sit on ADC2, which stops working once WiFi runs.
#define AUDIO_CHANNEL 9

#define FFT_N 512        // 256 bins of ~39 Hz
#define SAMPLE_HZ 20000  // 10 kHz span
#define OVERSAMPLE 4     // ADC samples averaged per FFT sample: ~6 dB less noise; ADC tops out at 83 kHz
#define BIN_WIDTH 2      // columns sharing one band, as the sketch booted on AVR
#define BANDS (LED_COLS / BIN_WIDTH)
#define BAND_LOW_HZ 40  // bands are spaced logarithmically from here up to SAMPLE_HZ / 2

#define PEAK_FALLOFF_MS 500  // how long a peak marker hangs before sinking
#define NOISE_MARGIN_DB 6    // bars start this far above each band's measured noise
#define CAL_HOLD_MS 2000     // hold the UP button this long to calibrate, with the needle up
#define CAL_MS 15000         // calibration measures this long, keeping each band's loudest second
// Keep it at 255. The DMA library dims by pulsing the LEDs off within every row, and those
// current pulses reach the ADC: at 180 the Spotify view added 9-15 dB to the bass and mid
// bands, the board took that for vinyl and swapped to the bars every few seconds.
#define BRIGHTNESS 255  // 0-255
#define DISPLAY_DB 20        // range the bars show, from the top of the scale down
// The top of the scale jumps to any louder band at once and sinks this fast after it,
// so the loudest moment just reaches the top and nothing is cut off there (an average
// as the top did that to every peak above it, and the bass stuck to the ceiling)
#define SCALE_DECAY_DB_S 6
#define MM_SIZE 32           // frames averaged for silence detection
#define ATTACK_MS 25         // how fast bars rise to a new level
// boot animation, one phase after the other; the reels turn and the tape winds throughout
#define BOOT_FADE_MS 3000                                      // the cassette fades in
#define BOOT_GLINT_MS 1000                                     // then light sweeps across it
#define BOOT_HOLD_MS 1000                                      // then it stays
#define BOOT_MS (BOOT_FADE_MS + BOOT_GLINT_MS + BOOT_HOLD_MS)  // then the tape slides out
#define REEL_STEP_MS 500
#define SLIDE_MS 500    // pushes run this long: new album down, new title left, tape out
#define FADE_FROM 0.5f  // fade start; much lower and colors tint, 5 bits per channel
#define GAMMA 2.2f      // LEDs are linear, images are sRGB
#define RELEASE_MS 140  // how slowly they sink back; raise for calmer bars

// MatrixPortal S3 pins (as in Adafruit's Protomatter examples):
// R1 G1 B1 R2 G2 B2, A B C (D and E unused on a 16-row panel), LAT OE CLK
static const HUB75_I2S_CFG::i2s_pins matrixPins = {42, 41, 40, 38, 39, 37, 45, 36, 48, -1, -1, 47, 14, 2};

// The LCD peripheral refreshes the panel by DMA, without the CPU; Protomatter's
// timer-driven refresh flickered whenever WiFi sent a clip (not the power supply:
// a lower transmit power made no difference). Double buffered: drawing goes
// to the hidden buffer and show() swaps, so every frame is drawn in full.
static HUB75_I2S_CFG matrixConfig()
{
    HUB75_I2S_CFG cfg(LED_COLS, LED_ROWS, 1, matrixPins);
    cfg.double_buff = true;
    cfg.clkphase = false;  // this panel latches on the other clock edge; true shifts everything 1 px left
    return cfg;
}
MatrixPanel_I2S_DMA matrix(matrixConfig());

static void show() { matrix.flipDMABuffer(); }

float vReal[FFT_N], vImag[FFT_N];
ArduinoFFT<float> FFT(vReal, vImag, FFT_N, (float)SAMPLE_HZ);

float bands[BANDS];
#ifdef IDLE_LOG
float bandsRaw[BANDS];  // before despiking, for the UDP log
#endif
float bandHistory[2][BANDS];   // the two frames before, for despiking
uint16_t bandEdge[BANDS + 1];  // first FFT bin of each band, plus the end of the last
// Each band's level with the needle up, in dB. Measured, because the noise isn't even:
// mains hum puts 50 Hz and its harmonics well above the rest. Holding UP recalibrates
// and stores the result in flash; these are the defaults until then (measured with an
// earlier band layout, so only rough: calibrate once after flashing).
static const int8_t bandNoiseDb[BANDS] = {-34, -40, -43, -44, -45, -43, -39, -41,
                                          -37, -36, -34, -34, -32, -32, -30, -29};
float bandNoise[BANDS];     // where each band's bars start: its noise plus NOISE_MARGIN_DB
char noiseStatus[160];      // where bandNoise came from, for serial and the UDP log
bool noiseStatusNew;        // not yet sent over UDP
float bandLevelSum[BANDS];  // summed for the print
uint32_t bandLevelFrames;
uint32_t samplesUsed;  // FFT samples taken since the last print, to check SAMPLE_HZ is met

// one frame of DMA results, 4 bytes each
uint8_t adcBuf[FFT_N * OVERSAMPLE * SOC_ADC_DIGI_RESULT_BYTES];

#ifdef NOW_PLAYING
// Song recognition: whoever connects to CLIP_PORT gets the next CLIP_S seconds of
// audio as raw 16-bit mono at SAMPLE_HZ. Recorded first and sent in one burst, so
// WiFi disturbs the ADC once instead of every frame.
#define CLIP_PORT 4212
#define CLIP_ASK_PORT 4213  // broadcast "clip <reason>" when a clip is worth fetching
#define CLIP_S 10
#define CLIP_SAMPLES (CLIP_S * SAMPLE_HZ)
int16_t* clip;                     // 400 kB, in PSRAM
uint32_t clipPos = CLIP_SAMPLES;  // recording while below CLIP_SAMPLES
#endif
uint8_t peaks[LED_COLS];
uint32_t peakMillis[LED_COLS];
float barLevel[LED_COLS];
uint32_t lastDraw = 0;
uint32_t frames = 0;  // loop() runs, one per audio block

uint16_t colorOff, colorPeak, colorRow[LED_ROWS];
// a recognized record paints the bars in its cover's colors until the turntable stops
bool albumColors = false;
uint16_t albumPeak, albumRow[LED_ROWS];
// a new track's colors wait until its pop-in has pushed the bars off the panel
bool pendingColors = false, pendingAlbumColors;
uint16_t pendingPeak, pendingRow[LED_ROWS];

static void applyPendingColors()
{
    albumColors = pendingAlbumColors;
    albumPeak = pendingPeak;
    memcpy(albumRow, pendingRow, sizeof(albumRow));
    pendingColors = false;
}

struct MovingAverage
{
    float values[MM_SIZE] = {};
    float sum = 0.0f;
    int index = 0, count = 0;

    float add(float val)
    {
        sum += val - values[index];
        values[index] = val;
        index = (index + 1) % MM_SIZE;
        count = min(count + 1, MM_SIZE);
        return sum / count;
    }
};

MovingAverage signalAvg;
float peakDb = -1e9f;  // the top of the bar scale: the loudest band, sinking SCALE_DECAY_DB_S after it
float signalDb;  // recent loudest band relative to its noise floor; > 0 means audio

// RGB565 gives green one bit more than red and blue, so dark grays would light
// only the green LED. Quantizing all three to 5 bits keeps darks neutral.
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (r >> 3 << 11) | (g >> 3 << 6) | (b >> 3);
}

// boot screen: '.' is off, letters index cassettePalette; lit from the top left
static const char* const cassette[LED_ROWS] = {
    ".....EEEEEEEEEEEEEEEEEEEEEE.....",
    "....EGAAAAAAAAAAAAAAAAAAAAGF....",
    "....EAABBBBBBBBBBBBBBBBBBAAF....",
    "....EABBBBBBBBBBBBBBBBBBBBAF....",
    "....EABBBBBBBBBBBBBBBBBBBBAF....",
    "....EABBBAAAAAAAAAAAAAABBBAF....",
    "....EABBBACCADDDDDDACCABBBAF....",
    "....EABBBACCADDDDDDACCABBBAF....",
    "....EABBBAAAAAAAAAAAAAABBBAF....",
    "....EABBBBBBBBBBBBBBBBBBBBAF....",
    "....EABBBBBBBBBBBBBBBBBBBBAF....",
    "....EAAAAEEEEEEEEEEEEEEAAAAF....",
    "....EAAAEAAAAAAAAAAAAAAFAAAF....",
    "....EAAAEAAA.AAAAAA.AAAFAAAF....",
    "....EGAEAA.AAAAAAAAAA.AAFAGF....",
    ".....FFEFFFFFFFFFFFFFFFFFFF.....",
};
static const uint8_t cassettePalette[][3] = {
    {100, 100, 100},  // A shell
    {190, 70, 255},   // B label
    {255, 255, 255},  // C reel hubs
    {60, 40, 80},     // D window, smoked; the tape shows through
    {255, 255, 255},  // E shell, lit edge
    {64, 64, 64},     // F shell, shadow edge; LEDs lift darks, so go deep
    {16, 16, 16},     // G screw
};
static const int cassetteColorCount = sizeof(cassettePalette) / 3;
static const float glintShine[] = {0.0f, 0.45f, 0.85f};  // blend toward white: none, edge, core
static const int reelX[] = {10, 20}, reelY = 6;          // top left of the 2x2 reel hubs
static const int loadX = 13, loadW = 6;                  // the D window between the hubs
static const uint8_t tapeColor[3] = {180, 145, 72};      // amber; LEDs push warm tones red
bool cassetteShown = false;                              // drawn last frame, so a track can push it out
uint32_t bootStart;                                      // end of setup(), when the cassette appears

static uint32_t bootElapsed() { return millis() - bootStart; }
static bool booting() { return bootElapsed() < BOOT_MS; }
// with nothing to push it out, the tape slides down on its own
static bool ejecting() { return bootElapsed() < BOOT_MS + SLIDE_MS; }

// how far a push has come, 0..1 over SLIDE_MS; ease out: fast start, settles gently
static float slideProgress(uint32_t elapsed)
{
    float p = 1.0f - min(elapsed, (uint32_t)SLIDE_MS) / (float)SLIDE_MS;
    return 1.0f - p * p;
}

static uint16_t cassetteColor(const uint8_t color[3], float level, float shine)
{
    uint8_t c[3];
    for (int k = 0; k < 3; k++)
    {
        float v = color[k] + (255 - color[k]) * shine;
        // rgb() drops the low 3 bits; round to the nearest step instead, or dim
        // channels lose more than bright ones and faded colors shift hue (tape goes red)
        int q = min(lroundf(powf(v * level / 255.0f, GAMMA) * 255) + 4, 255L);
        // keep dim channels lit so the shadow edges don't pop in late and the
        // cassette doesn't seem to grow
        c[k] = v > 0 ? max(q, 8) : q;
    }
    return rgb(c[0], c[1], c[2]);
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
static void drawCassette(int dy)
{
    // fade in step by step with the reels; scaled before gamma, so it looks even
    uint32_t elapsed = bootElapsed();
    int step = elapsed / REEL_STEP_MS;
    const int fadeSteps = BOOT_FADE_MS / REEL_STEP_MS;
    float level = FADE_FROM + (1.0f - FADE_FROM) * min(step, fadeSteps) / fadeSteps;
    uint16_t colors[3][cassetteColorCount];
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
            if (c == 'D')
            {
                float cover = 0.0f;
                for (int r = 0; r < 2; r++) cover = max(cover, packCoverage(x, y, reelX[r] + 1.0f, hubY, radius[r]));
                if (cover > 0.15f)
                {
                    // the pack's edge pixel blends into the window by how much it covers
                    uint8_t mixed[3];
                    for (int k = 0; k < 3; k++)
                        mixed[k] = lroundf(cassettePalette['D' - 'A'][k] + (tapeColor[k] - cassettePalette['D' - 'A'][k]) * cover);
                    matrix.drawPixel(x, y + dy, cassetteColor(mixed, level, glintShine[shine]));
                    continue;
                }
            }
            matrix.drawPixel(x, y + dy, colors[c == 'G' ? 0 : shine][c - 'A']);
        }

    // one dark pixel circling each hub reads as rotation: top left, bottom left, ...
    static const uint8_t dx[] = {0, 0, 1, 1}, dyHub[] = {0, 1, 1, 0};
    int phase = step % 4;
    for (int x : reelX) matrix.drawPixel(x + dx[phase], reelY + dyHub[phase] + dy, colorOff);
}

// The ADC samples on its own timer at SAMPLE_HZ * OVERSAMPLE and DMA collects the
// results, so the timing is exact and sampling goes on while a frame is drawn.
static void beginAudio()
{
    adc_digi_init_config_t init = {};
    init.max_store_buf_size = sizeof(adcBuf) * 2;  // room for a frame of lag
    init.conv_num_each_intr = 256 * SOC_ADC_DIGI_RESULT_BYTES;
    init.adc1_chan_mask = BIT(AUDIO_CHANNEL);

    static adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;
    pattern.channel = AUDIO_CHANNEL;
    pattern.unit = 0;  // ADC1; Espressif's example encodes this in the channel, which breaks above 7
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    adc_digi_configuration_t config = {};
    config.pattern_num = 1;
    config.adc_pattern = &pattern;
    config.sample_freq_hz = SAMPLE_HZ * OVERSAMPLE;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

    ESP_ERROR_CHECK(adc_digi_initialize(&init));
    ESP_ERROR_CHECK(adc_digi_controller_configure(&config));
    ESP_ERROR_CHECK(adc_digi_start());
}

static void sampleAudio()
{
    int32_t sum = 0;
    int n = 0, k = 0;  // FFT samples filled, ADC samples in the current average
    uint32_t acc = 0;

    while (n < FFT_N)
    {
        // ask only for what is still missing, so no samples are thrown away
        uint32_t want = ((FFT_N - n) * OVERSAMPLE - k) * SOC_ADC_DIGI_RESULT_BYTES, len = 0;
        adc_digi_read_bytes(adcBuf, want, &len, ADC_MAX_DELAY);

        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= len && n < FFT_N; i += SOC_ADC_DIGI_RESULT_BYTES)
        {
            const adc_digi_output_data_t* d = (const adc_digi_output_data_t*)&adcBuf[i];
            if (d->type2.unit != 0 || d->type2.channel != AUDIO_CHANNEL) continue;

            acc += d->type2.data;
            if (++k < OVERSAMPLE) continue;

            vReal[n] = (float)acc / OVERSAMPLE;
            vImag[n] = 0.0f;
#ifdef NOW_PLAYING
            // 12 bits around mid-rail to full-scale 16 bits; the receiver filters the rest of the bias
            if (clipPos < CLIP_SAMPLES) clip[clipPos++] = ((int)(acc / OVERSAMPLE) - 2048) * 16;
#endif
            sum += acc / OVERSAMPLE;
            n++;
            k = 0;
            acc = 0;
        }
    }
    samplesUsed += FFT_N;

    // subtract the measured bias rather than trusting the divider to sit at mid-rail
    float mean = (float)sum / FFT_N;
    for (int i = 0; i < FFT_N; i++) vReal[i] = (vReal[i] - mean) / 8.0f;
}

static void runFFT()
{
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    float maxDb = -1e9f, maxSignal = -1e9f;

    for (int b = 0; b < BANDS; b++)
    {
        // summing power over the band means wide treble bands collect as much as narrow
        // bass ones, which offsets music's falling energy toward the top
        float power = 0.0f;
        for (int i = bandEdge[b]; i < bandEdge[b + 1]; i++)
        {
            float mag = vReal[i] / FFT_N;
            power += mag * mag;
        }

        // median of the last three frames: interference (the TV, a WiFi burst) lasts one
        // frame and would leave a stray peak marker, music lasts longer
        float db = 10.0f * log10f(max(power, 1e-12f));
#ifdef IDLE_LOG
        bandsRaw[b] = db;
#endif
        float a = bandHistory[0][b], c = bandHistory[1][b];
        bandHistory[0][b] = c;
        bandHistory[1][b] = db;
        bands[b] = max(min(a, c), min(max(a, c), db));

        maxDb = max(maxDb, bands[b]);
        maxSignal = max(maxSignal, bands[b] - bandNoise[b]);
        bandLevelSum[b] += bands[b];
    }
    bandLevelFrames++;

    // one frame lasts FFT_N / SAMPLE_HZ, ~26 ms
    peakDb = max(maxDb, peakDb - SCALE_DECAY_DB_S * FFT_N / (float)SAMPLE_HZ);
    signalDb = signalAvg.add(maxSignal);
}

// one color per row, bottom to top
static void fillGradient(uint16_t* rows, const uint8_t bottom[3], const uint8_t top[3])
{
    for (int j = 0; j < LED_ROWS; j++)
    {
        uint32_t t = (uint32_t)j * 255 / (LED_ROWS - 1);
        uint8_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = (bottom[k] * (255 - t) + top[k] * t) / 255;
        rows[j] = rgb(c[0], c[1], c[2]);
    }
}

// draws the bars shifted down by dy rows, without clearing or showing the panel;
// runs every frame, even pushed off the panel, so the bars keep moving
static void drawBars(int dy)
{
    // Bars span DISPLAY_DB below the top of the scale, but never reach into a band's
    // noise: quiet passages show whatever rises above it, silence shows nothing.
    const float scale = (float)LED_ROWS / DISPLAY_DB;
    const float top = peakDb;
    const uint16_t* rows = albumColors ? albumRow : colorRow;
    const uint16_t peak = albumColors ? albumPeak : colorPeak;

    // time-based so the feel doesn't depend on how fast loop() runs
    uint32_t now = millis();
    float dt = (float)(now - lastDraw);
    lastDraw = now;
    float attack = 1.0f - expf(-dt / ATTACK_MS);
    float release = 1.0f - expf(-dt / RELEASE_MS);

    for (int i = 0; i < LED_COLS; i++)
    {
        int b = i / BIN_WIDTH;
        float bottom = max(top - DISPLAY_DB, bandNoise[b]);
        float target = constrain((bands[b] - bottom) * scale, 0.0f, (float)LED_ROWS);
        barLevel[i] += (target - barLevel[i]) * (target > barLevel[i] ? attack : release);
        int val = (int)lroundf(barLevel[i]);

        if (val > peaks[i])
        {
            peaks[i] = val;
            peakMillis[i] = millis();
        }
        else if (peaks[i] > 0 && millis() - peakMillis[i] > PEAK_FALLOFF_MS)
        {
            peaks[i] -= 1;
        }

        for (int j = 0; j < val; j++)
            matrix.drawPixel(i, LED_ROWS - 1 - j + dy, rows[j]);

        if (peaks[i] > 0) matrix.drawPixel(i, LED_ROWS - peaks[i] + dy, peak);
    }
}

static void draw()
{
    matrix.fillScreen(colorOff);
    drawBars(0);
    show();
}

#ifdef NOW_PLAYING
// now playing, pushed by snapserver-lcd over UDP
#define HOSTNAME "vinyl-matrix"  // reachable as vinyl-matrix.local
#define NP_PORT 4210
#ifdef IDLE_LOG
#define LOG_PORT 4211  // idle spikes and a heartbeat, broadcast; see the README to listen
#define LOG_BEAT_MS 60000
#endif
#define NP_TIMEOUT_MS 30000  // sender repeats every 10 s; drop the view after this
#define HELLO_MS 2000        // until snapserver-lcd answers, ask it for the state this often
#define VINYL_HOLD_MS 3000   // silence needed before handing the panel to spotify
#define VINYL_DB 6           // above the bars' floor before it counts as vinyl; hum wanders
// While Spotify's cover and text light the panel, its current reaches the ADC: up to ~5 dB
// at full brightness, 9-15 dB dimmed. Vinyl must be louder than that to take over, so the
// panel can't swap back and forth on its own interference.
#define VINYL_OVER_SPOTIFY_DB 20
// A new track: the needle drops, or the level falls GAP_DB below the recent loud level
// for GAP_MS (the groove still crackles, so not down to the floor) and comes back.
// CLIP_DELAY_MS into the track the board asks for a clip; gapless sides every RECHECK_MS.
#define GAP_DB 15
#define GAP_MS 1000
#define CLIP_DELAY_MS 4000
#define RECHECK_MS 120000
#define LOUD_DECAY_DB_S 2.0f  // how fast the recent loud level sinks through quiet passages
#define POP_MS 8000           // a recognized record track shows this long before the bars return
#define POP_MAX_MS 15000      // long names stretch it until they've scrolled through once, up to this
#define ART_SIZE 16
#define TEXT_MAX 128
#define SCROLL_MS_PER_PX 70  // marquee speed, ~14 px/s
#define SCROLL_HOLD_MS 1500  // pause at the start of each marquee cycle
// The panel changes once per frame, one audio block plus ~1-2 ms of FFT and drawing.
// Stepping by time would move a pixel after 2 frames, then 3, which reads as judder,
// so the marquee steps every whole number of frames, rounded from the speed above.
#define FRAME_MS (1000.0f * FFT_N / SAMPLE_HZ)
static const uint32_t scrollFramesPerPx = max(1, (int)(SCROLL_MS_PER_PX / FRAME_MS + 0.5f));
static const uint32_t scrollHoldFrames = (uint32_t)(SCROLL_HOLD_MS / FRAME_MS + 0.5f);
#define SCROLL_GAP 8           // blank pixels between marquee repeats
#define TEXT_X (ART_SIZE + 1)  // one blank column between artwork and text
#define TITLE_BASELINE 15      // 3x5 font: caps on rows 10-14, descenders on the bottom row
#define ARTIST_BASELINE 9      // one font line (6 px) above: caps on rows 4-8

U8G2_FOR_ADAFRUIT_GFX text;

struct Track
{
    uint32_t artistSince, titleSince;  // marquee starts, in frames
    uint16_t art[ART_SIZE * ART_SIZE];
    uint16_t colorArtist;
    char artist[TEXT_MAX], title[TEXT_MAX];
    int artistWidth, titleWidth;  // in px
};

struct NowPlaying
{
    bool active = false;
    uint32_t lastPacket = 0;
    uint32_t slideSince = 0;
    bool visible = false;  // drawn last frame
    bool leaving = false;  // stopped: the track slides out at the bottom
    uint32_t leaveSince = 0;
    enum
    {
        FROM_BLANK,
        FROM_CASSETTE,
        FROM_TRACK,
        FROM_TITLE  // same album: only the title changes
    } slideFrom;
    Track track, previous;  // previous is only drawn while it slides out
} np;

// how far above the bars' floor the turntable must be to count as vinyl; higher while the
// Spotify view (or its slide out) lights the panel
static int vinylThreshold() { return np.visible || np.leaving ? VINYL_OVER_SPOTIFY_DB : VINYL_DB; }

// the record on the turntable, recognized by groove-id on tape
struct Record
{
    bool known = false;  // until the turntable goes silent
    uint32_t popSince = 0, popMs = 0;  // a new track slides in, stays, then slides out
    Track track;
} record;

WiFiUDP udp;
WiFiServer clipServer(CLIP_PORT);
WiFiClient clipClient;
volatile bool clipSending = false;  // sendClip() owns clipClient until it clears this
uint8_t packet[1200];
uint8_t artGamma[256];
uint32_t lastVinyl = -VINYL_HOLD_MS;  // no vinyl yet, so a track can take over right at boot
bool mdnsStarted = false;
bool heardFromPi = false;

// Fills t (all but the marquee starts) from a 'P' or 'V' packet of length len;
// extra bytes sit between the accent and the artist. False if it is malformed.
static bool readTrack(int len, int extra, Track& t)
{
    const int artBytes = ART_SIZE * ART_SIZE * 3;
    if (len < 4 + artBytes + 3 + extra + 2) return false;

    // both strings must be terminated inside the packet
    const uint8_t* p = packet + 4;
    const char* artist = (const char*)p + artBytes + 3 + extra;
    const char* end = (const char*)packet + len;
    const char* title = (const char*)memchr(artist, 0, end - artist);
    if (!title || !memchr(++title, 0, end - title)) return false;

    for (int i = 0; i < ART_SIZE * ART_SIZE; i++, p += 3)
        t.art[i] = rgb(artGamma[p[0]], artGamma[p[1]], artGamma[p[2]]);

    // the accent in the artwork's colors: gamma like the art, but brightened first so
    // its strongest channel is full, or dark accents would make the text unreadable
    uint8_t top = max(max(p[0], p[1]), max(p[2], (uint8_t)1));
    uint8_t c[3];
    for (int k = 0; k < 3; k++) c[k] = artGamma[p[k] * 255 / top];
    t.colorArtist = rgb(c[0], c[1], c[2]);
    strlcpy(t.artist, artist, TEXT_MAX);
    strlcpy(t.title, title, TEXT_MAX);
    t.artistWidth = text.getUTF8Width(t.artist);
    t.titleWidth = text.getUTF8Width(t.title);
    return true;
}

// Long names make the pop-in last until both have scrolled through once.
static uint32_t popMs(const Track& t)
{
    const int area = LED_COLS - TEXT_X;
    int widest = max(t.artistWidth, t.titleWidth);
    uint32_t scrollMs = 0;
    if (widest > area)
        scrollMs = (scrollHoldFrames + (widest + SCROLL_GAP) * scrollFramesPerPx) * FRAME_MS;
    return constrain(scrollMs, (uint32_t)POP_MS, (uint32_t)POP_MAX_MS);
}

// Packet: "NP" version cmd, then for cmd 'P' (spotify):
// 16x16 RGB888 art (row-major), accent RGB, artist\0, title\0 (UTF-8)
// cmd 'S' (or silence for NP_TIMEOUT_MS) ends the now playing view.
// cmd 'V' (the record on the turntable, from groove-id) is 'P' with the bars' bottom
// and top RGB after the accent, all zero to keep the bars' own colors. It comes with
// every recognition; only a new track pops in.
static void receiveNowPlaying()
{
    int len = udp.parsePacket();
    if (len <= 0) return;
    len = udp.read(packet, sizeof(packet));

    if (len < 4 || packet[0] != 'N' || packet[1] != 'P' || packet[2] != 4) return;

    if (packet[3] == 'V')
    {
        Track t;
        if (!readTrack(len, 6, t)) return;
        bool newTrack = !record.known || strncmp(t.artist, record.track.artist, TEXT_MAX) ||
                        strncmp(t.title, record.track.title, TEXT_MAX);
        if (newTrack)
        {
            t.artistSince = t.titleSince = frames;
            record.track = t;
            record.popSince = millis();
            record.popMs = popMs(t);
        }
        record.known = true;

        const uint8_t* bars = packet + 4 + ART_SIZE * ART_SIZE * 3 + 3;
        pendingAlbumColors = false;
        for (int k = 0; k < 6; k++) pendingAlbumColors |= bars[k] != 0;
        if (pendingAlbumColors)
        {
            uint8_t bottom[3], top[3];
            for (int k = 0; k < 3; k++)
            {
                bottom[k] = artGamma[bars[k]];
                top[k] = artGamma[bars[3 + k]];
            }
            fillGradient(pendingRow, bottom, top);
            pendingPeak = t.colorArtist;
        }
        // a recheck of the playing track changes nothing visible, so it applies at once
        pendingColors = true;
        if (!newTrack && millis() - record.popSince >= SLIDE_MS) applyPendingColors();
        return;
    }

    heardFromPi = true;  // snapserver-lcd; groove-id doesn't answer the hello
    if (packet[3] == 'S')
    {
        np.active = false;
        return;
    }
    Track t;
    if (packet[3] != 'P' || !readTrack(len, 0, t)) return;

    // no album in the packet, but its tracks share the cover, which downscales identically
    Track& current = np.track;
    bool artistChanged = strncmp(t.artist, current.artist, TEXT_MAX);
    bool titleChanged = strncmp(t.title, current.title, TEXT_MAX);
    bool albumChanged = artistChanged || memcmp(t.art, current.art, sizeof(t.art));
    if ((artistChanged || titleChanged) && np.visible)
    {
        np.previous = current;
        np.slideFrom = albumChanged ? NowPlaying::FROM_TRACK : NowPlaying::FROM_TITLE;
        np.slideSince = millis();
    }
    t.artistSince = albumChanged || !np.active ? frames : current.artistSince;
    t.titleSince = artistChanged || titleChanged || !np.active ? frames : current.titleSince;
    current = t;

    np.active = true;
    np.lastPacket = millis();
}

static void drawMarquee(const char* s, int width, int dx, int baseline, uint16_t color, uint32_t since)
{
    const int x0 = TEXT_X + dx, area = LED_COLS - TEXT_X;
    int offset = 0;

    text.setForegroundColor(color);

    if (width > area)
    {
        uint32_t period = width + SCROLL_GAP;
        uint32_t cycle = scrollHoldFrames + period * scrollFramesPerPx;
        uint32_t t = (frames - since) % cycle;
        if (t > scrollHoldFrames) offset = (t - scrollHoldFrames) / scrollFramesPerPx;
        text.drawUTF8(x0 - offset + period, baseline, s);
    }

    text.drawUTF8(x0 - offset, baseline, s);
}

static void drawArtist(const Track& t, int dy)
{
    drawMarquee(t.artist, t.artistWidth, 0, ARTIST_BASELINE + dy, t.colorArtist, t.artistSince);
}

static void drawTitle(const Track& t, int dx, int dy)
{
    drawMarquee(t.title, t.titleWidth, dx, TITLE_BASELINE + dy, rgb(255, 255, 255), t.titleSince);
}

// the gap and artwork go last so they cover text scrolling out to the left
static void drawArt(const Track& t, int dy)
{
    matrix.fillRect(ART_SIZE, dy, TEXT_X - ART_SIZE, LED_ROWS, colorOff);
    for (int y = 0; y < ART_SIZE; y++)
        for (int x = 0; x < ART_SIZE; x++)
            matrix.drawPixel(x, y + dy, t.art[y * ART_SIZE + x]);
}

// draws a track shifted down by dy rows; whatever leaves the panel is clipped
static void drawTrack(const Track& t, int dy)
{
    drawArtist(t, dy);
    drawTitle(t, 0, dy);
    drawArt(t, dy);
}

static void drawNowPlaying()
{
    matrix.fillScreen(colorOff);

    uint32_t elapsed = millis() - np.slideSince;
    if (elapsed < SLIDE_MS)
    {
        float eased = slideProgress(elapsed);
        if (np.slideFrom == NowPlaying::FROM_TITLE)
        {
            // the new title pushes the old one left, under the artwork
            const int area = LED_COLS - TEXT_X;
            int edge = LED_COLS - lroundf(eased * area);
            drawArtist(np.track, 0);
            drawTitle(np.previous, edge - LED_COLS, 0);
            // a long old title would run on under the new one
            matrix.fillRect(edge, ARTIST_BASELINE + 1, LED_COLS - edge, LED_ROWS, colorOff);
            drawTitle(np.track, edge - TEXT_X, 0);
            drawArt(np.track, 0);
        }
        else
        {
            int dy = lroundf(eased * LED_ROWS);
            if (np.slideFrom == NowPlaying::FROM_TRACK) drawTrack(np.previous, dy);
            if (np.slideFrom == NowPlaying::FROM_CASSETTE) drawCassette(dy);
            drawTrack(np.track, dy - LED_ROWS);
        }
    }
    else
    {
        drawTrack(np.track, 0);
    }

    show();
}

static void beginNowPlaying()
{
    for (int i = 0; i < 256; i++)
        artGamma[i] = (uint8_t)lroundf(powf(i / 255.0f, GAMMA) * 255);

    text.begin(matrix);
    text.setFont(u8g2_font_tom_thumb_4x6_tf);  // smallest readable: 4 px per character, Latin-1
    text.setFontMode(1);                       // transparent background

    // not waited for: the spectrum runs whether or not WiFi ever shows up
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    udp.begin(NP_PORT);

    clip = (int16_t*)ps_malloc(CLIP_SAMPLES * sizeof(int16_t));
    if (clip) clipServer.begin();
    else Serial.println("no PSRAM, no clips for song recognition");
}

// Takes one clip request at a time: records while the client waits, then sends.
// ~0.5 s for 400 kB. Recording has stopped by then, so the clip can't change underneath.
static void sendClip(void*)
{
    clipClient.write((const uint8_t*)clip, CLIP_SAMPLES * sizeof(int16_t));
    clipClient.stop();
    clipSending = false;
    vTaskDelete(nullptr);
}

static void updateClip()
{
    if (!clip || clipSending) return;
    if (!clipClient)
    {
        clipClient = clipServer.accept();
        if (clipClient) clipPos = 0;
        return;
    }
    if (clipPos < CLIP_SAMPLES) return;

    // sent on core 0, next to the WiFi stack, so loop() on core 1 keeps the bars moving;
    // sending here instead froze them for half a second right before the cover came in
    clipSending = true;
    if (xTaskCreatePinnedToCore(sendClip, "clip", 4096, nullptr, 1, nullptr, 0) != pdPASS)
    {
        clipClient.stop();  // no memory for the task: groove-id just gets no clip this time
        clipSending = false;
    }
}

static void tellGrooveId(const char* msg)
{
    udp.beginPacket(WiFi.broadcastIP(), CLIP_ASK_PORT);
    udp.write((const uint8_t*)msg, strlen(msg));
    udp.endPacket();
    Serial.println(msg);
}

static void askForClip(const char* reason)
{
    char msg[32];
    snprintf(msg, sizeof(msg), "clip %s", reason);
    tellGrooveId(msg);
}

// Once per frame: asks for a clip a little after each track starts, and says "stop"
// once the turntable has gone silent, so the LCD needn't wait for the track's end.
static void detectTrackStart(bool vinyl)
{
    static bool wasVinyl = false, inGap = false, starting = false;
    static float loudDb, gapDb;
    static uint32_t quietSince, startSince, lastAsk;
    static const char* reason;
    uint32_t now = millis();

    if (!vinyl)
    {
        if (wasVinyl) tellGrooveId("stop");
        wasVinyl = starting = false;
        return;
    }
    if (!wasVinyl)
    {
        wasVinyl = starting = true;
        inGap = false;
        startSince = quietSince = now;
        loudDb = signalDb;
        reason = "needle";
    }

    loudDb = max(signalDb, loudDb - LOUD_DECAY_DB_S * FRAME_MS / 1000.0f);
    if (signalDb > loudDb - GAP_DB) quietSince = now;
    if (!inGap && now - quietSince >= GAP_MS)
    {
        inGap = true;
        gapDb = signalDb;
    }
    if (inGap)
    {
        gapDb = min(gapDb, signalDb);
        if (signalDb > gapDb + GAP_DB)  // the music is back
        {
            inGap = false;
            starting = true;
            startSince = now;
            loudDb = signalDb;
            reason = "gap";
        }
    }

    if (inGap) return;
    if (starting ? now - startSince >= CLIP_DELAY_MS : now - lastAsk >= RECHECK_MS)
    {
        askForClip(starting ? reason : "recheck");
        starting = false;
        lastAsk = now;
    }
}

#ifdef IDLE_LOG
static void sendLog(const char* fmt, ...)
{
    char line[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    udp.beginPacket(WiFi.broadcastIP(), LOG_PORT);
    udp.printf("%s\n", line);
    udp.endPacket();
}

// The needle is up, yet a band rises above its noise floor: reports each such burst
// once it's over, with how loud it was before and after despiking.
static void logIdle()
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (noiseStatusNew)
    {
        sendLog("%lus %s", millis() / 1000, noiseStatus);
        noiseStatusNew = false;
    }

    // the panel going to the spectrum and back: which band took it from Spotify
    const float binHz = (float)SAMPLE_HZ / FFT_N;
    static bool vinyl = false;
    static uint32_t vinylSince = 0;
    if ((millis() - lastVinyl <= VINYL_HOLD_MS) != vinyl)
    {
        vinyl = !vinyl;
        if (vinyl)
        {
            int loudest = 0;
            for (int b = 1; b < BANDS; b++)
                if (bands[b] - bandNoise[b] > bands[loudest] - bandNoise[loudest]) loudest = b;
            sendLog("%lus vinyl on: band %d (%d-%d Hz) %+.1f dB above its floor, needs %+d", millis() / 1000,
                    loudest, (int)(bandEdge[loudest] * binHz), (int)(bandEdge[loudest + 1] * binHz),
                    bands[loudest] - bandNoise[loudest], vinylThreshold());
            vinylSince = millis();
        }
        else
            sendLog("%lus vinyl off after %lus", millis() / 1000, (millis() - vinylSince) / 1000);
    }

    static uint32_t lastBeat = -LOG_BEAT_MS;
    if (millis() - lastBeat > LOG_BEAT_MS)
    {
        lastBeat = millis();
        sendLog("%lus alive, above noise %+.1f dB, WiFi %d dBm", millis() / 1000, signalDb,
                WiFi.RSSI());
    }

    static struct Spike
    {
        uint32_t since = 0;
        int frames = 0, band = 0;
        float raw = 0.0f, filtered = -1e9f;  // dB above the noise floor
    } spike;

    int band = -1;
    float raw = 0.0f, filtered = -1e9f;
    if (!booting() && millis() - lastVinyl > VINYL_HOLD_MS)
        for (int b = 0; b < BANDS; b++)
        {
            if (bandsRaw[b] - bandNoise[b] > raw)
            {
                raw = bandsRaw[b] - bandNoise[b];
                band = b;
            }
            filtered = max(filtered, bands[b] - bandNoise[b]);
        }

    if (band >= 0)
    {
        if (!spike.frames) spike.since = millis();
        spike.frames++;
        if (raw > spike.raw)
        {
            spike.raw = raw;
            spike.band = band;
        }
        spike.filtered = max(spike.filtered, filtered);
    }
    else if (spike.frames)
    {
        sendLog("%lus spike: band %d (%d-%d Hz), %+.1f dB for %d frame%s, after filter %+.1f dB%s",
                spike.since / 1000, spike.band, (int)(bandEdge[spike.band] * binHz),
                (int)(bandEdge[spike.band + 1] * binHz), spike.raw, spike.frames,
                spike.frames == 1 ? "" : "s", spike.filtered,
                spike.filtered > 0.0f ? " -> SHOWN" : "");
        spike = Spike();
    }
}
#endif

// A new record track pushes the bars down, stays, then the bars push it back out.
// False once it has left.
static bool drawPopIn()
{
    uint32_t t = millis() - record.popSince;
    if (pendingColors && t >= SLIDE_MS) applyPendingColors();  // the bars are off the panel now
    if (t >= record.popMs + SLIDE_MS) return false;

    int dy = LED_ROWS;
    if (t < SLIDE_MS) dy = lroundf(slideProgress(t) * LED_ROWS);
    else if (t >= record.popMs) dy = LED_ROWS - lroundf(slideProgress(t - record.popMs) * LED_ROWS);

    matrix.fillScreen(colorOff);
    drawBars(dy);
    drawTrack(record.track, dy - LED_ROWS);
    show();
    return true;
}

// Draws the now playing view and returns true while Spotify plays and the
// turntable is silent, or while a recognized record track pops in. The ADC only
// hears the turntable, so vinyl always wins.
static bool updateNowPlaying()
{
    if (!mdnsStarted && WiFi.status() == WL_CONNECTED)
    {
        mdnsStarted = MDNS.begin(HOSTNAME);
        Serial.printf("WiFi up, %s.local -> %s\n", HOSTNAME, WiFi.localIP().toString().c_str());
    }

    // a broadcast hello gets an immediate answer instead of waiting for the next heartbeat
    static uint32_t lastHello = 0;
    if (!heardFromPi && WiFi.status() == WL_CONNECTED && millis() - lastHello > HELLO_MS)
    {
        lastHello = millis();
        udp.beginPacket(WiFi.broadcastIP(), NP_PORT);
        udp.write((const uint8_t*)"NP\x04H", 4);
        udp.endPacket();
    }

    receiveNowPlaying();
    updateClip();
    if (np.active && millis() - np.lastPacket > NP_TIMEOUT_MS) np.active = false;

    if (signalDb > vinylThreshold()) lastVinyl = millis();
#ifdef IDLE_LOG
    logIdle();
#endif
    // waits for the boot animation to finish
    bool vinyl = millis() - lastVinyl <= VINYL_HOLD_MS;
    if (WiFi.status() == WL_CONNECTED) detectTrackStart(vinyl);
    if (!vinyl) record.known = albumColors = pendingColors = false;  // the next record starts in the bars' own colors
    bool visible = np.active && !booting() && !vinyl;

    // stopped, the track slides out at the bottom like it does for a new one; vinyl
    // takes the panel straight away
    if (np.visible && !visible && !vinyl)
    {
        np.leaving = true;
        np.leaveSince = millis();
    }
    if (visible || vinyl || millis() - np.leaveSince >= SLIDE_MS) np.leaving = false;

    // appearing, the track slides in from the top, pushing the cassette out at boot;
    // if the tape is already on its way down, the track joins in at the same height
    if (visible && !np.visible)
    {
        np.slideFrom = cassetteShown ? NowPlaying::FROM_CASSETTE : NowPlaying::FROM_BLANK;
        np.slideSince = cassetteShown ? bootStart + BOOT_MS : millis();
    }
    np.visible = visible;

    if (visible) drawNowPlaying();
    if (np.leaving)
    {
        matrix.fillScreen(colorOff);
        drawTrack(np.track, lroundf(slideProgress(millis() - np.leaveSince) * LED_ROWS));
        show();
    }
    if (vinyl && record.known && !ejecting() && drawPopIn()) return true;
    return visible || np.leaving;
}
#endif

// Calibration: hold UP with the needle up. For CAL_MS the panel shows a progress bar
// while each band's level is averaged per second; the loudest second becomes the band's
// noise, stored in flash so it survives power cuts and firmware updates.
Preferences prefs;
#define NOISE_LAYOUT (SAMPLE_HZ * 1000u + FFT_N + BAND_LOW_HZ)  // changes when the bands do
int8_t bandNoiseMeasured[BANDS];

static void applyNoise(const int8_t* noise, const char* source)
{
    memcpy(bandNoiseMeasured, noise, BANDS);
    for (int b = 0; b < BANDS; b++) bandNoise[b] = noise[b] + NOISE_MARGIN_DB;

    int n = snprintf(noiseStatus, sizeof(noiseStatus), "noise %s: {", source);
    for (int b = 0; b < BANDS && n < (int)sizeof(noiseStatus); b++)
        n += snprintf(noiseStatus + n, sizeof(noiseStatus) - n, b ? ", %d" : "%d", noise[b]);
    if (n < (int)sizeof(noiseStatus)) snprintf(noiseStatus + n, sizeof(noiseStatus) - n, "}");
    noiseStatusNew = true;
    Serial.println(noiseStatus);
}

static void beginCalibration()
{
    pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
    int8_t stored[BANDS];
    prefs.begin("vinyl");
    // a calibration only fits the band layout it was measured with
    bool have = prefs.getUInt("layout") == NOISE_LAYOUT && prefs.getBytesLength("noise") == BANDS &&
                prefs.getBytes("noise", stored, BANDS) == BANDS;
    applyNoise(have ? stored : bandNoiseDb,
               have ? "from flash" : "defaults, no calibration for these bands");
}

// returns true while calibrating, having drawn the frame
static bool updateCalibration()
{
    static uint32_t pressedSince = 0, calStart = 0, secondStart = 0, calFrames = 0;
    static bool calibrating = false, released = true;
    static float sum[BANDS], loudest[BANDS];

    bool pressed = digitalRead(PIN_BUTTON_UP) == LOW;
    if (!pressed) released = true;
    if (!calibrating)
    {
        if (!pressed || !released)
        {
            pressedSince = millis();
            return false;
        }
        if (millis() - pressedSince < CAL_HOLD_MS) return false;

        calibrating = true;
        released = false;  // still held; a new hold is needed for the next run
        calStart = secondStart = millis();
        calFrames = 0;
        for (int b = 0; b < BANDS; b++) sum[b] = 0.0f, loudest[b] = -1e9f;
        Serial.println("calibrating noise floor, keep the needle up");
    }

    for (int b = 0; b < BANDS; b++) sum[b] += bands[b];
    calFrames++;
    if (millis() - secondStart >= 1000)
    {
        for (int b = 0; b < BANDS; b++)
        {
            loudest[b] = max(loudest[b], sum[b] / calFrames);
            sum[b] = 0.0f;
        }
        calFrames = 0;
        secondStart = millis();
    }

    uint32_t elapsed = millis() - calStart;
    if (elapsed >= CAL_MS)
    {
        int8_t noise[BANDS];
        for (int b = 0; b < BANDS; b++) noise[b] = (int8_t)ceilf(loudest[b]);
        prefs.putBytes("noise", noise, BANDS);
        prefs.putUInt("layout", NOISE_LAYOUT);
        applyNoise(noise, "calibrated");
        calibrating = false;
        return false;
    }

    // progress bar across the middle rows
    matrix.fillScreen(colorOff);
    matrix.fillRect(0, LED_ROWS / 2 - 1, LED_COLS * elapsed / CAL_MS + 1, 2, colorPeak);
    show();
    return true;
}

void setup()
{
    Serial.begin(115200);

    beginAudio();

    if (!matrix.begin())
    {
        Serial.println("matrix init failed");
        while (1) delay(10);
    }
    matrix.setBrightness8(BRIGHTNESS);

#ifdef PANEL_TEST
    // a still image to check the panel mapping: white border, red top left, green top
    // right, blue bottom left, yellow bottom right, a diagonal from the top left
    matrix.fillScreen(0);
    matrix.drawRect(0, 0, LED_COLS, LED_ROWS, rgb(255, 255, 255));
    for (int i = 0; i < LED_ROWS; i++) matrix.drawPixel(i, i, rgb(0, 160, 255));
    matrix.drawPixel(0, 0, rgb(255, 0, 0));
    matrix.drawPixel(LED_COLS - 1, 0, rgb(0, 255, 0));
    matrix.drawPixel(0, LED_ROWS - 1, rgb(0, 0, 255));
    matrix.drawPixel(LED_COLS - 1, LED_ROWS - 1, rgb(255, 255, 0));
    show();
    while (1) delay(1000);
#endif

    colorOff = rgb(0, 0, 0);
    // soft sage at the bottom fading into lilac on top
    const uint8_t sage[3] = {80, 200, 90}, lilac[3] = {190, 70, 255};
    fillGradient(colorRow, sage, lilac);
    colorPeak = rgb(lilac[0], lilac[1], lilac[2]);

    // Log-spaced band edges. The FFT is too coarse to split the bass logarithmically,
    // so each band takes at least one bin of its own: linear up to ~230 Hz, log above.
    const float binHz = (float)SAMPLE_HZ / FFT_N, highHz = SAMPLE_HZ / 2.0f;
    for (int b = 0; b <= BANDS; b++)
    {
        int bin = lroundf(BAND_LOW_HZ * powf(highHz / BAND_LOW_HZ, (float)b / BANDS) / binHz);
        if (b > 0) bin = max(bin, bandEdge[b - 1] + 1);
        bandEdge[b] = min(bin, FFT_N / 2);
    }
    beginCalibration();

#ifdef NOW_PLAYING
    beginNowPlaying();
#endif

    bootStart = millis();
}

void loop()
{
    frames++;
    sampleAudio();
    runFFT();
    if (updateCalibration()) return;

    // with the needle up, "above noise" should sit around -NOISE_MARGIN_DB; if a band
    // twitches, recalibrate (hold UP)
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 1000)
    {
        lastPrint = millis();
        // below SAMPLE_HZ means frames take longer than they last and samples get dropped
        Serial.printf("above noise %+.1f dB, peak %.1f dB, keeping up with %lu of %d Hz\n", signalDb,
                      peakDb, samplesUsed, SAMPLE_HZ);
        samplesUsed = 0;

        // each band's level; with the needle up, this row is what bandNoiseDb holds
        Serial.print("  needle up: {");
        for (int b = 0; b < BANDS; b++)
        {
            Serial.printf(b ? ", %.0f" : "%.0f", ceilf(bandLevelSum[b] / bandLevelFrames));
            bandLevelSum[b] = 0.0f;
        }
        Serial.println("}");
        bandLevelFrames = 0;
    }

#ifdef NOW_PLAYING
    bool nowPlaying = updateNowPlaying();
    cassetteShown = false;
    if (nowPlaying) return;
#endif

    if (ejecting())
    {
        int dy = booting() ? 0 : lroundf(slideProgress(bootElapsed() - BOOT_MS) * LED_ROWS);
        matrix.fillScreen(colorOff);
        drawCassette(dy);
        show();
        cassetteShown = true;
        return;
    }

    cassetteShown = false;
    draw();
}
