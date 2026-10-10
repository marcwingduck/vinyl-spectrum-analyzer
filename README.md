# Vinyl Spectrum Analyzer: LED Rulez D00d

A 32×16 LED spectrum analyzer that listens to the record player. Line-level audio is tapped after the phono preamp and run through an FFT on an Adafruit MatrixPortal S3, and the result is drawn as 32 bars on a HUB75 panel. The bars fade from sage to lilac, with peak dots that hang and sink.

It's a port of my earlier Arduino Uno build, [VisEQ](https://github.com/marcwingduck/viseq), which used a microphone.

## Hardware

- [Adafruit MatrixPortal S3](https://www.adafruit.com/product/5778)
- [Medium 16×32 RGB LED matrix panel](https://www.adafruit.com/product/420) (HUB75)
- ART DJ PRE II phono preamp (any line-level source works)
- 2× RCA Y splitter, 1 male → 2 female (Cordial CFY 0,3 CEE)
- 1× stereo RCA cable (20 cm is enough if the frame sits next to the preamp)
- 2× RCA chassis jack (REAN NYS367, black = L, red = R)
- Resistors: 2× 47k, 2× 100k, 1× 1k
- 1× capacitor, 1–10 µF

## Build and flash

The project uses [PlatformIO](https://platformio.org/). Dependencies (Adafruit Protomatter, arduinoFFT) are installed automatically.

```sh
pio run -t upload
pio device monitor   # optional, 115200 baud
```

## Wiring

Line-level audio is tapped from the preamp output and fed to the MatrixPortal's ADC on **A3 (GPIO10, ADC1_CH9)**. The hi-fi signal path stays untouched.

### Signal path

```
                         ┌──[female]◄── existing cable ──► Receiver IN
DJ PRE OUT ◄──[male] Y ──┤
  (L and R)              └──[female]◄── 20 cm RCA cable ──► [female jack] frame housing
```

Use one Y splitter per channel. To move the frame somewhere else, swap in a longer stereo RCA cable. Nothing else changes.

### Tap circuit (inside the frame)

```
L jack center ──[47k]──┐
                       │
R jack center ──[47k]──┤   sum L+R to mono
                       │
                      ═╪═  C1 1–10 µF  (+ side toward A3)
                       │
   3.3V ──[100k]───────●───────[1k]──── A3 (GPIO10)
                       │
   GND  ──[100k]───────┘

L jack shield ─┬────────────────────── GND (MatrixPortal)
R jack shield ─┘
```

`●` is the bias node: C1, both 100k resistors and the 1k all connect there. 3.3V and GND are the MatrixPortal's pins.

| Part | Value | Purpose |
|------|-------|---------|
| R1, R2 | 47k | Mix L+R. High enough that the tap doesn't cause crosstalk in the hi-fi path |
| C1 | 1–10 µF | Blocks the preamp's DC level |
| R3, R4 | 100k | Bias the signal to ~1.65 V, the middle of the ADC range |
| R5 | 1k | Series protection for the ADC pin |

With an electrolytic capacitor, put its **+** leg toward the bias node. A film capacitor has no polarity.

### Before connecting audio

With the board powered and no audio plugged in, A3 should read about **1.65 V** against GND. If it reads close to 0 V or 3.3 V, the bias divider or the A3 wire isn't connected properly.

### Notes

- **Stay on A3.** A0 and A4 are on ADC2, which doesn't work while WiFi is running.
- **Only tap after the preamp.** Never split the turntable's phono signal.
- **Hum:** if the hi-fi buzzes in step with the LEDs, power the MatrixPortal from a separate USB wall charger. If that doesn't help, add an RCA ground-loop isolator.

## Tuning

All settings are `#define`s at the top of [src/main.cpp](src/main.cpp).

| Setting | Default | What it does |
|---------|---------|--------------|
| `BRIGHTNESS` | 100 | Peak channel value, 0–255 |
| `ATTACK_MS` | 25 | How fast bars rise. Lower it for punchier beats |
| `RELEASE_MS` | 140 | How slowly bars sink. Raise it for calmer bars |
| `PEAK_FALLOFF_MS` | 500 | How long a peak dot hangs before sinking |
| `SILENCE_FLOOR` | 0.16 | Below this level the bars stay dark. Raise it if bars twitch during silence |

The colors are the `sage` and `lilac` RGB values in `setup()`.

**Input gain:** the samples are divided by `8.0f` in `sampleAudio()`. If the bars drop out in quiet passages, lower it. If they're at full height most of the time, raise it, or turn down the preamp output.

## License

[MIT](LICENSE). The FFT library it builds against, [arduinoFFT](https://github.com/kosme/arduinoFFT), is GPL-3.0.
