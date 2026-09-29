# Credits and attribution

中文摘要见文末（[跳到中文](#中文摘要)）。

NO SIGNAL is a first-party project: the code, the artwork (all silhouettes are drawn in
code with a hand-rolled primitive library), the palettes and the 5×7 bitmap font were all
written for this device. There is **no third-party code copied into this repository except
one small QR library**, and no assets of any kind (no ROMs, no BIOS, no film stills, no
proprietary fonts).

## The one vendored file: `src/qrcode.h`

`src/qrcode.h` is the QR encoder used by the on-screen time-sync page. It is **MIT
licensed**, © 2017 Richard Moore and © 2017 Project Nayuki, and its notice is reproduced
here in full as the licence requires:

```
The MIT License (MIT)

This library is written and maintained by Richard Moore.
Major parts were derived from Project Nayuki's library.

Copyright (c) 2017 Richard Moore     (https://github.com/ricmoo/QRCode)
Copyright (c) 2017 Project Nayuki    (https://www.nayuki.io/page/qr-code-generator-library)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Build dependencies (fetched at build time, not vendored)

| Project | Author / origin | Licence | How it is used here |
|---|---|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) + [M5GFX](https://github.com/m5stack/M5GFX) | M5Stack | MIT | Board abstraction, ST7789 driver, the `LGFX_Sprite` canvas this project draws every frame into, `M5.Speaker`, `M5.Imu`, `M5.BtnA/BtnB` |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | Espressif | LGPL-2.1 | Arduino API, `Preferences`/NVS, `WebServer` (dev builds), USB CDC, `esp_light_sleep_start` |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif | Apache-2.0 | The SDK underneath: `esp_wifi`, `driver/gpio.h` wake sources, RTC timers |
| [PlatformIO](https://platformio.org/) + [platform-espressif32](https://github.com/platformio/platform-espressif32) | PlatformIO Labs / community | Apache-2.0 | Build system, upload, serial monitor |

## First-party code reused from the author's own earlier project

The 5×7 bitmap font in `src/main.cpp` (`FONT57`) was hand-drawn for the author's earlier
ESP32 project **backrooms-walker** (`src/atmos.cpp`) and is extended here from 28 to 37
glyphs (`H W P Q X Z K` were added in v0.1.0 — see `docs/VERIFY-v0.1.0.md` P1-1). It is
the author's own work, reused with no third-party terms attached. Everything else — the
snow engine, the 3,864-combination glitch pipeline, the 38 program subjects, the sound
axis, the time-sync flow — was written for this repository.

## Trademarks and assets

- **M5Stack**, **StickS3**, **ESP32** and **ESP8266** are trademarks of their respective
  owners. This is an unaffiliated community project written by somebody who owns the
  hardware; it is not endorsed by or connected to M5Stack.
- The repository ships **no game ROMs, no BIOS images, no film stills, no logos, no
  artwork files and no fonts under proprietary licences**. Every pixel is generated at
  runtime from a few hundred bytes of hand-written primitives and a 37-glyph bitmap font.
- The "NO SIGNAL" legend, the world channels, the station callsigns and the test-card
  frames are stylistic references to broadcast television hardware in general, not
  reproductions of any specific broadcast or recording.
- If you believe something here infringes your rights, please open an issue — it will be
  removed or replaced promptly.

---

## 中文摘要

本项目全部为作者原创，仓库内**只有一个小文件是第三方代码**：

- **`src/qrcode.h`**：屏上二维码对时页用的 QR 编码器，**MIT**，© 2017 Richard Moore 与
  © 2017 Project Nayuki（其许可原文已按 MIT 要求完整附在上文英文部分）。
- **构建依赖**（构建时自动下载，不入库）：M5Unified / M5GFX（MIT，M5Stack）、Arduino-ESP32
  核心（LGPL-2.1，乐鑫）、ESP-IDF（Apache-2.0，乐鑫）、PlatformIO 与 platform-espressif32
  （Apache-2.0）。
- **5×7 点阵字模**（`FONT57`）出自作者早先的自研项目 backrooms-walker（`src/atmos.cpp`），
  本仓库把它从 28 字模扩到 37（v0.1.0 补 `H W P Q X Z K`）。
- 其余全部——雪花引擎、3864 种组合的故障流水线、38 个伪节目主体、声学轴、扫码对时——
  均为本仓库原创。
- **M5Stack / StickS3 / ESP32** 是各自所有者的商标，本项目与 M5Stack 无隶属关系。
  仓库**不含任何游戏 ROM、BIOS 镜像、影视素材、logo、美术文件或专有字体**：
  每一个像素都是运行时用手写图元和 37 字模位图生成的。
  若你认为此处内容侵犯了你的权利，请开 issue，我们会尽快移除或替换。
