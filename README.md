# Vinyl Spectrum Analyzer: LED Rulez D00d

A 32×16 LED spectrum analyzer that listens to the record player. Line-level audio is tapped after the phono preamp and run through an FFT on an Adafruit MatrixPortal S3, and the result is drawn as 16 bars on a HUB75 panel, spaced like a real analyzer: one bar per 47 Hz in the bass, widening logarithmically up to 6 kHz. The bars fade from sage to lilac, with peak dots that hang and sink.

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

The project uses [PlatformIO](https://platformio.org/). Dependencies (Adafruit Protomatter, arduinoFFT, U8g2_for_Adafruit_GFX) are installed automatically.

```sh
pio run -t upload
pio device monitor   # optional, 115200 baud
```

That builds the plain spectrum analyzer. For the optional [now playing view](#now-playing-from-spotify-optional), add WiFi credentials before uploading:

```sh
cp src/secrets.h.example src/secrets.h   # git-ignored; fill in SSID and password
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

## Now playing from Spotify (optional)

Only built when `src/secrets.h` exists, see [Build and flash](#build-and-flash). When the turntable is silent, the panel can show what the [snapserver](https://github.com/marcwingduck/snapserver-lcd) is streaming: the album art at 16×16 on the left, with artist and title scrolling on the right in U8g2's 3×5 `tom_thumb` font, which includes umlauts. Paused or stopped means nothing is playing, so the panel goes blank.

The board joins WiFi as `vinyl-matrix.local` and listens on UDP port 4210. snapserver-lcd sends one datagram per change and repeats it every 10 s. Once on WiFi, the board broadcasts a hello, which snapserver-lcd answers right away, so a track that is already playing shows up as soon as the boot animation ends. If nothing arrives for 30 s, or a stop packet comes in, the panel returns to the spectrum. If the board never connects, the spectrum runs as before.

Vinyl always wins. The ADC only hears the turntable, so any band above its noise floor switches to the spectrum straight away. The panel goes back to Spotify only after `VINYL_HOLD_MS` of silence.

Datagram layout (version 4). Everything after byte 3 is only sent with `P`:

| Bytes | Content |
|-------|---------|
| 0–1 | `NP` |
| 2 | version, `4` |
| 3 | `P` show, `S` stop; `H` hello, broadcast by the board until the sender answers |
| 4–771 | 16×16 artwork, RGB888, row-major |
| 772–774 | accent color RGB, used for the artist line |
| 775… | artist `\0` title `\0`, UTF-8, Latin-1 characters only |

## Tuning

All settings are `#define`s at the top of [src/main.cpp](src/main.cpp).

| Setting | Default | What it does |
|---------|---------|--------------|
| `ATTACK_MS` | 25 | How fast bars rise. Lower it for punchier beats |
| `RELEASE_MS` | 140 | How slowly bars sink. Raise it for calmer bars |
| `PEAK_FALLOFF_MS` | 500 | How long a peak dot hangs before sinking |
| `NOISE_FLOOR_DB` | -28 | Per-bin noise level. Each bar only shows what rises above it, so silence stays dark and quiet passages still show. See below |
| `DISPLAY_DB` | 30 | Range the bars cover below the recent loudest band. Lower it for livelier bars |
| `SAMPLE_HZ` | 12000 | `analogRead()` takes ~75 µs, so ~13 kHz is the ceiling. The serial monitor shows the rate actually reached |
| `ART_GAMMA` | 2.2 | Gamma correction for the artwork. Lower it if dark covers lose detail |
| `SCROLL_PX_PER_S` | 15 | Marquee speed of artist and title |
| `VINYL_HOLD_MS` | 3000 | Silence needed before Spotify takes the panel back |

The colors are the `sage` and `lilac` RGB values in `setup()`. Everything runs at full brightness: dimming in software costs color depth (dark tones flicker and posterize), so tame the panel with a tinted film or smoked acrylic in front instead.

**Calibrating the noise floor:** run `pio device monitor`. Once a second the board prints how far the loudest band is above its noise floor. With the needle up it should stay a few dB below 0. If it reaches 0, raise `NOISE_FLOOR_DB` by that much plus ~3 dB, since the bars and the Spotify handover would otherwise react to noise. If it sits far below, lower the floor so quiet passages show more.

**Stray peaks while idle:** with WiFi set up, the board needs no cable for this. Uncomment `#define IDLE_LOG` at the top of [src/main.cpp](src/main.cpp), flash, and listen on any machine in the network (macOS `nc -ul` stops after the first line):

```sh
python3 -c 'import socket,time;s=socket.socket(2,2);s.bind(("",4211));[print(time.strftime("%H:%M:%S"),d.decode(),end="",flush=True) for _ in iter(int,1) if (d:=s.recv(512))]'
```

Every minute the board broadcasts a heartbeat, and whenever a band rises above its noise floor while the turntable is silent, it sends one line: which band, how loud, how many frames, and how loud after the 3-frame median that hides one-frame interference. `-> SHOWN` marks the ones that made it onto the panel.

## License

[MIT](LICENSE). The FFT library it builds against, [arduinoFFT](https://github.com/kosme/arduinoFFT), is GPL-3.0.
