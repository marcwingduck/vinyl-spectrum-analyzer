# Vinyl Spectrum Analyzer: LED Rulez D00d

A 32×16 LED spectrum analyzer that listens to the record player. Line-level audio is tapped after the phono preamp and run through an FFT on an Adafruit MatrixPortal S3, and the result is drawn as 16 bars on a HUB75 panel, spaced like a real analyzer: one bar per 39 Hz in the bass, widening logarithmically up to 10 kHz. The bars fade from sage to lilac, with peak dots that hang and sink.

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

The project uses [PlatformIO](https://platformio.org/). Dependencies (ESP32 HUB75 LED Matrix Panel DMA, Adafruit GFX, arduinoFFT, U8g2_for_Adafruit_GFX) are installed automatically. The panel is refreshed by DMA, without the CPU, so WiFi traffic doesn't disturb it. To check a new panel's mapping, flash a still test image with `PLATFORMIO_BUILD_FLAGS="-DPANEL_TEST" pio run -t upload`: a white border, red top left, green top right, blue bottom left, yellow bottom right, a diagonal from the top left. Everything one pixel to the left, the left column wrapped to the right, means the panel latches on the other clock edge: flip `clkphase` in `matrixConfig()`.

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

Vinyl always wins. The ADC only hears the turntable, so a band rising `VINYL_DB` (6 dB) above where its bars start switches to the spectrum straight away. The extra margin keeps wandering mains hum from taking the panel from Spotify. The panel goes back to Spotify only after `VINYL_HOLD_MS` of silence.

Datagram layout (version 4). Everything after byte 3 is only sent with `P` and `V`:

| Bytes | Content |
|-------|---------|
| 0–1 | `NP` |
| 2 | version, `4` |
| 3 | `P` show, `S` stop; `H` hello, broadcast by the board until the sender answers; `V` the record on the turntable, see below |
| 4–771 | 16×16 artwork, RGB888, row-major |
| 772–774 | accent color RGB, used for the artist line |
| 775… | artist `\0` title `\0`, UTF-8, Latin-1 characters only |

`V` has 6 more bytes before the strings: the bars' bottom and top color RGB at 775–780, all zero to keep sage and lilac. The strings start at 781.

## Recognizing the record (optional)

With WiFi set up, the board can tell [groove-id](../groove-id) on the snapserver Pi what's on the turntable. It fingerprints a 10 s clip against the MP3s of the records in your Discogs collection. When a new track is recognized, its cover and title slide in from the top, pushing the bars down, and after 8 s the bars push them back out. Until the turntable goes silent, the bars take the album's colors and the peaks its accent.

The board decides when a clip is worth it. A needle drop counts as a new track, and so does a gap: the level falling `GAP_DB` below the recent loud level for `GAP_MS` and coming back. The groove still crackles between tracks, so the level doesn't fall all the way to the noise floor. `CLIP_DELAY_MS` into the track, the board broadcasts `clip needle` or `clip gap` on UDP port 4213. Sides that run from track to track without a gap get a `clip recheck` every `RECHECK_MS`. groove-id then connects to TCP port 4212 and gets the next 10 s as raw 16-bit mono at `SAMPLE_HZ`, 400 kB from PSRAM. The board records first and sends in one burst of about half a second, so WiFi disturbs the ADC once. It sends from a task on the other core, so the bars keep moving meanwhile, and the panel is refreshed by DMA, so the burst doesn't make it flicker. groove-id answers every recognition with a `V` datagram, and only a new track pops in. Once the turntable has been silent for `VINYL_HOLD_MS`, the board broadcasts `stop` on the same port, so snapserver-lcd's LCD drops the record right away instead of at the track's end.

Without groove-id nothing changes: nobody fetches clips, and the broadcasts go unanswered.

## Tuning

All settings are `#define`s at the top of [src/main.cpp](src/main.cpp).

| Setting | Default | What it does |
|---------|---------|--------------|
| `ATTACK_MS` | 25 | How fast bars rise. Lower it for punchier beats |
| `RELEASE_MS` | 140 | How slowly bars sink. Raise it for calmer bars |
| `PEAK_FALLOFF_MS` | 500 | How long a peak dot hangs before sinking |
| `bandNoiseDb` | measured | Default for each band's level with the needle up, until you calibrate with the UP button. Bars only show what rises above it, so silence stays dark and quiet passages still show. See below |
| `NOISE_MARGIN_DB` | 6 | How far above its noise a band has to rise to show. Raise it if bars twitch with the needle up |
| `DISPLAY_DB` | 20 | Range the bars cover below the top of the scale. Lower it for lower, livelier bars; raise it for fuller ones |
| `SCALE_DECAY_DB_S` | 6 | The top of the scale follows the loudest band: up at once, down this fast. The loudest moment just reaches the top, and nothing is cut off there. Slower keeps the bars lower after a loud hit; faster brings them back up sooner. An average as the top (the old way) cut off every peak above it, so the bass bands stuck to the ceiling |
| `SAMPLE_HZ` | 20000 | FFT sample rate, so the bars reach `SAMPLE_HZ / 2`. The ADC runs at `OVERSAMPLE` times this on its own timer (DMA), up to 83 kHz |
| `OVERSAMPLE` | 4 | ADC samples averaged per FFT sample. Each doubling takes ~3 dB off random noise, not off hum |
| `GAMMA` | 2.2 | Gamma correction for the artwork. Lower it if dark covers lose detail |
| `SCROLL_MS_PER_PX` | 70 | Marquee speed of artist and title, ~14 px/s. Rounded to whole frames (one audio block each, ~21 ms at the default `FFT_N` and `SAMPLE_HZ`) so every step is equally long |
| `VINYL_HOLD_MS` | 3000 | Silence needed before Spotify takes the panel back. Also ends a recognized record, so its colors go |
| `GAP_DB` | 15 | How far the level has to fall below the recent loud level to count as a gap between tracks. Lower it to catch tracks that run into each other, at the risk of quiet passages asking for clips |
| `GAP_MS` | 1000 | How long a gap has to last |
| `CLIP_DELAY_MS` | 4000 | How long into a track before asking for a clip, past quiet intros |
| `RECHECK_MS` | 120000 | Clip interval when no gap shows up, for sides without pauses |
| `POP_MS` | 8000 | How long a recognized track stays before the bars return. Long names stretch it to `POP_MAX_MS` (15000) so they scroll through once |

The colors are the `sage` and `lilac` RGB values in `setup()`. `BRIGHTNESS` (0–255, 180 by default) dims the panel in hardware, by shortening how long the LEDs are on, so moderate dimming keeps the color depth; very low values lose the darkest tones. A tinted film or smoked acrylic in front works too.

**Calibrating the noise floor:** the noise isn't even across bands. Mains hum puts 50 Hz and its harmonics well above the rest, so each band has its own floor. Calibrate wherever the board will run, since the hum depends on cables and power supply: lift the needle, then hold the **UP** button for 2 seconds. A progress bar runs for 15 seconds while each band is measured. The result is stored in flash and survives power cuts and firmware updates, unless an update changes the bands (`SAMPLE_HZ`, `FFT_N`, `BAND_LOW_HZ`), which needs a new calibration. So does a change to the panel driver or its refresh settings: the panel's refresh couples into the ADC, and switching from Protomatter to the DMA library raised the idle level by about 9 dB, so the outer bands stayed lit until a recalibration. Until the first calibration, the defaults in `bandNoiseDb` apply. With the needle up, `pio device monitor` should then show `above noise` around `-NOISE_MARGIN_DB`.

**Stray peaks while idle:** with WiFi set up, the board needs no cable for this. Uncomment `#define IDLE_LOG` at the top of [src/main.cpp](src/main.cpp), flash, and listen on any machine in the network (macOS `nc -ul` stops after the first line):

```sh
python3 -c 'import socket,time;s=socket.socket(2,2);s.bind(("",4211));[print(time.strftime("%H:%M:%S"),d.decode(),end="",flush=True) for _ in iter(int,1) if (d:=s.recv(512))]'
```

Every minute the board broadcasts a heartbeat, and whenever a band rises above its noise floor while the turntable is silent, it sends one line: which band, how loud, how many frames, and how loud after the 3-frame median that hides one-frame interference. `-> SHOWN` marks the ones that made it onto the panel. It also reports where the noise floor came from (stored calibration or defaults) after boot and after each calibration, and every handover between Spotify and the spectrum, with the band that triggered it.

## License

[MIT](LICENSE). The FFT library it builds against, [arduinoFFT](https://github.com/kosme/arduinoFFT), is GPL-3.0.
