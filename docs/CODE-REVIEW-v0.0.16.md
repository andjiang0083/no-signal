# no-signal v0.0.16 — 代码走查报告

> 日期：2026-09-29 · 范围：`src/main.cpp`(3640 行) / `breath.h` / `timesync.h` / `prog_sound.h` / `prog_shapes.h` / `harness.h` / `platformio.ini` / 文档口径
> 方式：逐行读源码 + 编译实证（dev/prod 双环境）+ 产物字符串审查 + 脚本核算（字模覆盖 / 静态内存）+ 依赖库交叉验证（M5Unified 板级定义）
> 代码基线：`031d68b`（工作树干净）

## 0. 结论先行

| 级别 | 条数 | 一句话 |
|---|---|---|
| **P0** | 1 | v0.0.16 主打卖点「声学轴」**没接线**：节目期间除拍打音弧外全程静默，5 档里 4 档是死代码 |
| **P1** | 8 | 都是用户能看见/听见的：缺字模（台标显示 `C 5`）、省电唤醒引脚写错、唤醒键误触发静音/亮度、开机亮度被"临时诊断"写死 100%、雪花呼吸失效、强撕裂空转、对时界面提示的功能不存在 |
| **P2** | 9 | 卫生类：静态内存 28.8KB 白占、字节序口径不一、越界读、死代码、注释/文档漂移 |

**编译实证**：`pio run -e sticks3` 与 `-e sticks3-prod` 均 SUCCESS，无编译器告警；dev RAM 81.8%（268,040/327,680）、Flash 39.8%；prod RAM 81.8%、Flash 39.6%。
**发布安全**：prod 固件里不含本机开发凭据（Wi-Fi SSID / 密码）与 `/hrns/bmp` / `/hrns/btn` 路由，**均为 0 处**（凭据与 harness 路由确实被排除）✅；但 `NO-SIGNAL`(3) / `WIFI:T:nopass`(1) / `Time Sync`(1) 存在 → 发布版首次开机会开 SoftAP（见 P1-8，决策项）。

---

## P0

### P0-1 「声学轴」档位从未被写入 → 盖革/外星语/SOS/机械嗡 全部死代码

**证据链（四步，任一环节都断）**

1. 抽档只写进结构体字段，没人读：`main.cpp:279` `g_g.progSnd = psPick(s_subj[g_g.progSubj].cat);`
   —— 全树 grep `progSnd` 只有两处：定义（`main.cpp:113`）与这次赋值，**没有任何读取点**。
2. 模块内部真正决定播放的 `s_psSnd` 从未被"抽档"赋值：`prog_sound.h:29` 初值 `PS_SILENT`，此后只在 `prog_sound.h:120/139` 被置回 `PS_SILENT`、在 `:132` 被置 `PS_ALIEN`。`psPick()` 只是 `return` 一个值，**不落任何状态**（`prog_sound.h:61-71`）。顺带两个口径问题：`psPick` 有 5 个分支（自然/文明/神话/**cat4 = AI 频道**/科幻），但**永远不会返回 `PS_SILENT`** → 计划与 CHANGELOG 里写的"静谧（白噪只留底）"档现在是抽不到的。
3. 调用方传的 `active` 用错了旗标：`main.cpp:3577-3579` 传 `g_g.active`（=严重故障场景），而"节目进行中"其实是 `g_g.progT > 0`；`prog_sound.h:117` 的注释还把 `active` 解释成"节目进行中（g_g.active）"，等于把错误固化进文档。且 `main.cpp:3601` 在节目帧里刚把 `g_g.active = 0` 清掉 → `prog_sound.h:138` 的 `if (!active)` 立刻判"无节目 → 静默"。
4. 结果：`prog_sound.h:143` 的 `switch (s_psSnd)` 只会命中 `PS_SILENT`（`default: break`，什么都不做）或 special6 那条外星语背景。`PS_GEIGER_SPARSE / PS_GEIGER_DENSE / PS_MORSE_SOS / PS_HUM_CRACKLE` 四档为死代码（`alienStep`/`morseStep`/`humStep` 三个函数无调用者可达）；`PS_ALIEN` 只在 special6 生效。

**影响**：`CHANGELOG.md` v0.0.16「节目全程按主体类别配乐 → 节目结束戛然静默」、`README.md`「画面和声音讲同一个故事」、实施计划 T13/T17 —— **均未落地**。用户拍出节目实际只听到：拍打"嗡"声 + 1.1~2s 锁定音弧（`psArcStart` 这条通路），然后安静。
**改法**（三处小改，不动架构）：
- `prog_sound.h` 增加 `static inline void psSetSnd(uint8_t snd) { s_psSnd = snd; s_psNext = millis() + 300u; s_psMorse = 0; s_psT2 = 0xFFFFFFFF; }`
- `main.cpp:279` 改为 `g_g.progSnd = psPick(s_subj[g_g.progSubj].cat); psSetSnd(g_g.progSnd);`
- `main.cpp:3577-3579` 第三参数改传 `g_g.progT > 0`；special6 分支不动（它自己覆盖 ALIEN）。
- 顺带：`psPick` 补回"静谧"档并规定概率（或改 CHANGELOG 口径），`prog_sound.h:117` 注释同步改成"节目进行中（g_g.progT > 0）"。

**验证方式**：dev 版真机拍打 10 次，自然/文明/神话/科幻各拍到 2 次 → 应分别听到盖革稀（3~8s 一声）/ SOS 摩尔斯 / 外星数字语 / 盖革密；节目结束立即静默。可先临时在 harness 的 `onData` 里加 `psSetSnd(N)` 缩短验证（免去靠随机抽类）。

---

## P1（用户可见故障 / 复发风险 / 照注释做事就错）

### P1-1 字模缺 12 个字母，台标与呼号静默缺字（屏幕显示 `C 5`、`NO S O ING`）

**证据（脚本核算 `FONT57[30][7]`，`main.cpp:412-443`）**
字模全集 = `0-9 # % - . : A B C E G I L M N O R S T V Y`（与 `glyphIdx` 支持集一致）。
屏幕文案缺字核算结果：

| 文案 | 缺字 | 实际显示 |
|---|---|---|
| `"CH %d"`（台标 case 1/2/default，`main.cpp:2921/2934/2988`）| **H** | `C 5` |
| `"NOW SHOWING"`（台标 case 3，`main.cpp:2940`）| **H W** | `NO S O ING` |
| `"TEMP"`（天气预报底部，`main.cpp:2197`）| **P** | `TEM` |
| `"KH-TV"` / `"WQ-3"` / `"ZXN"` / `"QR-9"` / `"LP-2"`（呼号表 `main.cpp:2946`）| **K H** / **Q W** / **X Z** / **Q** / **P** | `-TV` / `-3` / `N` / `R-9` / `L-2` |
| `"NO SIGNAL"` `"V0.0.16"` `"A: TIME SYNC"` `"ALERT"` `"N V 9 3"` `"M5-TV"` | — | ✅ 安全 |

`drawStr57`（`main.cpp:479-485`）对不存在的字形只跳过、不报错、字距照走 → 屏幕上是"漏字 + 空格"，一眼看得出是 bug 而不是风格。
**改法**（二选一）：
1. 补 `H W P Q`（建议再补 `X Z`）字模进 `FONT57`，索引 30 扩到 34/36，同步 `glyphIdx` 的 `case` 与头注释；**必须按 §8.46 逐字目视校对**（A/E 那次的教训）。
2. 或把文案改成字模内的字母（呼号换 `M5-TV / LP-2 / RC-9` 之类），`CH` 单独画一个 `H`。
**验证方式**：dev 真机截图（`tools/control.py <ip> snap`）连拍抽到台标 8 款 + 呼号，逐字校对——字模类的坑只有拼写级目视抓得到。

### P1-2 省电模式的唤醒源引脚写错（GPIO37/39 ≠ 按键）

**证据**
- `main.cpp:3281-3282`：`SAVE_WAKE_A = GPIO_NUM_37`（注释 `// BtnA`）、`SAVE_WAKE_B = GPIO_NUM_39`（`// BtnB`）；`main.cpp:3322-3323` 拿它做 `gpio_wakeup_enable(..., GPIO_INTR_LOW_LEVEL)`。
- M5Unified 对 StickS3 的按键定义：`M5Unified.cpp:3538-3541` `case board_t::board_M5StickS3: use_rawstate_bits = 0b00011; btn_rawstate_bits = ((!gpio_in(GPIO_NUM_11))&1) | ((!gpio_in(GPIO_NUM_12))&1)<<1;` → **BtnA = GPIO11 / BtnB = GPIO12**。
- 另注：GPIO33~37 在 ESP32-S3 上是 SPI0/1 高速端口（八线 PSRAM 走的就是这几根）、GPIO39 是 MTCK —— 两个都不是按键，电平由总线/悬空决定。

**影响**：light sleep 的 GPIO 唤醒从未生效，"按键即醒"实际靠 1Hz 定时唤醒 + 主循环 `M5.update()` 轮询（最长 ~1s 延迟）；悬空/总线引脚还可能造成虚假唤醒，白吃省电收益。
**改法**：`SAVE_WAKE_A = GPIO_NUM_11; SAVE_WAKE_B = GPIO_NUM_12;`（别照抄 GPIO 数字，建议直接引用 M5Unified 的板级定义，免得再错一次）。
**验证方式**：dev 版把 `SAVE_IDLE_MS`（`main.cpp:3276`）临时改 20000 → 进省电后按 A/B 应在 <100ms 内退出；再改回 `300000` 烧发布版让用户目视。省电/时序类**不走截图验证**（背光与 light sleep 都是截图盲区）。

### P1-3 唤醒那一次按键没被"吃掉" → A 唤醒顺带静音、B 唤醒顺带切亮度并写 NVS

**证据**：`lightSleepTill()` 在 GPIO 唤醒后**自己**调用了 `powerSaveExit()`（`main.cpp:3318-3332`，退出发生在 `main.cpp:3327-3328` 的 `gpio_wakeup_disable` 附近）；等控制权回到 `loop()` 时 `g_g.powerSave` 已是 `false`，于是 `main.cpp:3524-3533` 的 `if (g_g.powerSave) {…消费掉这次点击…}`（注释还特意写着"本次按键已消费：不触发 A 静音 / B 亮度"）进不去，落到 `else` → `M5.BtnA.wasClicked()` 翻转静音；`main.cpp:3549-3558` 的 B 键分支（注释"省电唤醒的那次 B 不切换亮度（刚被消费）"）跟着切亮度并写 NVS。
**时序细节**：GPIO 唤醒发生在**按下瞬间**（此刻按键仍按住，`M5.update()` 只看到 pressed），等**松手**那一次才产生 `wasClicked()` —— 而那时 `powerSave` 早已是 false，所以这次点击必然落进常规处理。
**说明**：目前这条被 P1-2 掩盖（引脚错 → 没有 GPIO 唤醒路径，唤醒靠定时器，`powerSave` 那时仍为 true，点击确实被消费了）。**修完 P1-2 后立刻显形**，必须同批修。
**改法**：`lightSleepTill()` 改成只置标志 `g_g.saveWakeClick = true;`（不直接退出省电），在 `loop()` 顶部 `M5.update()` 之后、按键分支之前消费一次 A/B 点击（消费后清标志 + `powerSaveExit()`），再进常规分支。
**验证方式**：与 P1-2 同一次真机验证里——进省电 → 按 A 唤醒 → 确认**没有**静音（白噪仍在）→ 按 B 唤醒 → 确认亮度档位没变（重启后 NVS 里 `bri` 仍是原值）。

### P1-4 开机亮度被"临时诊断"写死 100%（NVS 记忆与默认 30% 档全部失效）

**原文**（`main.cpp:3462-3466`）
```cpp
g_briIdx = prefs.getUChar("bri", 2);   // 默认 30%（省电优先）
if (g_briIdx > 5) g_briIdx = 2;        // 脏值兜底
prefs.end();
M5.Display.setBrightness(255);   // ★临时诊断：强制全亮（验证 NVS 亮度档位是否导致黑屏闪烁）
(void)g_briIdx;
```
**影响**：每次开机都是 100% 背光——NVS 亮度记忆（经验沉淀 §8.49 的修复）与"默认 30%"一起被这行调试代码废掉；桌搭常显 + 电池场景直接违背省电定位（夜间也刺眼）。这正是 §117 的第 ④ 类"自己写的规矩自己不守"。
**改法**：删掉这两行，恢复 `M5.Display.setBrightness(BRI_LEVELS[g_briIdx]);`。
**验证方式**：截图验不出来（背光不改帧缓冲，§8.47）→ 真机目视：B 键切到 5% → 断电重启 → 应仍是 5%（而非全亮）。

### P1-5 雪花「呼吸」变体实际上不呼吸（`mult` 恒等于 128）

**原文**（`main.cpp:906-923`，关键行 909）
```cpp
uint8_t mult = (uint8_t)(128 + 100 * (int16_t)(sinf(ph * 0.12f) * 0.5f + 0.5f) / 100);
```
`sinf(...)*0.5f+0.5f` 值域 `[0,1]`，先被 `(int16_t)` 截断 → 恒为 0（`ph*0.12` 取不到精确 π/2）→ `mult = 128 + 0 = 128` 恒定。注释写的"整体亮度随时间起伏"不成立，实际等价于"雪花亮度固定减半"（这个半亮值还被写回 `g_phos`，场景结束后从半亮恢复）。
**改法**：`int mult = 128 + (int)(100.0f * (0.5f + 0.5f * sinf(ph * 0.12f)));   // 128..228`
**验证方式**：dev 版加 `#ifdef DEBUG_HOLD_SNBREATH` 常驻该变体 → 截图序列看平均亮度起伏（PIL 数值统计即可，不必送视觉模型）。

### P1-6 「强撕裂」扭曲（WR_TEAR）多数时候等于空转

**证据**：`sceneApply` 的 WR_TEAR（`main.cpp:1386-1392`）设置 `g_g.tearRow/tearLen/tearShift` 后调用 `tearApply()`，但 `tearApply` 第一行是 `if (g_g.tearRow == 0 || g_g.tearT == 0) return;`（`main.cpp:985`）。故障场景期间 `tearT` 绝大多数帧为 0（撕裂是 4~12s 一次的独立小事件，每次仅几帧）→ 该变体被门槛挡掉，C 类扭曲里这一条等于"什么都不做"。
**改法**：给 WR_TEAR 走专用多带撕裂（不复用被 `tearT` 门控的 `tearApply`），或调用前显式 `g_g.tearT = g_g.t;` 让场景内保持。
**验证方式**：dev 版临时把 `g_g.warp` 固定成 `WR_TEAR`（改动一行烧一版），截图应看到上下两带错位 ≥20px。

### P1-7 对时界面提示的功能不存在，超时口径三处不一致

**证据**
- `timesync.h:171` QR 页面底部印着 `"B=skip  A=1.5s exit"`，但 `loop()` 的 `ST_SYNC` 分支（`main.cpp:3511-3522`）只做 `syncAPTick()` + 成功/超时判断，**没有任何按键处理** → B 跳过 / A 退出都不存在（`syncAPTick` 里的 `M5.update()` 只是刷新按键状态）。
- 超时口径不一致：常量 `timesync.h:239` `SYNC_TIMEOUT_MS = 60000`（行内注释写 60s），而使用点注释写 30s——`timesync.h:273`、`main.cpp:3512`、`main.cpp:3518`；经验沉淀 §8.50 要求"AP 必须有时限（30s）"。→ 实际 AP + 屏显 60s。
**影响**：用户按提示操作无反应，只能干等；首次开机（或每次没对过时）AP 时长实为 60s，电池场景是实质差异。
**改法**（建议①，或退一步②）：① 实现按键——ST_SYNC 分支里 `M5.BtnA.wasClicked() || M5.BtnB.wasClicked()` → `syncAPEnd(); enterApp();`（A 在 splash 已承担"进对时"，语义一致）；② 删掉那句提示字符串。同时把常量与三处注释统一（30s 或显式 60s）。
**验证方式**：dev 真机进对时界面按 A/B，应立刻回雪花界面。

### P1-8 发布版首次开机会开 SoftAP（决策项，非缺陷）

**证据**：prod 固件字符串里 `NO-SIGNAL` 3 处、`WIFI:T:nopass` 1 处、`Time Sync` 1 处；`syncAPBegin()`（`timesync.h:241-265`）没有 `PRODUCTION_BUILD` 保护。
**影响**：产品设计上这是"扫码对时"入口（有意为之），但与发布检查清单"发布版不应带 AP 模式热点"冲突；`platformio.ini:14` 注释"发布版：关闭 harness + WiFi + 串口日志"也与事实不符（prod 仍保留 WiFi 用于对时）。
**建议**：保持功能但把口径写清——`platformio.ini` 注释改成"关闭 harness + 凭据；保留 WiFi 仅用于首次对时（有时限）"，`README.md`/发布说明写明"首次开机屏显 QR，未对时自动回雪花"。

---

## P2（卫生）

| # | 位置 | 问题 | 改法 |
|---|---|---|---|
| P2-1 | `main.cpp:1-8` | 头注释停在 **v0.0.3**（列的功能是雪花+招牌+撕裂/跳帧/静噪+B 键亮度），落后 13 个版本 | 改成 v0.0.16 的功能摘要或直接指向 CHANGELOG |
| P2-2 | `main.cpp:1014` | `static uint16_t compressed[SCR_W * 60]` = **28,800B** 独占静态内存（map 实证），只在 `band<60` 时用 | 改用现成的 `s_fbTmp` 前 60 行（该路径无人并发使用）→ 静态占用 81.8% → ~73%，heap 余量 +28.8KB |
| P2-3 | `harness.h:122-159` | BMP 需一次性 **64,854B** 连续堆，而内部堆余量只有 `327,680-268,040 ≈ 59.6KB` → 历史"半截断流（exit=0，5658/30070/35814 字节）"很可能**不是弱 WiFi 而是堆不足**（Arduino `String` 追加失败会静默变短） | BMP 缓冲显式 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` 或分块 `send()`；验证：`/hrns/debug` 的 `free_heap` + 每次截图硬校验 `size==64854`。这条同时复核 F3C38F2 里"弱 WiFi 导致断流"的旧结论 |
| P2-4 | `main.cpp:1250/1265/1285/1301` vs `breath.h:226-230` | 字节序口径不一：`breath.h` 老实 swap 回逻辑 RGB565 再算，`invert/vign/focus/echo` 直接按逻辑 RGB565 解已 swap 的 fb → 通道分组被重排（负片/重影是"错位的负片"） | 统一走 `breath.h` 的 swap 口径；对称类操作（同系数缩放/均值）视觉上差异小，可只改 `invertApply` |
| P2-5 | `main.cpp:986-993` | `tearApply` 负位移越界：`s_tealTmp[(x + tearShift) % SCR_W]`，当 `tearShift = -12`（WR_TEAR 第二条带）时 `x<12` 得到负下标 → 越界读 24B | 归一化：`((x + tearShift) % SCR_W + SCR_W) % SCR_W` |
| P2-6 | `main.cpp:649-650` | 死代码：`case 9`（上下分裂）里 `memcpy(&fb[y*SCR_W], &fb[y*SCR_W], SCR_W*2);  // 保留（用平移实现）` 是自我拷贝，整段循环无副作用（下一行的真平移才是实现） | 删除该循环 |
| P2-7 | `main.cpp:743-745` + `689-691` | 乱码位索引两处小错：`if (ci == 2) ci = 3;` 注释说跳过 `"NO SIGNAL"` 里的空格，但计数只数**非空格**字符 → idx 0..7 = `N O S I G N A L`，**idx2 是 'S' 不是空格**（白白放过一个字母）；`% 9` 又让 idx=8 指向串尾之后 → 约 1/9 概率乱码事件空转（换成 `23:47`/`C:42` 这类短串时更容易空转） | 删掉 `ci==2` 判断，`% 9` 改 `% 8`（更稳：按 `msg` 实际非空格字数取模） |
| P2-8 | `main.cpp:697-699` vs `714-716` | 文字变体 5（打字机）那一帧提前 `return`，**不更新** `s_legX0..s_legY1` → 呼吸层下一帧按旧矩形调制 OSD（一帧错位，肉眼基本看不出） | 在 return 前记录矩形，或把记录移到函数入口 |
| P2-9 | `README.md:52-53`、`platformio.ini:14`、`config.h:1`、`main.cpp:3563`、`main.cpp:3497` | 口径漂移五处：README「M5Burner 刷写」段仍是 **v0.0.13** 文件名（Releases 表已是 v0.0.16）；`config.h` 写 `RELEASE_BUILD`（项目实际用 `PRODUCTION_BUILD`）；platformio 注释与"保留 WiFi"不符（见 P1-8）；`main.cpp:3563` 注释"省电稳态 3fps"而 `SAVE_FPS_HZ = 1`（`main.cpp:3278`）；`main.cpp:3497` 注释"全循环唯一一次 update"但 `syncAPTick()` 内也有 `M5.update()` | 逐处同步；发布前检查清单加"README 文件名/版本号与 releases/ 一致" |

---

## 优化建议（按收益排序）

1. **回收 28.8KB 静态内存**（P2-2）→ 直接给 P2-3 的 65KB BMP 分配和 WiFi 对时留余量；静态降到 ~73%，是这套固件里最便宜的一笔。
2. **全屏后处理的定点化（去浮点/去重复计算）**：`pincApply`（`main.cpp:1195-1204`）每像素都在算 `y/(float)SCR_H` 与 `fy*fy*fy` —— `srcY` 只与 `y` 有关，**每行算一次**即可（135 次而非 32,400 次）；`zoomApply`（`main.cpp:1230-1243`）的 `(x-cx)*k` 可在行内增量累加（每步 +k），省掉每像素乘法；`vignApply`（`main.cpp:1256-1269`）可复用 `breath.h` 已建好的 `s_vigC[256]` 归一表（现在还每像素做 `dx*dx+dy*dy` + 比较 + 3 次乘）。场景帧（1.5~3s）里这些函数是唯一重负载，prod 80MHz 下省下的时间直接换成"场景更不容易掉帧"。
3. **结构拆分**：`progDrawBase`（`main.cpp:1472-2998`，约 1,526 行，其中主体 switch 约 1,150 行）。计划 T3 本来就要拆成 `progSubjNature/Civil/Myth/Scifi` 四个函数（另可放 `prog_subj.h`）。现在 38 主体之后每加一个主体都要在大 switch 里找位置，拆完再加成本减半。
4. **消除"保存/恢复全局 RNG"的脆弱模式**：`progDrawBase` 用 `rngSave = rngState; rngState = progSeed; … rngState = rngSave;`（`main.cpp:1475/1476/1710/2997`），要求**每个出口都恢复**——现在两条出口都对，但下次加分支极易漏（漏了就是"元素逐帧漂移"那类难查 bug）。建议把 RNG 状态当参数传（`fastRand(uint32_t& st)`）或用一个 RAII 小结构。
5. **省电稳态再抠一点**：`powerSave && saveFx==0` 时每帧仍调用 `noiseTick/psSoundTick/legendTick/tapTick`（各自早退，µs 级）——收益很小，做省电专项时顺手短路即可（`clockTick` 必须保留）。
6. **版本口径纪律**：把「splash 版本号 / `main.cpp` 头注释 / README / CHANGELOG / platformio.ini / config.h 宏名」列成发布前固定检查项（v0.0.14→v0.0.16 已经漏过一次 splash 版本号）。

---

## 待办建议（可执行，含文件/参数/验证方式）

**批次 A — 必做（P0 + P1 高频可见，一版改完一起验证）**
1. 接线声学轴：`prog_sound.h` 加 `psSetSnd()`；`main.cpp:279` 调用它；`main.cpp:3577-3579` 第三参数改 `g_g.progT > 0`；顺手补回"静谧"档。验证：真机拍 10 次听四类音。
2. 改唤醒引脚：`main.cpp:3281-3282` → `GPIO_NUM_11` / `GPIO_NUM_12`；同批修 P1-3（`lightSleepTill` 只置 `saveWakeClick` + `loop()` 顶部消费）。验证：dev 版 `SAVE_IDLE_MS=20000` 试唤醒，确认 A 不静音、B 不改亮度。
3. 删 P1-4 的两行诊断（`main.cpp:3465-3466`），恢复 `setBrightness(BRI_LEVELS[g_briIdx])`。验证：真机切 5% → 重启仍是 5%。
4. 对时界面：实现 B=跳过 / A=退出（`main.cpp:3511-3522`）+ 超时口径统一（`timesync.h:239`、`:273`、`main.cpp:3512/3518`）。验证：进对时界面按 A/B 立即回雪花。

**批次 B — 视觉/手感（可单独出一版做截图 review）**
5. 补 `H W P Q`（+`X Z`）字模 + 逐字目视校对（P1-1）——**这条对屏幕观感提升最直接**（`C 5` → `CH 5`）。
6. 修 `SN_BREATH` 的 `mult` 公式（`main.cpp:909`）+ 修 `WR_TEAR` 空转（`main.cpp:1386-1392`）；两处都用"常驻调试宏"截图确认后再删宏。

**批次 C — 卫生与性能（可合并进上面任意一版）**
7. P2-2（28.8KB 回收）+ P2-5（越界归一）+ P2-6（删死代码）+ P2-7/P2-8（乱码索引、OSD 矩形）。
8. P2-4（字节序口径统一，至少 `invertApply`）。
9. 优化建议 2（后处理定点化）：想做"故障场景更稳"时做，改完用 `#ifdef` 计时宏量一次场景帧耗时再对比。

**文档批次（不刷机）**
10. P2-1 / P2-9：`main.cpp` 头注释、README 文件名、platformio 注释、config.h 宏名、3fps/1fps 与"唯一一次 update"注释，逐处同步。

---

## 处置状态（逐条勾）

| # | 条目 | 状态 | 真机/实证 |
|---|---|---|---|
| P0-1 | 声学轴未接线 | ☑ 已修 | 注入 `snd=1..5` 可达；四档听感待用户耳验 |
| P1-1 | 字模缺 H/W/P/Q/X/Z（+K） | ☑ 已修 | 10 款台标 ink 召回 96.3~100% |
| P1-2 | 省电唤醒引脚 37/39 → 11/12 | ☑ 已修 | M5Unified 板表交叉验证；电池态待用户 |
| P1-3 | 唤醒键未消费（静音/亮度误触发） | ☑ 已修 | 同上（省电态） |
| P1-4 | 开机亮度写死 100% 诊断残留 | ☑ 已修 | 默认 30% + 重启记忆待用户 |
| P1-5 | SN_BREATH 不呼吸 | ☑ 已修 | 连拍 16 帧极差 3.0（基线 0.5） |
| P1-6 | WR_TEAR 空转 | ☑ 已修 | 探针实测 −20px / +12px |
| P1-7 | 对时提示不存在 + 60s/30s 口径 | ☑ 已修 | 首次开机按 A/B 待用户 |
| P1-8 | 发布版 SoftAP（决策+文档） | ☑ 已修 | 三处文档同步 |
| P2-1…9 | 卫生 9 条 | ☑ 已修 | RAM 81.8%→73.0%；BMP 64854×3；乱码 +57/+16/+4 等 |

> 全量修订与真机验证明细见 **`docs/VERIFY-v0.1.0.md`**（含每个数字的判据与反事实对照）。
> v0.1.0 已实烧真机（merged 全镜像 @0x0，hash verified）。

> 计数自校：P0 = 1 条、P1 = 8 条、P2 = 9 条（表内 9 行），与第 0 节摘要一致。
