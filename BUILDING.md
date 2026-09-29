# Building, flashing and testing NO SIGNAL

[中文](BUILDING_CN.md) · **English**

Everything here is PlatformIO + the Arduino core for ESP32. No ESP-IDF project, no CMake
wrangling, one `pio run`.

## 1. Prerequisites

| Need | Why |
|---|---|
| [VS Code](https://code.visualstudio.com/) + the [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension | the normal way to build/upload |
| or the PlatformIO CLI (`pip install platformio`) | `pio run -e sticks3-prod -t upload` from a terminal |
| Python 3 with `pillow` (**only** for the dev harness client `tools/control.py`) | `pip install pillow` |
| An M5Stack **StickS3** | ESP32-S3, 8 MB flash, 8 MB octal PSRAM. A StickC will not work — different flash mode, different buttons |
| A USB-C cable that carries data | charge-only cables are a classic 20-minute debugging session |

The first build downloads the ESP32 platform, the Arduino core and M5Unified (a few hundred MB).
That is normal and only happens once.

**Dependencies are pinned** (`platformio.ini`): M5Unified `0.2.21` and M5GFX `0.2.28`. The v0.1.0
binaries on the Releases page were built with exactly those two, against Arduino core `3.3.9` /
ESP-IDF `5.5.4` / the xtensa-esp-elf `14.2.0` toolchain. Leaving the libraries unpinned means a
clone picks up whatever is newest that day — the first clone of this repository resolved M5Unified
`0.2.23` / M5GFX `0.2.30` and produced a binary 656 bytes different from the published one, with
different section sizes. Bump the pins deliberately, in their own commit, and see §11.

## 2. The two environments

| Env | Command | What it is |
|---|---|---|
| **dev** | `pio run -e sticks3` | Wi-Fi STA + the HTTP test harness + serial credentials from `src/config.h`. What you flash while developing |
| **release** | `pio run -e sticks3-prod` | `-DPRODUCTION_BUILD=1`: the harness, the STA credentials and the `/hrns/*` routes are compiled out. Wi-Fi itself is **kept on purpose** for the first-boot QR time sync |

Both share every other flag: `board_build.flash_mode = dio`, 8 MB flash,
`board_build.partitions = default_8MB.csv`, `board_build.arduino.memory_type = qio_opi`
(PSRAM in OPI mode — independent of the flash mode), 240 MHz.

> ⚠️ **`dio` is not a preference.** The StickS3 does not boot with a QIO bootloader; if you change
> `flash_mode`, clean the build (`pio run -t clean`) so the bootloader is recompiled, or you get a
> black screen with a perfectly valid-looking binary.

## 3. Dev credentials

The dev build connects to your LAN so the harness is reachable:

```bash
cp src/config.h.example src/config.h
$EDITOR src/config.h          # your 2.4 GHz SSID and password
```

`src/config.h` is git-ignored. The release build never compiles it — verify whenever you are
unsure:

```bash
strings -a .pio/build/sticks3-prod/firmware.bin | grep -c '<your-ssid>'    # must print 0
```

## 4. Flash

```bash
pio run -e sticks3-prod -t upload                      # auto-detects the port
pio run -e sticks3-prod -t upload --upload-port /dev/cu.usbmodem101
pio device monitor -e sticks3 -b 115200                # dev build only, see §8
```

**Download mode** (when a normal upload cannot connect): hold **KEY1 (BtnA)** while tapping
**RST** on the back, keep holding KEY1, start the upload, release when it finishes. This is
mandatory after the device has been in power save: Light Sleep detaches USB CDC, so an automatic
reset does not reach the chip.

## 5. Making an M5Burner image

PlatformIO already produces the merged image — do **not** hand-merge unless you must:

```bash
pio run -e sticks3-prod
cp .pio/build/sticks3-prod/firmware.factory.bin  releases/no-signal-<version>-merged.bin
cp .pio/build/sticks3-prod/firmware.bin          releases/no-signal-<version>.bin
shasum -a 256 releases/no-signal-<version>*.bin  # attach these to the GitHub release
```

Then verify the layout before publishing (this is cheap and catches the classic mistakes):

```bash
python3 - <<'PY'
d = open('releases/no-signal-<version>-merged.bin','rb').read()
print('header:', ' '.join(f'{b:02x}' for b in d[:4]), '  # e9 03 02 3f = ESP32-S3 + DIO')
for name, off, want in [('bootloader',0x0,0xe9), ('partitions',0x8000,0xaa),
                        ('boot_app0',0xe000,0x01), ('app',0x10000,0xe9)]:
    print(f'{name:>10} @ {off:#07x} magic=0x{d[off]:02x} {"OK" if d[off]==want else "BAD"}')
print('size', len(d), 'bytes')
PY
```

Cross-check against what PlatformIO itself writes to the chip:

```bash
pio run -e sticks3-prod -t upload -v 2>&1 | grep write-flash
# ... write-flash -z --flash-mode dio --flash-freq 80m --flash-size detect
#     0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

The merged image must agree with those offsets and that flash mode. **M5Burner always writes at
0x0**, which is exactly why the app-only `*.bin` must never be handed to it.

M5Burner import: **Custom → Import Custom FW →** pick `m5burner/no-signal.json` (the manifest
references the merged bin that must sit next to it) or the merged bin directly.

## 6. The test harness (dev builds only)

```
GET  /hrns/bmp     -> the real framebuffer as a 54-byte-header 16-bit BMP
GET  /hrns/debug   -> {"uptime":…,"free_heap":…,"min_free_heap":…,"psram_free":…,"btn_a_pending":…}
POST /hrns/btn     -> inject a button press  ({"btn":"a"} / {"btn":"b"} / {"btn":"a","long":1})
POST /hrns/data    -> JSON switches into the scene generators, see below
```

```bash
python3 tools/control.py <device-ip> snap       # save a screenshot next to you
python3 tools/control.py <device-ip> debug      # heap + uptime
python3 tools/control.py <device-ip> data '{"nosig":1}'
curl -s http://<device-ip>/hrns/bmp | wc -c     # must be exactly 64854
```

The injection switches exist so that a reviewer can pin one specific scene instead of waiting for
the random generator to produce it:

| Key | Effect |
|---|---|
| `nosig` | the plain glitch scene (the regression baseline) |
| `snow=N` / `warp=N` / `text=N` | pin one snow variant / warp / sign-text variant |
| `glitch=N` | pin the garbage-character index in the sign, to check the font and the index maths |
| `info=N` / `ch=N` | pin the station-ID style / channel number (`CH 42`, callsigns, alien runes …) |
| `snd=N` | pin one sound profile instead of hoping for the right subject family |
| `marker=N` | draw deterministic dots into the frame *before* the warp, so a row shift becomes measurable |

`marker` is the trick that made the strong-tear bug measurable: the dots sit every 7 px, so the
phase of the dots in each band tells you the shift modulo 7, and the wrapped dots at the row edge
pin the absolute value (−20 px / +12 px in `docs/VERIFY-v0.1.0.md`).

## 7. How to verify a change here

The rule used throughout this project: **a number, not an impression.**

- Anything visual: capture frames through `/hrns/bmp` and compute statistics on the pixels
  (mean luminance per frame, ink counts inside the sign box, palette signatures). Sending the frame
  to a vision model is *slower and less precise* than counting pixels.
- Anything about glyphs: parse `FONT57` from the source, render the expected string in Python, and
  compare it pixel by pixel against the capture inside the glyph cells.
- Anything about timing/power/audio: **the harness cannot see it** (Light Sleep detaches USB CDC,
  backlight never touches the framebuffer). Say so honestly in the PR, describe the on-device
  procedure you used, and prefer a release build + human observation over a fake measurement.
- Keep the frame-size assertion (`64854` bytes) in any capture script: a truncated BMP means a
  failed capture, not a small screen.

## 8. Serial console — what it is (not) good for

`pio device monitor -e sticks3 -b 115200` works for the dev build and is useful for boot messages.
Two caveats specific to this device:

- The power-save path calls `esp_light_sleep_start()` between frames; **USB CDC detaches** while
  the chip sleeps. Expect to re-plug, or to hold KEY1 + tap RST to get a port back.
- `Serial.printf` in the ESP-IDF flavour **does not support `%lld` or `%f`**. Using them shifts
  every following argument and prints garbage — format integers as `int` with `%d` in any probe
  you add.

## 9. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Black screen after flashing with M5Burner | You flashed the app-only image. Use the **merged** image (§5) |
| Black screen after flashing with `pio run -t upload` | `flash_mode` is not `dio`, or a stale bootloader: set `board_build.flash_mode = dio` and `pio run -t clean` before rebuilding |
| `Failed to connect … No serial data received` | The device is asleep or was left in Light Sleep. Hold KEY1 (BtnA), tap RST, keep holding until the upload starts |
| Upload works, screen stays dark, device reboots in a loop | Usually a corrupted NVS from a partial flash: `esptool erase_flash` (erases NVS, Wi-Fi/time settings) and flash again |
| `fatal error: config.h: No such file or directory` | Dev build without credentials: `cp src/config.h.example src/config.h` (a fresh clone compiles the release build without it) |
| The harness is unreachable | The dev build needs a 2.4 GHz network (the ESP32-S3 has no 5 GHz radio). Check the device's IP in your router, or press RST and watch the boot log |
| The picture is very dim | That is the breathing layer at its dark phase plus the default 30 % brightness. Press B to step the brightness up; the breathing still happens |

## 10. Release checklist

1. `pio run -e sticks3` and `pio run -e sticks3-prod` both build with no new warnings.
2. Bump the version string **everywhere**: the splash (`main.cpp`, `const char* ver = "V0.1.0";`),
   `main.cpp`'s header comment, this repo's `CHANGELOG.md`, `README.md`/`README_CN.md`, and
   `m5burner/no-signal.json`. (This project has shipped a stale splash version before — check it.)
3. `strings` the release binary for credentials and for `/hrns/` routes: both must be zero hits.
4. Build the merged image, verify the layout (§5), write the `.sha256` files.
5. Flash the *merged* image onto a real device at least once (`esptool write_flash 0x0 …`) and
   confirm `Hash of data verified`.
6. Tag, publish, attach `merged.bin`, `.bin` and both `.sha256` files; paste the version string and
   the sha256 into the release notes.

## 11. Reproducibility — what "same binary" means here

A fresh clone of this repository, built with the pinned dependency set (§1), produces the **same
machine code** as the released firmware. It cannot produce a byte-identical *file*, and it is worth
knowing exactly why before you conclude something is broken:

| Section | Comparison | Why |
|---|---|---|
| `.flash.text` | ✅ byte-identical | the compiled code — this is the one that matters |
| `.dram0.data` | ✅ byte-identical | initialised data |
| `.flash.rodata` | ⚠️ differs | `__FILE__` paths in assert/log strings. E.g. `/Users/you/…/verify-clone/.pio/libdeps/sticks3-prod/M5GFX@0.2.28/src/lgfx/v1/panel/Panel_AMOLED.cpp` is embedded with your absolute build path |
| `firmware.bin` sha256 | ⚠️ always differs | same reason, plus the image header carries an ELF-hash field and the app descriptor carries build date/time |

So compare sections, not files:

```bash
OC=$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-objcopy
$OC -O binary --only-section='.flash.text*' -j '.text*' firmware.elf a.bin   # run once per tree
shasum -a 256 a.bin b.bin        # identical => same code
```

Two further notes from verifying the v0.1.0 release this way:

- PlatformIO keeps the transitively required `M5GFX` **and** the pinned `M5GFX@0.2.28` in
  `.pio/libdeps/<env>/`. The pinned one is what gets compiled — verified by the `.flash.text`
  comparison above; the other directory is unused storage.
- The published v0.1.0 images themselves are reproducible: rebuilding `main` at the pin gave
  `.flash.text` = 956112 B, `.dram0.data` = 23648 B, both byte-identical to the released files.
