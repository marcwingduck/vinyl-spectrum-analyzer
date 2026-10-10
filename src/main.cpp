#include <Adafruit_Protomatter.h>
#include <Arduino.h>
#include <arduinoFFT.h>

#define LED_COLS 32
#define LED_ROWS 16

// GPIO10 / ADC1_CH9. A0 and A4 sit on ADC2, which stops working once WiFi runs.
#define AUDIO_PIN A3

#define FFT_N 64        // 32 bins, one per column
#define SAMPLE_HZ 9000  // ~4.5 kHz span, matching the old AVR analogRead pacing
#define BIN_WIDTH 2     // columns sharing one band, as the sketch booted on AVR

#define BRIGHTNESS 100       // peak channel value, 0-255
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

    colorOff = matrix.color565(0, 0, 0);
    // soft sage at the bottom fading into lilac on top; RGB at full scale,
    // dimmed by BRIGHTNESS
    const uint8_t sage[3] = {80, 200, 90}, lilac[3] = {190, 70, 255};
    for (int j = 0; j < LED_ROWS; j++)
    {
        uint32_t t = (uint32_t)j * 255 / (LED_ROWS - 1);
        uint8_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = (sage[k] * (255 - t) + lilac[k] * t) / 255 * BRIGHTNESS / 255;
        colorRow[j] = matrix.color565(c[0], c[1], c[2]);
    }
    colorPeak = matrix.color565((uint32_t)lilac[0] * BRIGHTNESS / 255,
                                (uint32_t)lilac[1] * BRIGHTNESS / 255,
                                (uint32_t)lilac[2] * BRIGHTNESS / 255);

    memset(mmValues, 0, sizeof(mmValues));
    resetPeaks();
}

void loop()
{
    sampleAudio();
    runFFT();
    draw();
}
