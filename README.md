# NO SIGNAL — a mini static-noise TV for the M5StickS3

[中文](README_CN.md) · **English**

**A 240×135 CRT stuck forever on a dead channel.** Amber phosphor snow that breathes, a permanent `NO SIGNAL` sign, 3,864 glitch scenes (14 snow × 12 text × 23 warps) — and **slap it**: half the time a fake TV world locks in out of the static (38 of them, each with its own sound), half the time two bright scan lines roll down. Leave it alone for five minutes and it quietly turns itself into a clock.

> *Not a screensaver. A small appliance that is always almost broken.*

| Normal: amber snow + the `NO SIGNAL` sign | A locked-in world: station ID `CH 42` |
|---|---|
| ![Amber phosphor snow with the NO SIGNAL sign in the middle of the 240x135 screen](docs/images/sticks3-normal.jpg) | ![A fake TV program with a mountain silhouette and a CH 42 station ID in the top right corner](docs/images/sticks3-program-ch42.jpg) |

*Both images are real 240×135 framebuffer captures from a v0.1.0 build, upscaled 4× nearest-neighbour. They are screen captures, not photographs — in a dark room the device looks warmer and dimmer than this.*

---

## What it is

A desk ambient piece and, on sleepless nights, a white-noise + gaze companion. Two buttons, no menus, no configuration file: everything it does is a probability distribution with a long tail.

| | |
|---|---|
| **Snow engine (14 variants)** | coarse / fine / split-tier / breathing / clumping / wind-drift / dark field / vertical-bar interference / warm-channel cast / blown-out white / sparse / dense … |
| **Sign text (12 variants)** | typewriter reveal / colour bars / callsign / double exposure / split top-bottom / melt / pixel shatter / jitter / blink / persistent clock / full garbage … |
| **Warp / distortion (23 variants)** | h-sync slant / v-roll / curl / shake / antenna jitter / ripple / beam-discharge arc / barrel & pincushion / mirror / splice / zoom / invert / vignette / letterbox / focus drift / multipath ghost / tearing / flip / channel search / fake power cut / fade / black band … |
| **Glitch scenes** | **14 × 12 × 23 = 3,864** combinations, firing every 40–150 s |
| **Special events** | channel change / signal-recovery hallucination / telephone interference (real DTMF tones) / loose antenna / ghost broadcast / **alien rune marquee** (every 20–110 s) |
| **Breathing layer** | five coprime slow LFOs underneath everything — brightness 9 s, vignette walk 41 s, row-creep 83 s, ageing band 197 s, field phase noise 17 s; the depth itself random-walks every 25–70 s, and the whole picture periodically decides to "come up for air" or go pathological (60–180 s checks, 25 % trigger) |
| **Slap-to-signal** | tap the device → **50 % a fake 6 s TV program / 50 % scan lines**. Program = 38 subjects × 6 backgrounds × 12 celestial bodies × 8 station-ID styles × 3 textures × 3×3×3 motion axes |
| **38 world channels** | 14 nature (aurora / lightning plain / dunes …) · 8 human wonders (pyramids / Stonehenge / Great Wall / Parthenon / Angkor Wat / Mt. Fuji torii / moai / mosque) · 4 mythic (cloud dragon / sky whale / feathered serpent / tree spirit) · 12 sci-fi (cyclops / UFO mothership / shuttle / rocket pad / satellite / moon base / alien city / robot / lander / monolith / space station / totem) |
| **Recency memory** | subjects never repeat within 4 slaps, backgrounds within 2, celestial bodies within 2 — the pool is much larger than it feels |
| **Rare in-program events** | 20 % of programs: Philips-style test card / full colour bars / grey staircase / teletext / mosaic / alien alphabet / reversed signal / radiation storm; plus an even rarer easter-egg pool |
| **Sound axis** | 22 kHz white-noise engine whose volume tracks what the picture is doing (fake power cut → silence + thump, bright noise burst, fade-out, channel-search sweep). Each program family has its own event sound: **sparse Geiger ticks** (nature/human), **dense Geiger** (sci-fi), **alien dial-up** (mythic), **Morse SOS**, **mechanical hum + crackle** — opened by a dense Geiger burst as the signal locks in, and cut dead when the program ends |
| **Clock power save** | 5 minutes idle → big HH:MM clock, **1 fps + Light Sleep between frames**, IMU and audio hardware off. Any button wakes it in <100 ms. **USB power deliberately never sleeps** |
| **Time sync** | first boot (or splash → A) shows an AP + on-screen QR page; the phone opens `http://192.168.4.1/` and posts the time. Press **A or B to skip**, or wait 60 s |
| **Boot animation** | black screen → a bright line in the middle → it unfolds into snow |

## Status (what actually works)

| Module | State | Notes |
|---|---|---|
| Snow engine + glitch pool | ✅ works | 3,864 combinations, 40–150 s apart, no repeats by design |
| Breathing layer | ✅ works | five coprime periods; verifiable numerically (see below) |
| Slap → program | ✅ works | BMI270, 1.8 g, two-frame confirmation, 3 s cooldown |
| Sound axis | ✅ works | v0.1.0 wired the four event profiles that were dead code in v0.0.16 |
| Station IDs / callsigns | ✅ works | needs the 7 glyphs added in v0.1.0 (`H W P Q X Z K`) — before that `CH 5` rendered as `C 5` |
| Clock power save | ✅ works | 5 min → 1 fps + Light Sleep; `BtnA`/`BtnB` GPIO wake; USB power exempts it |
| Brightness memory | ✅ works | six levels (5/10/30/50/80/100 %), stored in NVS |
| Time sync | ⚠️ manual | AP + QR page only — there is no touchscreen, so the device cannot host a real config page. Skippable |
| Sound *tuning* | ⚠️ subjective | profiles are chosen by subject family; no volume control beyond the mute key |
| OTA updates | ❌ not implemented | flash over USB (or M5Burner) |
| Configuration file / SD card | ❌ not implemented | every probability lives in the firmware |

## Hardware

| | |
|---|---|
| Board | **M5Stack StickS3** — ESP32-S3, 8 MB flash, 8 MB octal PSRAM |
| Display | 1.14″ 240×135 ST7789 (landscape), drawn into an `LGFX_Sprite` double buffer |
| Sensors | BMI270 IMU (the slap), ES8311 audio codec (the noise) |
| Buttons | `BtnA` (left, GPIO11) · `BtnB` (right, GPIO12) |
| Power | 250 mAh internal battery, USB-C |

### Controls

| Action | Effect |
|---|---|
| **Slap the device** | 50 % fake 6 s program / 50 % scan lines; slap again during a program = next channel (3 s cooldown) |
| **BtnB** | brightness: 5 / 10 / 30 / 50 / 80 / 100 % (written to NVS immediately) |
| **BtnA** (running) | mute / unmute |
| **BtnA** (splash, first 3 s) | enter the QR time-sync page |
| **BtnA / BtnB** (in power save) | wake up — and nothing else: that keypress is swallowed on purpose |

## Quick start

```bash
git clone https://github.com/andjiang0083/no-signal.git
cd no-signal
pio run -e sticks3-prod                 # release build (no WiFi, no harness, no credentials)
pio run -e sticks3-prod -t upload       # flash over USB
```

Download mode: hold **KEY1 (BtnA)** while tapping the **RST** button on the back, then flash.
The development build, the test harness and the troubleshooting table are in
[BUILDING.md](BUILDING.md).

### Install without a toolchain (M5Burner)

Grab `no-signal-v0.1.0-merged.bin` from the Releases page — M5Burner → **Custom** →
**Import Custom FW** → pick the file (or the `no-signal.json` manifest in `m5burner/`).
**Always the *merged* image**: M5Burner always writes at flash offset 0x0, so the app-only
`no-signal-v0.1.0.bin` will not boot when flashed that way (it is for `esptool`/OTA).

```bash
shasum -a 256 -c no-signal-v0.1.0-merged.bin.sha256      # da6bea2d…712b12
```

## How the picture is made

There is no image data in this repository — no bitmaps, no spritesheets, no GIFs. Every frame is
generated at runtime into a 240×135 RGB565 buffer:

1. **Snow** is a decaying phosphor field (`g_phos`); each frame injects new points, and each of
   the 14 variants changes the injection rate, the decay curve and the palette ramp.
2. **The sign** is drawn with a 37-glyph hand-made 5×7 bitmap font, straight into the
   framebuffer (never through `drawString`, which would double-apply the coordinate transform).
3. **The warp** is a full-frame post-process (23 of them): row shifts, field roll, curl, ripple,
   arcs, barrel/pincushion, mirror, splice, zoom, invert, vignette, letterbox, ghosting, tearing,
   fake power cut …
4. **The breathing layer** modulates the result — brightness envelope, walking vignette, row
   creep, ageing band, field-phase perturb — all on coprime periods, so the picture never loops.
5. **Programs** are drawn the same way: a primitive library (`prog_shapes.h`) plus per-subject
   silhouette routines, then the station ID, then the sound.

The framebuffer is pushed to the panel once per frame with `pushSprite` (atomic, no flicker), at
15 fps — or 1 fps in clock power save.

## Why this repo has no unit tests

Because the interesting behaviour is a 240×135 picture and a speaker. The development loop used
while building this is a **remote test harness** that only exists in the dev build
(`pio run -e sticks3`): an HTTP server that serves the *actual framebuffer* as a BMP, injects
button presses, and accepts JSON to pin the random generators to one scene, one station ID, one
warp or one sound profile. The PC side is `tools/control.py`.

That harness is how v0.1.0 was verified: the "snow breathing" fix was confirmed by capturing 16
consecutive frames and showing the mean frame luminance swings by 3.0 units instead of 0.5, and
the strong-tear fix by drawing deterministic dots into the frame and measuring how far each band
shifted (−20 px / +12 px). The measurements are written up in
[`docs/VERIFY-v0.1.0.md`](docs/VERIFY-v0.1.0.md), together with the audit that produced them
([`docs/CODE-REVIEW-v0.0.16.md`](docs/CODE-REVIEW-v0.0.16.md) — 18 findings: 1 P0, 8 P1, 9 P2).

Serial-console debugging is deliberately *not* the workflow here: the power-save path uses Light
Sleep, which detaches USB CDC, and backlight changes never touch the framebuffer — so a large
part of this firmware is simply not observable over a serial port.

## Known limits

- **No touchscreen and two buttons** — this constrains every UI decision in the project.
- **No OTA, no config file**: to change a probability you edit the firmware.
- **The clock takes whatever time it is given** by the phone during sync and keeps it in the RTC.
- **Sound is a 22 kHz white-noise engine with tone layering**, not a sample player; the profiles
  are deliberately synthetic.
- The palette is amber-only, and everything you can see is either a primitive or a glyph.

## Contributing

Contributions are very welcome — especially new world subjects, new warps and new sound ideas.
Read [CONTRIBUTING.md](CONTRIBUTING.md) first: this project prefers small patches to proven
code, evidence-based verification, and Chinese comments in the source (the author's language).
[ROADMAP.md](ROADMAP.md) lists the concrete open items, including the three performance/refactor
tasks that came out of the v0.0.16 audit.

## Credits & licence

- Bundled: [`src/qrcode.h`](src/qrcode.h) — MIT, © 2017 Richard Moore and © 2017 Project Nayuki.
- Build dependencies: M5Unified / M5GFX (MIT, M5Stack), Arduino-ESP32 (LGPL-2.1, Espressif),
  ESP-IDF (Apache-2.0, Espressif), PlatformIO (Apache-2.0).
- Everything else — engine, artwork, font, palettes, sound design — is this repository's own work.
- Full third-party notices: [CREDITS.md](CREDITS.md).

**Licence: MIT** (see [LICENSE](LICENSE)) — © 2026 andjiang0083.

M5Stack, StickS3 and ESP32 are trademarks of their respective owners. This is an unaffiliated
community project; it is not endorsed by or connected to M5Stack.
