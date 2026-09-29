# NO SIGNAL — M5Burner 安装包 / installer package (v0.1.0)

[English below](#english) · 中文在上

## 中文

**设备**：M5Stack StickS3（ESP32-S3, 8MB flash, 8MB PSRAM）— 选设备类型时选 **M5StickS3**。

**怎么装（M5Burner）**
1. M5Burner → **Custom** 标签 → **Import Custom FW**
2. 选本目录里的 `no-signal.json`（同目录下有它引用的 `no-signal-v0.1.0-merged.bin`；也可以选 `.bin` 直接烧）
   > ⚠️ 必须用 **merged 全镜像**：M5Burner 固定从 **0x0** 烧录，app-only 的 `no-signal-v0.1.0.bin` 只适合 esptool/OTA。
3. 设备进下载模式：按住左侧 **KEY1(BtnA)** → 点一下背面 **RST** → 继续按住 KEY1 → 点 Burn
4. 烧完自动重启。

**校验**（可选）：`shasum -a 256 -c no-signal-v0.1.0-merged.bin.sha256`

**首次开机**：刷完全镜像后 NVS 是空的 → 开机 3s 启动页 → 屏上二维码对时页（手机扫码连上 WiFi 后浏览 `http://192.168.4.1/`），
**按 A 或 B 立即跳过**，或 60 秒超时自动回雪花。对过一次后每次开机直接进雪花。

**操作**：拍一下=拍电视（50% 伪节目 6s / 50% 扫描线）· A=静音 · B=亮度 6 档（5/10/30/50/80/100%）· 启动页 A=扫码对时 ·
闲置 5 分钟=钟表省电（按 A/B 唤醒，插 USB 不省电）。

**版本串**：开机启动页左下角 `V0.1.0`（报问题时请带上）。

## English

**Board**: M5Stack StickS3 (ESP32-S3, 8 MB flash, 8 MB octal PSRAM) — pick device type **M5StickS3**.

**Install with M5Burner**
1. M5Burner → **Custom** tab → **Import Custom FW**
2. Pick `no-signal.json` from this folder (the `no-signal-v0.1.0-merged.bin` it references sits next to it; you can also pick the `.bin` directly)
   > ⚠️ Always flash the **merged** image: M5Burner always writes at **0x0**, so the app-only `no-signal-v0.1.0.bin` is only for esptool/OTA.
3. Download mode: hold **KEY1 (BtnA)** → tap **RST** on the back → keep holding KEY1 → click Burn
4. The device reboots when done.

**Verify** (optional): `shasum -a 256 -c no-signal-v0.1.0-merged.bin.sha256`

**First boot**: the merged image ships an empty NVS → 3 s splash → on-screen QR time-sync page (scan it with your phone, then open `http://192.168.4.1/`), or **press A / B to skip**, or wait 60 s. Once synced it boots straight to the snow.

**Controls**: slap it = slap the TV (50% fake 6 s program / 50% scan lines) · A = mute · B = brightness (6 levels) ·
splash A = time sync · 5 min idle = clock power save (press A/B to wake; USB power stays awake).

**Version string**: `V0.1.0` on the splash screen — please quote it in bug reports.

## Files

| File | Use |
|---|---|
| `no-signal-v0.1.0-merged.bin` | **M5Burner / esptool @0x0** (bootloader + partitions + app) |
| `no-signal-v0.1.0.bin` | app-only (PlatformIO upload, OTA) |
| `no-signal.json` | M5Burner Custom FW manifest (array format) |
| `cover-320x200.jpg` | store cover (320×200) |
| `*.sha256` | checksums |

Sources & docs: <https://github.com/andjiang0083/no-signal>
