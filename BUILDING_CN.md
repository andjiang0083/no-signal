# 构建 · 刷写 · 测试（NO SIGNAL）

**中文** · [English](BUILDING.md)

全部基于 PlatformIO + Arduino-ESP32 核心：没有 ESP-IDF 工程，没有 CMake 折腾，一条 `pio run`。

## 1. 前置

| 需要 | 为什么 |
|---|---|
| [VS Code](https://code.visualstudio.com/) + [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) 扩展 | 常规构建/刷写方式 |
| 或 PlatformIO CLI（`pip install platformio`） | 终端里 `pio run -e sticks3-prod -t upload` |
| Python 3 + `pillow`（**仅** dev harness 客户端 `tools/control.py` 需要） | `pip install pillow` |
| 一块 M5Stack **StickS3** | ESP32-S3、8MB flash、8MB 八线 PSRAM。StickC 不行——flash 模式与按键都不同 |
| 一根能传数据的 USB-C 线 | 只有充电线的线材是经典的"排查二十分钟"来源 |

第一次构建会下载 ESP32 平台、Arduino 核心与 M5Unified（几百 MB），这是正常的，只发生一次。

**依赖已钉版**（`platformio.ini`）：M5Unified `0.2.21` + M5GFX `0.2.28`。Releases 页上 v0.1.0 的
二进制就是用这两个版本、配 Arduino core `3.3.9` / ESP-IDF `5.5.4` / xtensa-esp-elf `14.2.0` 工具链构建的。
不钉版就意味着克隆当天拉到什么就用什么——本仓库第一次克隆解析到了 M5Unified `0.2.23` / M5GFX `0.2.30`，
产出的二进制与发布件差了 656 字节、各段尺寸也不同。要升级请单独一个 commit 改钉，并见 §11。

## 2. 两个编译环境

| 环境 | 命令 | 是什么 |
|---|---|---|
| **dev** | `pio run -e sticks3` | Wi-Fi STA + HTTP 测试 harness + `src/config.h` 里的串口凭据。开发期刷这个 |
| **release** | `pio run -e sticks3-prod` | `-DPRODUCTION_BUILD=1`：harness、STA 凭据、`/hrns/*` 路由全部编译期剔除。Wi-Fi 本身**刻意保留**，给首次开机的扫码对时用 |

其余 flag 完全一致：`board_build.flash_mode = dio`、8MB flash、`board_build.partitions = default_8MB.csv`、
`board_build.arduino.memory_type = qio_opi`（PSRAM 走 OPI，与 flash 模式独立）、240MHz。

> ⚠️ **`dio` 不是偏好，是硬要求。** StickS3 用 QIO bootloader 起不来；如果你改了 `flash_mode`，
> 必须 `pio run -t clean` 让 bootloader 重编，否则会得到一个"二进制看起来完全正常"的黑屏。

## 3. 开发凭据

dev 版要连上你的局域网，harness 才可达：

```bash
cp src/config.h.example src/config.h
$EDITOR src/config.h          # 填你的 2.4GHz SSID 与密码
```

`src/config.h` 已被 gitignore；发布版永不编译它。任何时候都可以自查：

```bash
strings -a .pio/build/sticks3-prod/firmware.bin | grep -c '<你的 SSID>'    # 必须输出 0
```

## 4. 刷写

```bash
pio run -e sticks3-prod -t upload                      # 自动找串口
pio run -e sticks3-prod -t upload --upload-port /dev/cu.usbmodem101
pio device monitor -e sticks3 -b 115200                # 仅 dev 版，见 §8
```

**下载模式**（常规上传连不上时）：按住 **KEY1（BtnA）** 的同时点一下背面 **RST**，继续按住 KEY1，
开始上传，刷完再松手。设备进过省电后这一步是必须的：Light Sleep 会断开 USB CDC，自动复位信号到不了芯片。

## 5. 制作 M5Burner 镜像

PlatformIO 已经会产出 merged 全镜像——除非必要，不要手工合并：

```bash
pio run -e sticks3-prod
cp .pio/build/sticks3-prod/firmware.factory.bin  releases/no-signal-<version>-merged.bin
cp .pio/build/sticks3-prod/firmware.bin          releases/no-signal-<version>.bin
shasum -a 256 releases/no-signal-<version>*.bin  # 附到 GitHub release
```

发布前校验布局（很便宜，能挡住经典错误）：

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

再和 PlatformIO 自己写进芯片的参数对齐：

```bash
pio run -e sticks3-prod -t upload -v 2>&1 | grep write-flash
# ... write-flash -z --flash-mode dio --flash-freq 80m --flash-size detect
#     0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

merged 镜像必须与这些偏移和 flash 模式一致。**M5Burner 固定从 0x0 写**，这就是 app-only
`*.bin` 绝对不能喂给它的原因。

M5Burner 导入：**Custom → Import Custom FW →** 选 `m5burner/no-signal.json`（清单引用的 merged bin 要放在同目录），
或直接选 merged bin。

## 6. 测试 harness（仅 dev 版）

```
GET  /hrns/bmp     -> 真实帧缓冲，54 字节头的 16bit BMP
GET  /hrns/debug   -> {"uptime":…,"free_heap":…,"min_free_heap":…,"psram_free":…,"btn_a_pending":…}
POST /hrns/btn     -> 注入按键（{"btn":"a"} / {"btn":"b"} / {"btn":"a","long":1}）
POST /hrns/data    -> JSON 开关，直接钉死场景生成器，见下表
```

```bash
python3 tools/control.py <设备IP> snap       # 存一张截图
python3 tools/control.py <设备IP> data '{"nosig":1}'
curl -s http://<设备IP>/hrns/bmp | wc -c     # 必须正好 64854
```

注入开关的意义：让复核者直接钉住某一个场景，而不是等随机数生成器恰好抽到它。

| 键 | 作用 |
|---|---|
| `nosig` | 普通故障场景（回归基线） |
| `snow=N` / `warp=N` / `text=N` | 固定一种雪花变体 / 扭曲 / 招牌文字变体 |
| `glitch=N` | 固定招牌乱码位（验证字模与索引算式） |
| `info=N` / `ch=N` | 固定台标款式 / 频道号（`CH 42`、呼号、外星符文…） |
| `snd=N` | 固定一档声音，不必碰运气等主体类别 |
| `marker=N` | 在扭曲**之前**往画面里画确定性白点，把行位移变成可测量量 |

`marker` 就是让"强撕裂"这个 bug 可测的招：白点每 7px 一个，于是每一带的白点相位给出位移模 7，
行边缘的环绕白点再锁定绝对值（`docs/VERIFY-v0.1.0.md` 里量到的是 −20px / +12px）。

## 7. 这里怎么验证一个改动

贯穿本项目的规矩：**给数字，不给感觉。**

- 视觉类：通过 `/hrns/bmp` 连拍，然后在像素上算统计量（整帧平均亮度、招牌盒内亮像素数、调色板签名）。
  把画面送给视觉模型**比数像素更慢也更不准**。
- 字形类：从源码解析 `FONT57`，在 Python 里渲染期望字符串，和抓帧在字形格内逐像素比对。
- 时序 / 功耗 / 音频类：**harness 看不见**（Light Sleep 断 USB CDC，背光根本不进帧缓冲）。
  在 PR 里如实说明，并写下你在真机上做的操作流程；宁要"发布版 + 人眼观察"，也不要编一个假测量。
- 抓帧脚本里保留 `64854` 字节断言：BMP 变短说明抓帧失败，不是"屏幕变小了"。

## 8. 串口控制台能做什么、不能做什么

`pio device monitor -e sticks3 -b 115200` 在 dev 版可用，看启动信息很方便。两个本机特有的坑：

- 省电路径每帧之间调 `esp_light_sleep_start()`，**芯片睡着时 USB CDC 会断开**。准备好重插，
  或按住 KEY1 + 点 RST 找回串口。
- ESP-IDF 版的 `Serial.printf` **不支持 `%lld` 与 `%f`**：用了会让后续参数全部错位、打出乱码。
  探针里一律把整数格式化成 `int` + `%d`。

## 9. 故障对照表

| 现象 | 原因 / 处理 |
|---|---|
| M5Burner 刷完黑屏 | 刷成了 app-only 镜像。用 **merged** 全镜像（§5） |
| `pio run -t upload` 之后黑屏 | `flash_mode` 不是 `dio`，或 bootloader 是旧的：设回 `dio` 并 `pio run -t clean` 重编 |
| `Failed to connect … No serial data received` | 设备在睡觉或停在 Light Sleep。按住 KEY1（BtnA）、点 RST、一直按住直到上传开始 |
| 上传成功但屏不亮、反复重启 | 通常是半途断电造成的 NVS 损坏：`esptool erase_flash`（会清 NVS / Wi-Fi / 对时）后重刷 |
| `fatal error: config.h: No such file or directory` | dev 版缺凭据：`cp src/config.h.example src/config.h`（全新 clone 编发布版不需要它） |
| harness 连不上 | dev 版要 2.4GHz 网络（ESP32-S3 没有 5GHz 射频）。去路由器里找设备 IP，或点 RST 看启动日志 |
| 画面很暗 | 那是呼吸层的暗相位 + 默认 30% 亮度。按 B 提亮；呼吸照常进行 |

## 10. 发布检查清单

1. `pio run -e sticks3` 与 `-e sticks3-prod` 都无新增告警。
2. **所有**版本串一起改：启动页（`main.cpp` 的 `const char* ver = "V0.1.0";`）、`main.cpp` 头注释、
   本仓 `CHANGELOG.md`、`README.md`/`README_CN.md`、`m5burner/no-signal.json`。
   （本项目历史上漏改过一次启动页版本号——记得核对。）
3. 对发布版二进制 `strings` 查凭据与 `/hrns/` 路由：两者都必须 0 命中。
4. 生成 merged 镜像、校验布局（§5）、写 `.sha256`。
5. 至少往真机**用 merged 镜像刷一次**（`esptool write_flash 0x0 …`），并确认 `Hash of data verified`。
6. 打 tag、发 release、附 `merged.bin` / `.bin` / 两个 `.sha256`；发布说明里写上版本串与 sha256。

## 11. 可复现性 —— "同一个二进制"在这里指什么

用钉住的依赖集（§1）从全新克隆构建，得到的**机器码与发布固件完全相同**。但它不可能产出一个逐字节一致的
*文件*，在下结论"是不是坏了"之前，值得知道确切原因：

| 段 | 比对结果 | 原因 |
|---|---|---|
| `.flash.text` | ✅ 逐字节相同 | 编译出的代码——这一项才是关键 |
| `.dram0.data` | ✅ 逐字节相同 | 已初始化数据 |
| `.flash.rodata` | ⚠️ 不同 | assert/日志字符串里的 `__FILE__` 路径，例如把 `/Users/你/…/verify-clone/.pio/libdeps/sticks3-prod/M5GFX@0.2.28/src/lgfx/v1/panel/Panel_AMOLED.cpp` 这类绝对构建路径嵌了进去 |
| `firmware.bin` 的 sha256 | ⚠️ 必然不同 | 同上，且镜像头里带一个由 ELF 推出的哈希字段、app descriptor 里带构建日期时间 |

所以要**比段，不比文件**：

```bash
OC=$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-objcopy
$OC -O binary --only-section='.flash.text*' -j '.text*' firmware.elf a.bin   # 每棵树各跑一次
shasum -a 256 a.bin b.bin        # 相同即同一份代码
```

用这套方法核对 v0.1.0 发布件时还得到两条结论：

- PlatformIO 会在 `.pio/libdeps/<env>/` 里同时留下传递依赖的 `M5GFX` 和钉住的 `M5GFX@0.2.28`；
  真正参与编译的是钉住的那个——由上面的 `.flash.text` 比对证实，另一个目录只是占地方。
- 已发布的 v0.1.0 镜像本身是可复现的：用钉版重编 `main` 得到 `.flash.text` = 956112 B、
  `.dram0.data` = 23648 B，与发布文件逐字节相同。
