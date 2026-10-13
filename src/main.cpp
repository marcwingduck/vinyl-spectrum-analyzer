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

// Uncomment to broadcast stray peaks while the turntable is silent (needs secrets.h).
// Off, it costs nothing; see "Stray peaks while idle" in the README.
#define IDLE_LOG

#define LED_COLS 32
#define LED_ROWS 16

// GPIO10 / ADC1_CH9. A0 and A4 sit on ADC2, which stops working once WiFi runs.
#define AUDIO_PIN A3

#define FFT_N 256        // 128 bins of ~47 Hz
#define SAMPLE_HZ 12000  // 6 kHz span; analogRead takes ~75 us, so ~13 kHz is the ceiling
#define BIN_WIDTH 2      // columns sharing one band, as the sketch booted on AVR
#define BANDS (LED_COLS / BIN_WIDTH)
#define BAND_LOW_HZ 60  // bands are spaced logarithmically from here up to SAMPLE_HZ / 2

#define PEAK_FALLOFF_MS 500  // how long a peak marker hangs before sinking
#define NOISE_FLOOR_DB -28   // per-bin noise level; raise if bars twitch with the needle up
#define DISPLAY_DB 30        // range the bars show, from the recent loudest band down
#define MM_SIZE 32           // frames averaged for the scale and silence detection
#define ATTACK_MS 25         // how fast bars rise to a new level
#define BOOT_MS 2000         // the cassette's loading bar fills over this long
#define BOOT_HOLD_MS 1000    // then the loaded tape stays before anything takes over
#define SLIDE_MS 500         // pushes run this long: new album down, new title left, tape out
#define REEL_STEP_MS 500
#define FADE_FROM 0.5f       // fade start; much lower and colors tint, 5 bits per channel
#define GAMMA 2.2f      // LEDs are linear, images are sRGB
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
#ifdef IDLE_LOG
float bandsRaw[BANDS];  // before despiking, for the UDP log
#endif
float bandHistory[2][BANDS];  // the two frames before, for despiking
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

// boot screen, from cassette.png: '.' is off, letters index cassettePalette
static const char* const cassette[LED_ROWS] = {
    ".....AAAAAAAAAAAAAAAAAAAAAA.....",
    "....AABBBBBBBBBBBBBBBBBBBBAA....",
    "....ABBBBBBBBBBBBBBBBBBBBBBA....",
    "....ABBBBBBBBBBBBBBBBBBBBBBA....",
    "....ABBBBBBBBBBBBBBBBBBBBBBA....",
    "....ABBBBB............BBBBBA....",
    "....ABBBB.CC.DDDDDD.CC.BBBBA....",
    "....ABBBB.CC.DDDDDD.CC.BBBBA....",
    "....ABBBBB............BBBBBA....",
    "....ABBBBBBBBBBBBBBBBBBBBBBA....",
    "....AABBBBBBBBBBBBBBBBBBBBAA....",
    "....AAAAA..............AAAAA....",
    "....AAAA.AAAAAAAAAAAAAA.AAAA....",
    "....AAAA.AAA.AAAAAA.AAA.AAAA....",
    "....AAA.AA.AAAAAAAAAA.AA.AAA....",
    ".....AA.AAAAAAAAAAAAAAAA.AA.....",
};
static const uint8_t cassettePalette[][3] = {
    {164, 164, 164},  // A
    {190, 70, 255},   // B
    {255, 255, 255},  // C
    {0, 0, 0},  // D
};
static const int reelX[] = {10, 20}, reelY = 6;  // top left of the 2x2 reel hubs
static const int loadX = 13, loadW = 6;           // the D window, a loading bar over BOOT_MS
bool cassetteShown = false;                       // drawn last frame, so a track can push it out
uint32_t bootStart;                               // end of setup(), when the cassette appears

static uint32_t bootElapsed() { return millis() - bootStart; }
static bool booting() { return bootElapsed() < BOOT_MS + BOOT_HOLD_MS; }
// with nothing to push it out, the tape slides down on its own
static bool ejecting() { return bootElapsed() < BOOT_MS + BOOT_HOLD_MS + SLIDE_MS; }

// how far a push has come, 0..1 over SLIDE_MS; ease out: fast start, settles gently
static float slideProgress(uint32_t elapsed)
{
    float p = 1.0f - min(elapsed, (uint32_t)SLIDE_MS) / (float)SLIDE_MS;
    return 1.0f - p * p;
}

// draws the cassette shifted down by dy rows, its reels turning clockwise
static void drawCassette(int dy)
{
    // fade in step by step with the reels; scaled before gamma, so it looks even
    uint32_t elapsed = bootElapsed();
    int step = elapsed / REEL_STEP_MS;
    const int fadeSteps = BOOT_MS / REEL_STEP_MS;  // fully lit when the bar is full
    float level = FADE_FROM + (1.0f - FADE_FROM) * min(step, fadeSteps) / fadeSteps;
    uint16_t cassetteColors[sizeof(cassettePalette) / 3];
    for (size_t i = 0; i < sizeof(cassettePalette) / 3; i++)
    {
        uint8_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = lroundf(powf(cassettePalette[i][k] * level / 255.0f, GAMMA) * 255);
        cassetteColors[i] = rgb(c[0], c[1], c[2]);
    }

    for (int y = 0; y < LED_ROWS; y++)
        for (int x = 0; x < LED_COLS; x++)
        {
            char c = cassette[y][x];
            if (c == '.') continue;
            if (c == 'D' && x - loadX < (int)(elapsed * loadW / BOOT_MS)) c = 'C';  // loaded part
            matrix.drawPixel(x, y + dy, cassetteColors[c - 'A']);
        }

    // one dark pixel circling each hub reads as rotation
    static const uint8_t dx[] = {0, 1, 1, 0}, dyHub[] = {0, 0, 1, 1};
    int phase = step % 4;
    for (int x : reelX) matrix.drawPixel(x + dx[phase], reelY + dyHub[phase] + dy, colorOff);
}

static void sampleAudio()
{
    const uint32_t period = 1000000UL / SAMPLE_HZ;
    uint32_t start = micros(), next = start;
    int32_t sum = 0;

    for (int i = 0; i < FFT_N; i++)
    {
        while ((int32_t)(micros() - next) < 0)
        {
        }
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
#define HOSTNAME "vinyl-matrix"  // reachable as vinyl-matrix.local
#define NP_PORT 4210
#ifdef IDLE_LOG
#define LOG_PORT 4211  // idle spikes and a heartbeat, broadcast; see the README to listen
#define LOG_BEAT_MS 60000
#endif
#define NP_TIMEOUT_MS 30000  // sender repeats every 10 s; drop the view after this
#define HELLO_MS 2000        // until snapserver-lcd answers, ask it for the state this often
#define VINYL_HOLD_MS 3000   // silence needed before handing the panel to spotify
#define ART_SIZE 16
#define TEXT_MAX 128
#define SCROLL_PX_PER_S 15
#define SCROLL_HOLD_MS 1500    // pause at the start of each marquee cycle
#define SCROLL_GAP 8           // blank pixels between marquee repeats
#define TEXT_X (ART_SIZE + 1)  // one blank column between artwork and text
#define TITLE_BASELINE 15      // 3x5 font: caps on rows 10-14, descenders on the bottom row
#define ARTIST_BASELINE 9      // one font line (6 px) above: caps on rows 4-8

U8G2_FOR_ADAFRUIT_GFX text;

struct Track
{
    uint32_t artistSince, titleSince;  // marquee starts
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
    enum
    {
        FROM_BLANK,
        FROM_CASSETTE,
        FROM_TRACK,
        FROM_TITLE  // same album: only the title changes
    } slideFrom;
    Track track, previous;  // previous is only drawn while it slides out
} np;

WiFiUDP udp;
uint8_t packet[1200];
uint8_t artGamma[256];
uint32_t lastVinyl = -VINYL_HOLD_MS;  // no vinyl yet, so a track can take over right at boot
bool mdnsStarted = false;
bool heardFromPi = false;

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
    heardFromPi = true;

    if (packet[3] == 'S')
    {
        np.active = false;
        return;
    }
    if (packet[3] != 'P' || len < 4 + artBytes + 3 + 2) return;

    // both strings must be terminated inside the packet
    const uint8_t* p = packet + 4;
    const char* artist = (const char*)p + artBytes + 3;
    const char* end = (const char*)packet + len;
    const char* title = (const char*)memchr(artist, 0, end - artist);
    if (!title || !memchr(++title, 0, end - title)) return;

    uint16_t art[ART_SIZE * ART_SIZE];
    for (int i = 0; i < ART_SIZE * ART_SIZE; i++, p += 3)
        art[i] = rgb(artGamma[p[0]], artGamma[p[1]], artGamma[p[2]]);

    // no album in the packet, but its tracks share the cover, which downscales identically
    Track& t = np.track;
    bool artistChanged = strncmp(artist, t.artist, TEXT_MAX);
    bool titleChanged = strncmp(title, t.title, TEXT_MAX);
    bool albumChanged = artistChanged || memcmp(art, t.art, sizeof(art));
    if ((artistChanged || titleChanged) && np.visible)
    {
        np.previous = t;
        np.slideFrom = albumChanged ? NowPlaying::FROM_TRACK : NowPlaying::FROM_TITLE;
        np.slideSince = millis();
    }
    if (albumChanged || !np.active) t.artistSince = millis();
    if (artistChanged || titleChanged || !np.active) t.titleSince = millis();

    memcpy(t.art, art, sizeof(art));
    t.colorArtist = rgb(p[0], p[1], p[2]);
    strlcpy(t.artist, artist, TEXT_MAX);
    strlcpy(t.title, title, TEXT_MAX);
    t.artistWidth = text.getUTF8Width(t.artist);
    t.titleWidth = text.getUTF8Width(t.title);

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
        uint32_t cycle = SCROLL_HOLD_MS + period * 1000 / SCROLL_PX_PER_S;
        uint32_t t = (millis() - since) % cycle;
        if (t > SCROLL_HOLD_MS) offset = (t - SCROLL_HOLD_MS) * SCROLL_PX_PER_S / 1000;
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

    matrix.show();
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
        const float binHz = (float)SAMPLE_HZ / FFT_N;
        sendLog("%lus spike: band %d (%d-%d Hz), %+.1f dB for %d frame%s, after filter %+.1f dB%s",
                spike.since / 1000, spike.band, (int)(bandEdge[spike.band] * binHz),
                (int)(bandEdge[spike.band + 1] * binHz), spike.raw, spike.frames,
                spike.frames == 1 ? "" : "s", spike.filtered,
                spike.filtered > 0.0f ? " -> SHOWN" : "");
        spike = Spike();
    }
}
#endif

// Draws the now playing view and returns true while Spotify plays and the
// turntable is silent. The ADC only hears the turntable, so vinyl always wins.
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
        udp.write((const uint8_t *)"NP\x04H", 4);
        udp.endPacket();
    }

    receiveNowPlaying();
    if (np.active && millis() - np.lastPacket > NP_TIMEOUT_MS) np.active = false;

    if (signalDb > 0.0f) lastVinyl = millis();
#ifdef IDLE_LOG
    logIdle();
#endif
    // waits for the boot animation to finish
    bool visible = np.active && !booting() && millis() - lastVinyl > VINYL_HOLD_MS;

    // appearing, the track slides in from the top, pushing the cassette out at boot;
    // if the tape is already on its way down, the track joins in at the same height
    if (visible && !np.visible)
    {
        np.slideFrom = cassetteShown ? NowPlaying::FROM_CASSETTE : NowPlaying::FROM_BLANK;
        np.slideSince = cassetteShown ? bootStart + BOOT_MS + BOOT_HOLD_MS : millis();
    }
    np.visible = visible;

    if (visible) drawNowPlaying();
    return visible;
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

    bootStart = millis();
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
    bool nowPlaying = updateNowPlaying();
    cassetteShown = false;
    if (nowPlaying) return;
#endif

    if (ejecting())
    {
        int dy = booting() ? 0 : lroundf(slideProgress(bootElapsed() - BOOT_MS - BOOT_HOLD_MS) * LED_ROWS);
        matrix.fillScreen(colorOff);
        drawCassette(dy);
        matrix.show();
        cassetteShown = true;
        return;
    }

    cassetteShown = false;
    draw();
}
