#include <Adafruit_Protomatter.h>
#include <Arduino.h>
#include <arduinoFFT.h>

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

#define LED_COLS 32
#define LED_ROWS 16

// GPIO10 / ADC1_CH9. A0 and A4 sit on ADC2, which stops working once WiFi runs.
#define AUDIO_PIN A3

#define FFT_N 256        // 128 bins of ~47 Hz
#define SAMPLE_HZ 12000  // 6 kHz span; analogRead takes ~75 us, so ~13 kHz is the ceiling
#define BIN_WIDTH 2      // columns sharing one band, as the sketch booted on AVR
#define BANDS (LED_COLS / BIN_WIDTH)
#define BAND_LOW_HZ 60   // bands are spaced logarithmically from here up to SAMPLE_HZ / 2

#define PEAK_FALLOFF_MS 500  // how long a peak marker hangs before sinking
#define NOISE_FLOOR_DB -28   // per-bin noise level; raise if bars twitch with the needle up
#define DISPLAY_DB 30        // range the bars show, from the recent loudest band down
#define MM_SIZE 32           // frames averaged for the scale and silence detection
#define ATTACK_MS 25    // how fast bars rise to a new level
#define RELEASE_MS 140  // how slowly they sink back; raise for calmer bars

// MatrixPortal S3 pins (as in Adafruit's Protomatter examples)
uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48};
uint8_t clockPin = 2, latchPin = 47, oePin = 14;

Adafruit_Protomatter matrix(LED_COLS, 6, 1, rgbPins, 3, addrPins,
                            clockPin, latchPin, oePin, true);

float vReal[FFT_N], vImag[FFT_N];
ArduinoFFT<float> FFT(vReal, vImag, FFT_N, (float)SAMPLE_HZ);

float bands[BANDS];
uint8_t bandEdge[BANDS + 1];  // first FFT bin of each band, plus the end of the last
float bandNoise[BANDS];       // NOISE_FLOOR_DB raised by the band's width: noise adds up per bin
uint32_t sampleMicros;        // how long the last FFT_N samples took, to check SAMPLE_HZ is met
uint8_t peaks[LED_COLS];
uint32_t peakMillis[LED_COLS];
float barLevel[LED_COLS];
uint32_t lastDraw = 0;

uint16_t colorOff, colorPeak, colorRow[LED_ROWS];

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

MovingAverage peakAvg, signalAvg;
float peakDb;    // recent loudest band, the top of the bar scale
float signalDb;  // recent loudest band relative to its noise floor; > 0 means audio

// RGB565 gives green one bit more than red and blue, so dark grays would light
// only the green LED. Quantizing all three to 5 bits keeps darks neutral.
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (r >> 3 << 11) | (g >> 3 << 6) | (b >> 3);
}

static void resetPeaks()
{
    for (int i = 0; i < LED_COLS; i++)
    {
        peaks[i] = LED_ROWS;
        peakMillis[i] = millis();
    }

    // double buffering means each frame has to be redrawn whole, not appended
    for (int i = 0; i < LED_COLS; i++)
    {
        matrix.fillScreen(colorOff);
        for (int k = 0; k <= i; k++) matrix.drawPixel(k, 0, colorPeak);
        matrix.show();
        delay(13);
    }
}

static void sampleAudio()
{
    const uint32_t period = 1000000UL / SAMPLE_HZ;
    uint32_t start = micros(), next = start;
    int32_t sum = 0;

    for (int i = 0; i < FFT_N; i++)
    {
        while ((int32_t)(micros() - next) < 0) {}
        next += period;

        int raw = analogRead(AUDIO_PIN);
        vReal[i] = raw;
        vImag[i] = 0.0f;
        sum += raw;
    }
    sampleMicros = micros() - start;

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

        bands[b] = 10.0f * log10f(max(power, 1e-12f));
        maxDb = max(maxDb, bands[b]);
        maxSignal = max(maxSignal, bands[b] - bandNoise[b]);
    }

    peakDb = peakAvg.add(maxDb);
    signalDb = signalAvg.add(maxSignal);
}

static void draw()
{
    // Bars span DISPLAY_DB below the recent loudest band, but never reach into a band's
    // noise: quiet passages show whatever rises above it, silence shows nothing.
    const float scale = (float)LED_ROWS / DISPLAY_DB;

    // time-based so the feel doesn't depend on how fast loop() runs
    uint32_t now = millis();
    float dt = (float)(now - lastDraw);
    lastDraw = now;
    float attack = 1.0f - expf(-dt / ATTACK_MS);
    float release = 1.0f - expf(-dt / RELEASE_MS);

    matrix.fillScreen(colorOff);

    for (int i = 0; i < LED_COLS; i++)
    {
        int b = i / BIN_WIDTH;
        float bottom = max(peakDb - DISPLAY_DB, bandNoise[b]);
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
            matrix.drawPixel(i, LED_ROWS - 1 - j, colorRow[j]);

        if (peaks[i] > 0) matrix.drawPixel(i, LED_ROWS - peaks[i], colorPeak);
    }

    matrix.show();
}

#ifdef NOW_PLAYING
// now playing, pushed by snapserver-lcd over UDP
#define HOSTNAME "vinyl-matrix"     // reachable as vinyl-matrix.local
#define NP_PORT 4210
#define NP_TIMEOUT_MS 30000         // sender repeats every 10 s; drop the view after this
#define VINYL_HOLD_MS 3000          // silence needed before handing the panel to spotify
#define ART_SIZE 16
#define ART_GAMMA 2.2f              // LEDs are linear, album art is sRGB
#define TEXT_MAX 128
#define SCROLL_PX_PER_S 15
#define SCROLL_HOLD_MS 1500         // pause at the start of each marquee cycle
#define SCROLL_GAP 8                // blank pixels between marquee repeats
#define TEXT_X (ART_SIZE + 1)       // one blank column between artwork and text
#define TITLE_BASELINE 15           // 3x5 font: caps on rows 10-14, descenders on the bottom row
#define ARTIST_BASELINE 9           // one font line (6 px) above: caps on rows 4-8
#define SLIDE_MS 500                // a new song pushes the old one down and out

U8G2_FOR_ADAFRUIT_GFX text;

struct Track
{
    uint32_t textSince;  // marquee start
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
    Track track, previous;  // previous is only drawn while it slides out
} np;

WiFiUDP udp;
uint8_t packet[1200];
uint8_t artGamma[256];
uint32_t lastVinyl = 0;
bool mdnsStarted = false;

// Packet: "NP" version cmd, then for cmd 'P':
// 16x16 RGB888 art (row-major), accent RGB, artist\0, title\0 (UTF-8)
// cmd 'S' (or silence for NP_TIMEOUT_MS) ends the now playing view.
static void receiveNowPlaying()
{
    int len = udp.parsePacket();
    if (len <= 0) return;
    len = udp.read(packet, sizeof(packet));

    const int artBytes = ART_SIZE * ART_SIZE * 3;
    if (len < 4 || packet[0] != 'N' || packet[1] != 'P' || packet[2] != 4) return;

    if (packet[3] == 'S')
    {
        np.active = false;
        return;
    }
    if (packet[3] != 'P' || len < 4 + artBytes + 3 + 2) return;

    // both strings must be terminated inside the packet
    const uint8_t *p = packet + 4;
    const char *artist = (const char *)p + artBytes + 3;
    const char *end = (const char *)packet + len;
    const char *title = (const char *)memchr(artist, 0, end - artist);
    if (!title || !memchr(++title, 0, end - title)) return;

    Track &t = np.track;
    bool changed = strncmp(artist, t.artist, TEXT_MAX) || strncmp(title, t.title, TEXT_MAX);
    if (changed && np.active)
    {
        np.previous = t;
        np.slideSince = millis();
    }
    if (changed || !np.active) t.textSince = millis();

    for (int i = 0; i < ART_SIZE * ART_SIZE; i++, p += 3)
        t.art[i] = rgb(artGamma[p[0]], artGamma[p[1]], artGamma[p[2]]);
    t.colorArtist = rgb(p[0], p[1], p[2]);
    strlcpy(t.artist, artist, TEXT_MAX);
    strlcpy(t.title, title, TEXT_MAX);
    t.artistWidth = text.getUTF8Width(t.artist);
    t.titleWidth = text.getUTF8Width(t.title);

    np.active = true;
    np.lastPacket = millis();
}

static void drawMarquee(const char *s, int width, int baseline, uint16_t color, uint32_t since)
{
    const int x0 = TEXT_X, area = LED_COLS - TEXT_X;
    int offset = 0;

    text.setForegroundColor(color);

    if (width > area)
    {
        uint32_t period = width + SCROLL_GAP;
        uint32_t cycle = SCROLL_HOLD_MS + period * 1000 / SCROLL_PX_PER_S;
        uint32_t t = (millis() - since) % cycle;
        if (t > SCROLL_HOLD_MS) offset = (t - SCROLL_HOLD_MS) * SCROLL_PX_PER_S / 1000;
        text.drawUTF8(x0 - offset + period, baseline, s);
    }

    text.drawUTF8(x0 - offset, baseline, s);
}

// draws a track shifted down by dy rows; whatever leaves the panel is clipped
static void drawTrack(const Track &t, int dy)
{
    drawMarquee(t.artist, t.artistWidth, ARTIST_BASELINE + dy, t.colorArtist, t.textSince);
    drawMarquee(t.title, t.titleWidth, TITLE_BASELINE + dy, rgb(255, 255, 255), t.textSince);

    // the gap and artwork go last so they cover text scrolling out to the left
    matrix.fillRect(ART_SIZE, dy, TEXT_X - ART_SIZE, LED_ROWS, colorOff);
    for (int y = 0; y < ART_SIZE; y++)
        for (int x = 0; x < ART_SIZE; x++)
            matrix.drawPixel(x, y + dy, t.art[y * ART_SIZE + x]);
}

static void drawNowPlaying()
{
    matrix.fillScreen(colorOff);

    uint32_t elapsed = millis() - np.slideSince;
    if (elapsed < SLIDE_MS)
    {
        // ease out: fast start, settles gently
        float p = 1.0f - (float)elapsed / SLIDE_MS;
        int dy = lroundf((1.0f - p * p) * LED_ROWS);
        drawTrack(np.previous, dy);
        drawTrack(np.track, dy - LED_ROWS);
    }
    else
    {
        drawTrack(np.track, 0);
    }

    matrix.show();
}

static void beginNowPlaying()
{
    for (int i = 0; i < 256; i++)
        artGamma[i] = (uint8_t)lroundf(powf(i / 255.0f, ART_GAMMA) * 255);

    text.begin(matrix);
    text.setFont(u8g2_font_tom_thumb_4x6_tf);  // smallest readable: 4 px per character, Latin-1
    text.setFontMode(1);                       // transparent background

    // not waited for: the spectrum runs whether or not WiFi ever shows up
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    udp.begin(NP_PORT);
}

// Draws the now playing view and returns true while Spotify plays and the
// turntable is silent. The ADC only hears the turntable, so vinyl always wins.
static bool updateNowPlaying()
{
    if (!mdnsStarted && WiFi.status() == WL_CONNECTED)
    {
        mdnsStarted = MDNS.begin(HOSTNAME);
        Serial.printf("WiFi up, %s.local -> %s\n", HOSTNAME, WiFi.localIP().toString().c_str());
    }

    receiveNowPlaying();
    if (np.active && millis() - np.lastPacket > NP_TIMEOUT_MS) np.active = false;

    if (signalDb > 0.0f) lastVinyl = millis();
    if (!np.active || millis() - lastVinyl <= VINYL_HOLD_MS) return false;

    drawNowPlaying();
    return true;
}
#endif

void setup()
{
    Serial.begin(115200);

    analogReadResolution(12);
    analogSetPinAttenuation(AUDIO_PIN, ADC_11db);

    if (matrix.begin() != PROTOMATTER_OK)
    {
        Serial.println("Protomatter init failed");
        while (1) delay(10);
    }

    colorOff = rgb(0, 0, 0);
    // soft sage at the bottom fading into lilac on top
    const uint8_t sage[3] = {80, 200, 90}, lilac[3] = {190, 70, 255};
    for (int j = 0; j < LED_ROWS; j++)
    {
        uint32_t t = (uint32_t)j * 255 / (LED_ROWS - 1);
        uint8_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = (sage[k] * (255 - t) + lilac[k] * t) / 255;
        colorRow[j] = rgb(c[0], c[1], c[2]);
    }
    colorPeak = rgb(lilac[0], lilac[1], lilac[2]);

    // Log-spaced band edges. The FFT is too coarse to split the bass logarithmically,
    // so each band takes at least one bin of its own: linear up to ~400 Hz, log above.
    const float binHz = (float)SAMPLE_HZ / FFT_N, highHz = SAMPLE_HZ / 2.0f;
    for (int b = 0; b <= BANDS; b++)
    {
        int bin = lroundf(BAND_LOW_HZ * powf(highHz / BAND_LOW_HZ, (float)b / BANDS) / binHz);
        if (b > 0) bin = max(bin, bandEdge[b - 1] + 1);
        bandEdge[b] = min(bin, FFT_N / 2);
    }
    for (int b = 0; b < BANDS; b++)
        bandNoise[b] = NOISE_FLOOR_DB + 10.0f * log10f(bandEdge[b + 1] - bandEdge[b]);

#ifdef NOW_PLAYING
    beginNowPlaying();
#endif

    resetPeaks();
}

void loop()
{
    sampleAudio();
    runFFT();

    // for tuning NOISE_FLOOR_DB: with the needle up, "above noise" should stay a few dB
    // below 0; add it to NOISE_FLOOR_DB if it doesn't
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 1000)
    {
        lastPrint = millis();
        Serial.printf("above noise %+.1f dB, peak %.1f dB, sampling at %lu Hz\n", signalDb, peakDb,
                      1000000UL * FFT_N / sampleMicros);
    }

#ifdef NOW_PLAYING
    if (updateNowPlaying()) return;
#endif

    draw();
}
