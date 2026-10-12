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

#define FFT_N 64        // 32 bins, one per column
#define SAMPLE_HZ 9000  // ~4.5 kHz span, matching the old AVR analogRead pacing
#define BIN_WIDTH 2     // columns sharing one band, as the sketch booted on AVR

#define PEAK_FALLOFF_MS 500  // how long a peak marker hangs before sinking
#define SILENCE_FLOOR 0.16f  // noise sits below this; raise if idle bars twitch
#define MM_SIZE 32
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

float spectrum[FFT_N / 2];
uint8_t peaks[LED_COLS];
uint32_t peakMillis[LED_COLS];
float barLevel[LED_COLS];
uint32_t lastDraw = 0;

uint16_t colorOff, colorPeak, colorRow[LED_ROWS];

// full-scale reference for the log amplitude scale, as in the original sketch
const float normAmp = logf(32768.0f);
float avgAmpNorm = 0.0f;

float mmValues[MM_SIZE];
float mmSum = 0.0f;
int mmIndex = 0, mmCount = 0;

// RGB565 gives green one bit more than red and blue, so dark grays would light
// only the green LED. Quantizing all three to 5 bits keeps darks neutral.
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (r >> 3 << 11) | (g >> 3 << 6) | (b >> 3);
}

static float addValue(float val)
{
    mmSum = mmSum - mmValues[mmIndex] + val;
    mmValues[mmIndex] = val;
    mmIndex = (mmIndex + 1) % MM_SIZE;
    mmCount = min(mmCount + 1, MM_SIZE);
    return mmSum / (float)mmCount;
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
    uint32_t next = micros();
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

    // subtract the measured bias rather than trusting the divider to sit at mid-rail
    float mean = (float)sum / FFT_N;
    for (int i = 0; i < FFT_N; i++) vReal[i] = (vReal[i] - mean) / 8.0f;
}

static void runFFT()
{
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    float maxAmpNorm = 0.0f;

    for (int i = 0; i < FFT_N / 2; i++)
    {
        spectrum[i] = 0.0f;

        // fix_fft scaled its output down by N; match that so normAmp still fits
        float mag = vReal[i] / FFT_N;
        float amp = mag * mag;

        if (amp > 1.0f)
        {
            float ampNorm = logf(amp) / normAmp;
            maxAmpNorm = max(maxAmpNorm, ampNorm);
            spectrum[i] = ampNorm;
        }
    }

    avgAmpNorm = addValue(maxAmpNorm);
}

static void draw()
{
    for (int i = 0; i < LED_COLS / BIN_WIDTH; i++)
    {
        float maxVal = 0.0f;
        for (int j = 0; j < BIN_WIDTH; j++)
            maxVal = max(maxVal, spectrum[i * BIN_WIDTH + j]);
        for (int j = 0; j < BIN_WIDTH; j++) spectrum[i * BIN_WIDTH + j] = maxVal;
    }

    float scale = avgAmpNorm < SILENCE_FLOOR ? 0.0f : 1.0f / avgAmpNorm * LED_ROWS;

    // time-based so the feel doesn't depend on how fast loop() runs
    uint32_t now = millis();
    float dt = (float)(now - lastDraw);
    lastDraw = now;
    float attack = 1.0f - expf(-dt / ATTACK_MS);
    float release = 1.0f - expf(-dt / RELEASE_MS);

    matrix.fillScreen(colorOff);

    for (int i = 0; i < LED_COLS; i++)
    {
        float target = min(spectrum[i] * scale, (float)LED_ROWS);
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

U8G2_FOR_ADAFRUIT_GFX text;

struct NowPlaying
{
    bool active = false;
    uint32_t lastPacket = 0;
    uint32_t textSince = 0;  // marquee start, reset when the text changes
    uint16_t art[ART_SIZE * ART_SIZE];
    uint16_t colorArtist;
    char artist[TEXT_MAX], title[TEXT_MAX];
    int artistWidth, titleWidth;  // in px
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

    const uint8_t *p = packet + 4;
    for (int i = 0; i < ART_SIZE * ART_SIZE; i++, p += 3)
        np.art[i] = rgb(artGamma[p[0]], artGamma[p[1]], artGamma[p[2]]);
    np.colorArtist = rgb(p[0], p[1], p[2]);
    p += 3;

    // both strings must be terminated inside the packet
    const char *artist = (const char *)p;
    const char *end = (const char *)packet + len;
    const char *title = (const char *)memchr(artist, 0, end - artist);
    if (!title || !memchr(++title, 0, end - title)) return;

    if (strncmp(artist, np.artist, TEXT_MAX) || strncmp(title, np.title, TEXT_MAX) || !np.active)
        np.textSince = millis();
    strlcpy(np.artist, artist, TEXT_MAX);
    strlcpy(np.title, title, TEXT_MAX);
    np.artistWidth = text.getUTF8Width(np.artist);
    np.titleWidth = text.getUTF8Width(np.title);

    np.active = true;
    np.lastPacket = millis();
}

static void drawMarquee(const char *s, int width, int baseline, uint16_t color)
{
    const int x0 = TEXT_X, area = LED_COLS - TEXT_X;
    int offset = 0;

    text.setForegroundColor(color);

    if (width > area)
    {
        uint32_t period = width + SCROLL_GAP;
        uint32_t cycle = SCROLL_HOLD_MS + period * 1000 / SCROLL_PX_PER_S;
        uint32_t t = (millis() - np.textSince) % cycle;
        if (t > SCROLL_HOLD_MS) offset = (t - SCROLL_HOLD_MS) * SCROLL_PX_PER_S / 1000;
        text.drawUTF8(x0 - offset + period, baseline, s);
    }

    text.drawUTF8(x0 - offset, baseline, s);
}

static void drawNowPlaying()
{
    matrix.fillScreen(colorOff);
    drawMarquee(np.artist, np.artistWidth, ARTIST_BASELINE, np.colorArtist);
    drawMarquee(np.title, np.titleWidth, TITLE_BASELINE, rgb(255, 255, 255));

    // the gap and artwork go last so they cover text scrolling out to the left
    matrix.fillRect(ART_SIZE, 0, TEXT_X - ART_SIZE, LED_ROWS, colorOff);
    for (int y = 0; y < ART_SIZE; y++)
        for (int x = 0; x < ART_SIZE; x++)
            matrix.drawPixel(x, y, np.art[y * ART_SIZE + x]);

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

    if (avgAmpNorm >= SILENCE_FLOOR) lastVinyl = millis();
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

#ifdef NOW_PLAYING
    beginNowPlaying();
#endif

    memset(mmValues, 0, sizeof(mmValues));
    resetPeaks();
}

void loop()
{
    sampleAudio();
    runFFT();

#ifdef NOW_PLAYING
    if (updateNowPlaying()) return;
#endif

    draw();
}
