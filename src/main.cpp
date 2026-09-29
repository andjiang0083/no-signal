// ============================================================
//  no-signal v0.1.0 — 桌搭静电电视 (M5StickS3)
//  一台永远停在无信号频道的迷你显像管。
//  日常：磷光雪花 14 变体 + NO SIGNAL 招牌（12 种故障写法）+ 事件型干扰
//        （撕裂/跳帧/静噪爆闪；无常驻扫描线 —— 用户否决项）
//  输入：拍电视(IMU 双阈值) → 故障场景 / 拍出信号(随机节目 6s + 声学轴)；
//        A=静音(启动页 A=对时) · B=六档亮度(5/10/30/50/80/100%，NVS 记忆)
//  时间：启动页 3s → AP+QR 扫码对时（60s 超时，A/B 跳过）→ RTC/NVS 记忆 → 右下角极暗隐藏时钟
//  省电：闲置 5min → 时钟 1fps + light sleep（GPIO11/12 按键唤醒；插电豁免 VBUS）
//  版本史 CHANGELOG.md · 设计意图 SPEC.md · 走查记录 docs/CODE-REVIEW-v0.0.16.md
// ============================================================
#include <Arduino.h>
#include <WiFi.h>       // 发布版也保留：开机 AP 扫码对时用（对时后 WiFi OFF）
#include <WebServer.h>
#include <M5Unified.h>
#include <Preferences.h>   // NVS 亮度档位记忆
#include "esp_sleep.h"     // light sleep 帧间休眠（省电模式）
#include "driver/gpio.h"   // 按键 GPIO 唤醒源

#ifndef PRODUCTION_BUILD
#if __has_include("config.h")
#include "config.h"   // 开发期 WiFi 凭据（gitignore，发布版不含）
#else
// 全新 clone 没有 config.h：仍可编译（dev 版只是连不上网、harness 不可达）
// 需要真机联调时 `cp src/config.h.example src/config.h` 填入自己的 SSID/密码
static const char* WIFI_SSID = "";
static const char* WIFI_PASS = "";
#endif
#endif

// ── Harness（发布版编译排除 begin/端点，见 setup）──
#include "harness.h"
HardwiredTestHarness hrns;

// ── 呼吸层（v0.0.14：常驻生命感，事件层之下；详见 breath.h）──
#include "breath.h"
#include "prog_shapes.h"
#include "prog_sound.h"
// 灯牌矩形（drawLegend 每次绘制后记录，呼吸层 A 阶段用于 OSD 16-bit 混合）
int16_t s_legX0 = -1, s_legY0 = -1, s_legX1 = -1, s_legY1 = -1;

// ── AP+QR 扫码对时（RTC 恢复链 + captive portal；详见 timesync.h）──
#include "timesync.h"

// ── 常量 ───────────────────────────────────────────────────
static const int SCR_W = 240, SCR_H = 135;

// xorshift32（禁 esp_random()/rand() 逐帧调用 — 经验沉淀）
static uint32_t rngState = 0x9E3779B9u;
static inline uint32_t fastRand() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}
// ST7789 直写 framebuffer 必须字节交换（大端序陷阱）
static inline uint16_t swap16(uint16_t v) { return (v << 8) | (v >> 8); }

// ── Fallout 琥珀调色板（逻辑 RGB565，写 fb 前统一 swap）──
static const uint8_t PAL_DARK_R = 14, PAL_DARK_G = 9, PAL_DARK_B = 4;
static const uint32_t COL_NOSIG_FG = rgb565(240, 196, 90);   // NO SIGNAL 字
static const uint32_t COL_NOSIG_BG = rgb565(24, 15, 6);       // NO SIGNAL 垫底

// ── 磷光缓冲：8-bit 亮度层 → 琥珀 LUT（避免对打包 RGB565 做乘法）──
// （外部链接：breath.h 共享；单文件项目无害）
uint8_t  g_phos[SCR_W * SCR_H];          // 32.4KB .bss
uint16_t g_lut[256];                       // 亮度→琥珀色（已 swap）

static void buildLut() {
    for (int i = 0; i < 256; i++) {
        uint8_t r = PAL_DARK_R + (uint8_t)((235 - PAL_DARK_R) * i / 255);
        uint8_t g = PAL_DARK_G + (uint8_t)((180 - PAL_DARK_G) * i / 255);
        uint8_t b = PAL_DARK_B + (uint8_t)((70  - PAL_DARK_B) * i / 255);
        g_lut[i] = swap16(rgb565(r, g, b));
    }
}

// ── 事件状态（干扰全是事件型；无常驻扫描线 — 用户否决项）──
struct GlitchState {
    // 撕裂：整行循环平移（DV 卡带）——常态小呼吸
    int      tearRow; int tearLen; int tearShift; uint8_t tearT;
    uint16_t tearCd;                 // 倒计帧数 → 下次撕裂（4~12s）
    // 跳帧：卡顿冻结画面（8~20s 一次，卡 2 帧）
    uint16_t jumpT;   uint8_t jumpHold;
    // ── 故障场景（v0.0.4+ NO SIGNAL 大集合）：三元素随机组合 ──
    //   每次触发 = 雪花类型 × 文字方式 × 画面扭曲，各抽一个
    uint8_t  active;     // 场景进行中
    uint8_t  snow;       // 雪花 0-13
    uint8_t  text;       // 文字 0-11
    uint8_t  warp;       // 扭曲 0-22
    uint8_t  t;          // 场景剩余帧（1.5~3s）
    uint8_t  t0;         // 触发时总帧数（进度反推用）
    uint8_t  p;          // 场景进度 0~255（渐弱/假关机/黑带用）
    uint32_t ms;         // 下次场景触发时刻（40~150s）
    uint8_t  special;    // 特殊事件: 0无 1频道切换 2信号恢复 3电话干扰 4天线松动 5幽灵广播
    uint8_t  spT;        // 特殊事件剩余帧
    uint8_t  spT0;       // 特殊事件总帧
    uint32_t nextSp;     // 下次特殊事件时刻（与场景错开）
    uint8_t  freq;       // 搜台扫频音相位（D5）
    uint8_t  dtmfT;      // 电话干扰音步进（D7）
    uint8_t  tapT;       // 拍电视扫描线剩余帧（0=无）
    uint8_t  tapT0;      // 拍电视总帧
    uint8_t  tapShock;   // 拍打确认帧计数（双帧确认防误触）
    uint32_t tapCd;      // 拍打冷却（防连拍 spam）
    uint32_t tapLast;    // 上次拍打 millis（冷却判定）
    // ── 拍出信号（v0.0.10+）：40% 概率拍打→随机节目画面 6s ──
    uint8_t  progT;      // 节目剩余帧（0=无）
    uint8_t  progT0;     // 节目总帧（6s）
    uint8_t  progBg;     // 背景 0-5（黄昏/深夜/过曝/雾霾/晨曦紫/墨绿夜）
    uint8_t  progSubj;   // 主体 0-9（山/落日/城市/星野/沙漠/森林/湖影/云海/月球/电线杆）
    uint8_t  progCh;     // 台标频道号 1-13
    uint8_t  progInfo;   // 台标款式 0-4（底中CH/右上CH/彩条CH/文字NOW SHOWING/字母呼号）
    uint8_t  progTex;    // 质感 0-2（微噪/半信号强噪/偏色故障）
    uint8_t  progEnter;  // 收拢风格 0-2（抖动/横向扫描线/圆环）
    uint8_t  progMain;   // 主段动态 0-2（静态微噪/垂直滚动/亮度呼吸）
    uint8_t  progExit;   // 溶回风格 0-2（行撕裂/垂直压缩/抽帧闪烁）
    uint8_t  progCel;    // 天体 0-7（无/太阳/半月/星群/云/飞鸟/闪电/信号塔）；subj=1 落日时忽略
    uint8_t  progRare;   // 稀有事件 0=普通 1=测试卡 2=全屏彩条 3=灰度阶梯 4=TELETEXT 5=马赛克 6=galcoll粒子流 7=backrooms黄雾 8=RLCD海浪线
    uint8_t  progSnd;    // 声学轴档 v0.0.16：0静默 1盖革稀 2盖革密 3外星语 4SOS 5机械嗡（拍出时按 cat 抽取）
    uint32_t progSeed;   // 底图随机种子（拍出时固定→元素位置一次定死，不逐帧漂移）
    uint8_t  progLive;   // 活动作层 0-4（无/云飘/飞鸟掠/海水波动/信号灯明灭）
    uint8_t  jolt;       // 轻拍抖动剩余帧（画面短促震动，老电视被拍的既视感）
    // ── 省电模式（闲置自动进入；按键/拍打退出）──
    bool     powerSave;  // 省电模式进行中
    uint32_t idleMs;     // 闲置计时（无用户输入毫秒）
    uint8_t  saveFx;     // 省电渐变过渡帧（进=2, 出=1 过场置 2）
    uint8_t  saveBri;    // 渐变目标亮度（过渡用）
    // （v0.0.17 删掉死字段 saveFrameRate：全树无引用，且注释停在旧的 3fps 口径）
};

// 雪花变体（A 类 0-13；颜色换 LUT，性格用注入率/采样/偏移表达）
enum { SN_NORMAL, SN_BRIGHT, SN_SPARSE, SN_DENSE, SN_WHITE, SN_RED,
       SN_COARSE, SN_FINE,   SN_TWOTIER, SN_BREATH, SN_CLUMP,
       SN_DRIFT,  SN_DARK,   SN_VBAR };
// 文字变体（B 类 0-11）
enum { TX_NORMAL, TX_MORPH, TX_CLOCK, TX_JITTER, TX_FLICK,
       TX_TYPEWR, TX_BARS,   TX_CHAN,  TX_GHOST,  TX_SPLIT,
       TX_MELT,   TX_PIXEL };
// 扭曲变体（C 类 0-22）
enum { WR_NONE, WR_TEAR, WR_FLIP, WR_SCAN, WR_POWER, WR_FADE, WR_BAND,
       WR_HOLD, WR_VROLL, WR_FOLD, WR_SHAKE, WR_ANT,
       WR_WAVE, WR_ARC,  WR_PINC,  WR_MIRROR, WR_SPLICE, WR_ZOOM,
       WR_INVERT, WR_VIGN, WR_LBOX, WR_FOCUS, WR_ECHO };  // 23 种
static GlitchState g_g;

// 撕裂 tmp：static，勿放大栈（8.37 族；480B）
static uint16_t s_tealTmp[SCR_W];
uint16_t s_fbTmp[SCR_W * SCR_H];   // 整帧重采样缓冲（64KB，仅重采样类扭曲用；breath.h 共享）
static void progLiveDraw(uint16_t* fb, int te, uint32_t seed);   // 前向（主段活动作层）

// 多套雪花色板（琥珀系变体：标准/亮白/暖红）——同构 LUT，渲染端只换指针
static uint16_t g_lutW[256];    // 亮白噪（信号过曝）
static uint16_t g_lutR[256];    // 暖红噪（信号偏色）

static void buildLutAlt() {     // 亮白: 220,214,170 → 255,252,240；暖红: 偏 R
    for (int i = 0; i < 256; i++) {
        uint8_t w = (uint8_t)((220 * i) / 255);
        uint8_t r = PAL_DARK_R + (uint8_t)((255 - PAL_DARK_R) * i / 255);
        uint8_t g = PAL_DARK_G + (uint8_t)((120 - PAL_DARK_G) * i / 255);
        uint8_t b = PAL_DARK_B + (uint8_t)((30  - PAL_DARK_B) * i / 255);
        g_lutW[i] = swap16(rgb565(w, w, (uint8_t)(w - (w >> 4))));
        g_lutR[i] = swap16(rgb565(r, g, b));
    }
}

// ── 去重记忆（recent-history ring）：主体 4 槽/背景 2 槽/天体 2 槽 ──
// 抽新值：若命中最近历史则步进+1 偏移（有界、确定、分布仍均匀）；保证连续 N 下不重
static uint8_t s_histSubj[4], s_histBg[2], s_histCel[2];

// ── 主体表：id = 数组下标（0-37），cat = 声学偏置类别（0自然/1文明/2神话/3科幻）──
// s_poolSubj：当前摇奖池大小（画法分批上线时渐进扩容，最终 38）
struct ProgSubj { const char* name; uint8_t cat; };
static const ProgSubj s_subj[39] = {
    {"山峦双峰",0},{"海平落日",0},{"城市天际线",0},{"星野驼影",0},{"沙漠",0},
    {"森林",0},{"湖影",0},{"云海",0},{"月球",0},{"电线杆",0},
    {"TEMP 天气预报",0},{"ALERT 紧急广播",0},{"极光雪原",0},{"雷暴平原",0},
    {"金字塔群",1},{"巨石阵",1},{"长城烽火台",1},{"帕特农神庙",1},
    {"吴哥窟塔群",1},{"富士山+鸟居",1},{"复活节岛石像",1},{"清真寺穹顶",1},
    {"云中神龙",2},{"天空巨鲸",2},{"羽蛇神",2},{"树人巨影",2},
    {"独眼巨人",3},{"飞碟母舰",3},{"航天飞机",3},{"火箭发射台",3},
    {"人造卫星",3},{"月球基地",3},{"外星城市",3},{"巨型机器人",3},
    {"月球着陆器",3},{"外星方碑",3},{"轨道空间站",3},{"外星图腾柱",3},
    {"AI 频道",4},
};
static uint8_t s_poolSubj = 39;    // 摇奖池大小：38 剪影 + TEMP/ALERT + AI 频道

// ── 手绘符文点阵（5×7，每字符 5 字节，bit 0=顶）──
// 台标外星款 / 稀有外星字母表 / special6 外星灯牌 共用
static const uint8_t s_runes[4][5] = {
    {0x04, 0x0A, 0x11, 0x0A, 0x04},   // 菱形 ◆（外星眼）
    {0x0E, 0x15, 0x15, 0x0A, 0x04},   // 三角目
    {0x11, 0x0A, 0x04, 0x0A, 0x11},   // 沙漏 X
    {0x1F, 0x02, 0x04, 0x08, 0x1F},   // 折线山
};
static void psetRune(uint16_t* fb, int x, int y, int idx, uint16_t c) {
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (s_runes[idx][col] & (1 << row))
                psPx(fb, x + col, y + row, c);
}

static void glitchInit() {
    g_g.tearRow = 0; g_g.tearLen = 0; g_g.tearShift = 0; g_g.tearT = 0;
    g_g.tearCd = 60 + (uint16_t)(fastRand() % 120);          // 首次 4~8s
    g_g.jumpT = 120 + (uint16_t)(fastRand() % 90);
    g_g.jumpHold = 0;
    g_g.active = 0; g_g.snow = 0; g_g.text = 0; g_g.warp = 0;
    g_g.t = 0; g_g.t0 = 0; g_g.p = 0;
    g_g.ms = millis() + 40000u + (uint32_t)(fastRand() % 110000);   // 首次 40~150s
    g_g.special = 0; g_g.spT = 0; g_g.spT0 = 0;
    g_g.nextSp = millis() + 20000u + (uint32_t)(fastRand() % 90000); // 首次 20~110s
    g_g.freq = 0; g_g.dtmfT = 0;
    g_g.tapT = 0; g_g.tapT0 = 0; g_g.tapShock = 0;
    g_g.tapCd = 0; g_g.tapLast = 0;
    // 拍出信号
    g_g.progT = 0; g_g.progT0 = 0; g_g.progBg = 0; g_g.progSubj = 0; g_g.progCh = 1;
    g_g.progInfo = 0; g_g.progTex = 0; g_g.progEnter = 0; g_g.progMain = 0; g_g.progExit = 0;
    g_g.progCel = 0; g_g.progRare = 0; g_g.progSeed = 0;
    g_g.progLive = 0; g_g.jolt = 0;
    s_histSubj[0] = s_histSubj[1] = s_histSubj[2] = 255;   // 255 = 未用槽
    s_histBg[0] = s_histBg[1] = 255;
    s_histCel[0] = s_histCel[1] = 255;
    // 省电模式
    g_g.powerSave = false; g_g.idleMs = 0;
    g_g.saveFx = 0; g_g.saveBri = 0;
}

// ════════════════════════════════════════════════════
// 「拍电视」：IMU 拍打检测 → 两根扫描线从顶滚到底（CRT 经典症状）
// 触发：>1.8g 冲击双帧确认（手抖 ±1.2g 不误触）；单帧 >4g 巨震立即触发；
//      冷却 3s 防连拍 spam
// ════════════════════════════════════════════════════
static uint32_t s_tapPollMs = 0;              // IMU 轮询节流（省电降频）
static void powerSaveExit();                  // 前向（loop 唤醒用）

// 去重抽取：若命中最近历史则步进+1 偏移（有界、确定、分布仍均匀）；保证连续 N 下不重
static uint8_t recencyPick(uint8_t n, uint8_t* hist, uint8_t slots) {
    uint8_t cand = (uint8_t)(fastRand() % n);
    bool dup;
    do {
        dup = false;
        for (uint8_t j = 0; j < slots; j++)
            if (hist[j] == cand) { dup = true; break; }
        if (dup) cand = (uint8_t)((cand + 1) % n);     // 步进偏移（最多 n-1 次必出新）
    } while (dup);
    for (uint8_t j = slots - 1; j > 0; j--) hist[j] = hist[j - 1];   // 挤入历史
    hist[0] = cand;
    return cand;
}

// 拍中 → 掷骰：50% 扫描线 / 50% 节目（1:1，力度不分级）
// forceProg=true：切台——节目进行中再拍，强制换新节目（去重记忆仍生效）
static void tapTrigger(bool forceProg = false) {
    g_g.tapLast = millis();
    if (forceProg || (fastRand() & 1)) {       // 1:1 均分（切台强制出节目）
        {   // 随机节目（多轴随机组合，关键轴去重）
            g_g.progT = g_g.progT0 = 90;                    // 6s @15fps
            g_g.progBg    = recencyPick(6, s_histBg, 2);    // 背景 0-5（最近2不重）
            g_g.progSubj  = recencyPick(s_poolSubj, s_histSubj, 4); // 主体 0-38（最近4不重）
            g_g.progCh    = 1 + (uint8_t)(fastRand() % 13); // 频道 1-13
            g_g.progInfo  = (uint8_t)(fastRand() % 9);      // 台标款式 0-8（8=AI 台标）
            if (g_g.progSubj == 38) g_g.progInfo = 8;       // AI 频道固定 AI 台标
            g_g.progTex   = (uint8_t)(fastRand() % 3);      // 质感 0-2
            g_g.progEnter = (uint8_t)(fastRand() % 3);      // 收拢 0-2
            g_g.progMain  = (uint8_t)(fastRand() % 3);      // 主段 0-2
            g_g.progExit  = (uint8_t)(fastRand() % 3);      // 溶回 0-2
            g_g.progCel   = recencyPick(12, s_histCel, 2);  // 天体 0-11（最近2不重）
            g_g.progLive  = (uint8_t)(fastRand() % 5);      // 活动作层 0-4
            // 稀有事件：节目内的 20%（对外≈10%）；其中 1-5 常见，6-8 彩蛋池（更稀）
            uint32_t rr = fastRand() % 100;
            if (rr < 20) {
                // 85% 常见稀有：1-5（测试卡/彩条/灰阶/TELETEXT/马赛克）+ 9-11（外星三件套）
                // 15% 彩蛋池（galcoll/backrooms/RLCD）
                if (fastRand() % 100 < 15) g_g.progRare = 6 + (uint8_t)(fastRand() % 3);
                else {
                    uint8_t c = (uint8_t)(fastRand() % 8);
                    g_g.progRare = (c < 5) ? (uint8_t)(c + 1) : (uint8_t)(c + 4);
                }
            } else {
                g_g.progRare = 0;
            }
            g_g.progSeed  = fastRand();                     // 固定底图种子（元素位置不再漂移）
            // 声学轴 v0.0.16：按主体类别抽档（70% 偏置/30% 全随机）+ 拍打锁定音弧
            g_g.progSnd = psPick(s_subj[g_g.progSubj].cat);
            psSetSnd(g_g.progSnd);   // v0.0.17：接线（此前只抽不用 = 声学轴四档死代码）
            psArcStart(millis());
        }
    } else {
        g_g.tapT = g_g.tapT0 = 18;                          // ~1.2s 扫描线
    }
}

static void tapTick() {
    if (g_g.powerSave) return;                 // 省电：IMU 已硬件休眠，不做拍打唤醒（按键唤醒）
    // 节流：66ms（≈渲染帧率）
    {
        uint32_t n = millis();
        if (n - s_tapPollMs < 66u) return;
        s_tapPollMs = n;
    }
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;         // IMU 未就绪
    float am = sqrtf(ax * ax + ay * ay + az * az);       // 含重力 ~1g
    if (g_g.progT > 0) {                                 // 节目进行中：再拍 = 换台
        if (am > 1.8f && millis() - g_g.tapLast > 400) { // 须真实拍到 + 跳过 400ms 防误触窗
            g_g.active = 0; g_g.special = 0;             // 清残留场景
            tapTrigger(true);                            // 强制换新节目
            M5.Speaker.tone(50, 200, -1, true);          // 换台"嗡"声
        }
        return;
    }
    if (am <= 1.8f) { g_g.tapShock = 0; return; }        // 未达拍打阈值
    if (millis() - g_g.tapLast < 3000) return;           // 冷却 3s（防连拍 spam）
    if (++g_g.tapShock >= 2) {                           // 双帧确认（防误触）
        g_g.tapShock = 0;
        tapTrigger();                                    // 拍一下 → 50% 扫描线 / 50% 节目
        M5.Speaker.tone(50, 200, -1, true);              // 拍打"嗡"声（低频闷响）
    }
}

// 扫描线渲染：两根亮线带从顶滚到底，线带内行错位扭曲（线圈受震感）
static void tapApply(uint16_t* fb) {
    if (g_g.tapT == 0) return;
    int t = g_g.tapT0 - g_g.tapT;                         // 已走帧 0..17
    float k = t / (float)g_g.tapT0;                       // 0→1 进度
    int yc = (int)(k * (SCR_H + 16)) - 8;                 // 扫描线的纵坐标
    // 线带（上下 6px 内）：行错位扭曲 + 两条亮线
    for (int y = yc - 6; y <= yc + 6; y++) {
        if (y < 0 || y >= SCR_H) continue;
        int sh = (int)((fastRand() % 9) - 4);             // 每行随机横向错位
        if (sh == 0) continue;
        memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
        for (int x = 0; x < SCR_W; x++) {
            int sx = x + sh;
            fb[y * SCR_W + x] = (sx >= 0 && sx < SCR_W) ? s_tealTmp[sx] : 0;
        }
    }
    // 两条亮线（主线+副线，CRT 震瞬间的双扫描线特征）
    for (int yy = yc - 1; yy <= yc + 1; yy++) {
        if (yy < 0 || yy >= SCR_H) continue;
        for (int x = 0; x < SCR_W; x += 2) fb[yy * SCR_W + x] = swap16(rgb565(255, 235, 170));
    }
    for (int yy = yc + 4; yy <= yc + 5; yy++) {
        if (yy < 0 || yy >= SCR_H) continue;
        for (int x = 1; x < SCR_W; x += 2) fb[yy * SCR_W + x] = swap16(rgb565(200, 160, 90));
    }
}

// 每渲染帧调用一次（跳帧冻结期间不推进——和 atmos 一致）
static void glitchUpdate() {
    // 省电模式：撕裂/跳帧可保留（1fps 时钟下自然变慢=微呼吸，无雪花可看），
    // 但故障场景与特殊事件冻结（省电=安静雪花，不触发扭曲+音频）
    if (g_g.powerSave) {
        if (g_g.active)  g_g.active = 0;                    // 立即冻结场景
        if (g_g.special) g_g.special = 0; g_g.spT = 0;
        return;                                             // 撕裂/跳帧仍跑（低速）
    }

    // ---- 撕裂触发（4~12s）——仅在无故障场景时运作----
    if (g_g.tearT > 0) {
        g_g.tearT--;
    } else {
        if (g_g.tearCd > 0) g_g.tearCd--;
        else {
            g_g.tearRow = 8 + (int)(fastRand() % (SCR_H - 24));
            g_g.tearLen = 3 + (int)(fastRand() % 6);           // 3~8 行
            g_g.tearShift = 4 + (int)(fastRand() % 9);          // 4~12 px
            g_g.tearT = 3 + (int)(fastRand() % 6);              // 0.2~0.5s
            g_g.tearCd = 60 + (uint16_t)(fastRand() % 120);     // 下次 4~12s
        }
    }

    // ---- 跳帧（8~20s；jumpHold 由 loop 递减，此处绝不清零 — atmos 死锁教训）----
    if (g_g.jumpT == 0) {
        g_g.jumpHold = 2;                                        // ≈133ms 冻结
        g_g.jumpT = 120 + (uint16_t)(fastRand() % 180);
    } else {
        g_g.jumpT--;
    }

    // ── 故障场景调度（40~150s 随机点，三元素各抽一组合）──
    if (g_g.active) {
        if (g_g.t > 0) g_g.t--;
        if (g_g.t == 0) g_g.active = 0;                          // 场景结束
        // 场景进度 p：0→255 线性推进（扭曲 4假关机/5渐弱/6黑带 依赖）
        uint32_t denom = (g_g.t0 > 0) ? g_g.t0 : 1;              // 触发时总帧数
        uint32_t prog = (uint32_t)(denom - (uint32_t)g_g.t) * 255 / denom;
        g_g.p = (uint8_t)((prog > 255) ? 255 : prog);
    }
    if (!g_g.active && millis() >= g_g.ms) {
        g_g.active = 1;
        g_g.snow = (uint8_t)(fastRand() % 14);   // 0..13 雪花类型
        g_g.text = (uint8_t)(fastRand() % 12);   // 0..11 文字方式
        g_g.warp = (uint8_t)(fastRand() % 23);   // 0..22 画面扭曲
        g_g.t = 22 + (uint8_t)(fastRand() % 24); // 场景时长 1.5~3s
        g_g.t0 = g_g.t;
        g_g.p = 0;
        g_g.ms = millis() + 40000u + (uint32_t)(fastRand() % 110000);   // 下次 40~150s
    }

    // ── 特殊事件池（20~110s 随机点；与故障场景错开：场景进行中不触发）──
    if (g_g.special) {
        if (g_g.spT > 0) g_g.spT--;
        if (g_g.spT == 0) g_g.special = 0;
    }
    if (!g_g.special && !g_g.active && millis() >= g_g.nextSp) {
        g_g.special = 1 + (uint8_t)(fastRand() % 6);   // 1..6 特殊事件（6=外星符文灯牌）
        g_g.spT = g_g.spT0 = (g_g.special == 2) ? 12 : 20 + (uint8_t)(fastRand() % 18);
        g_g.nextSp = millis() + 20000u + (uint32_t)(fastRand() % 90000); // 下次 20~110s
    }
}

// ── 手绘 5x7 点阵字体（移植自 backrooms atmos.cpp，真机验证）──
// 全部文字直写 fb（原生坐标），与雪花同一坐标系——drawString 走 canvas
// 逻辑坐标，pushSprite 时再变换一次 → 混用必偏移（v0.2 NO SIGNAL 偏心 bug）
// 索引: 0-9, 10=R 11=E 12=C 13=: 14=# 15=B 16=A 17=T 18=% 19=O 20=M 21=- 22=.
//       23=N 24=S 25=I 26=G 27=L 28=V 29=Y 30=H 31=W 32=P 33=Q 34=X 35=Z 36=K
static const uint8_t FONT57[37][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, // :
    {0x0A,0x15,0x0A,0x15,0x0A,0x15,0x0A}, // # 乱码
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, // B
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A（原字模误复制自E→看起来像L，v0.0.1修正）
    {0x1F,0x04,0x04,0x04,0x04,0x0C,0x08}, // T
    {0x11,0x11,0x11,0x0A,0x04,0x00,0x04}, // %
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x11,0x11,0x13,0x15,0x19,0x11,0x11}, // M
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x00,0x00,0x0C,0x0C,0x00,0x00,0x00}, // .
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, // I
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, // G
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x0A,0x04,0x04,0x04,0x04,0x04}, // Y
    {0x11,0x11,0x1F,0x11,0x11,0x11,0x11}, // H  ← v0.0.17 补：台标 "CH n" 曾静默缺字显示成 "C n"
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W  ← "NOW SHOWING" / 呼号 "WQ-3"
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P  ← "TEMP" / 呼号 "LP-2"
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q  ← 呼号 "QR-9" / "WQ-3"
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X  ← 呼号 "ZXN"
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z  ← 呼号 "ZXN"
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K  ← 呼号 "KH-TV"
};

static uint8_t glyphIdx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case 'R': return 10; case 'E': return 11; case 'C': return 12;
        case ':': return 13; case '#': return 14; case 'B': return 15;
        case 'A': return 16; case 'T': return 17; case '%': return 18;
        case 'O': return 19; case 'M': return 20; case '-': return 21;
        case '.': return 22; case 'N': return 23; case 'S': return 24;
        case 'I': return 25; case 'G': return 26; case 'L': return 27;
        case 'V': return 28; case 'Y': return 29;
        case 'H': return 30; case 'W': return 31; case 'P': return 32;
        case 'Q': return 33; case 'X': return 34; case 'Z': return 35;
        case 'K': return 36;
        default:  return 255;   // 空格等：跳过占位
    }
}

// scale=1 → 5x7（原生）；scale=3 → 15x21 大字
static void drawChar57(uint16_t* fb, int x, int y, uint8_t ci,
                       uint16_t fgSw, uint16_t bgSw, int scale) {
    if (ci == 255 || x + 5 * scale > SCR_W || y + 7 * scale > SCR_H) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = FONT57[ci][row];
        for (int sy = 0; sy < scale; sy++) {
            uint16_t* p = &fb[(y + row * scale + sy) * SCR_W + x];
            for (int col = 0; col < 5; col++) {
                uint16_t v = (bits & (0x10 >> col)) ? fgSw : bgSw;
                for (int sx = 0; sx < scale; sx++) p[col * scale + sx] = v;
            }
        }
    }
}

static int strWidth57(const char* s, int scale) {   // 含尾随字距
    return strlen(s) * (5 + 1) * scale - scale;      // 每字宽+gap(=scale)
}

static void drawStr57(uint16_t* fb, int x, int y, const char* s,
                      uint16_t fgSw, uint16_t bgSw, int scale) {
    for (; *s; s++) {
        if (*s != ' ') drawChar57(fb, x, y, glyphIdx(*s), fgSw, bgSw, scale);
        x += (5 + 1) * scale;
    }
}

// ── NO SIGNAL 常驻招牌（v0.0.3：闪烁事件可升级为"时间闪现"）──
// 文字永远居中直写 fb；静噪彩蛋=雪花爆亮，招牌不动。
// 生命感：每 ~6s 整块暗一帧；暗闪事件有概率升级为"时间闪现"——
// 招牌本体这一瞬被时钟顶替（NO SIGNAL → 23:47 → 还原），泄露机器还活着。
static uint8_t g_legFlickT = 0;      // >0 → 本帧招牌整体压暗
static uint8_t g_legTimeT = 0;       // >0 → 本帧招牌换成大时钟（时间闪现）
static uint8_t g_legGlitch = 255;    // 乱码字符位（0-7 = "NO SIGNAL" 的第 n 个非空格字符）
static uint32_t g_legGlitchT = 0;    // 下次乱码 millis
static uint32_t g_legGlitchEnd = 0;  // 乱码结束 millis
static uint16_t g_legFlickCd = 90;   // 暗闪倒计时（帧）

// 时钟字符串（clockTick 每秒刷新；drawLegend 时间闪现读取本份）
static uint32_t s_clockMs = 0;
static char s_clock[8] = "";

// drawLegend 文字变体：textMode 0=正常(含时间闪现) 1=全乱码 2=持续时间 3=抖动 4=闪烁
static uint32_t s_frameCounter = 0;   // 闪烁用帧计数

// ════════════════════════════════════════════════════
// 白噪声音频层（静电电视的"沙沙"声）
// M5StickS3 内置 ES8311 · I2S_NUM_0 · 22050Hz 立体声输出
// 架构：0.5s 随机 int16 样本 playRaw(repeat=~0u) 无限循环，
//       音量用 master volume 每帧联动视觉故障场景
// ════════════════════════════════════════════════════
enum AppState { ST_SPLASH, ST_SYNC, ST_APP };   // 提前声明（noiseTick 依赖）
static AppState g_state = ST_SPLASH;            // 初始=启动界面
#define NOISE_HZ   22050
#define NOISE_LEN  (NOISE_HZ / 2)             // 0.5 秒 → 11025 样本 (22KB)
static int16_t s_noise[NOISE_LEN];
static bool    s_noiseStarted = false;

// 音量档位（master 0~255，平方律：需真机校准大致档位）
static const uint8_t VOL_IDLE   = 40;   // 常态沙沙（低）
static const uint8_t VOL_SPIKE  = 200;  // 静噪爆亮
static const uint8_t VOL_SCAN   = 120;  // 搜台抖动基准
static const uint8_t VOL_GLITCH = 90;   // 普通故障提响

// 生成白噪声样本：均匀随机 ±2^14（留 4 倍余量防削波）
static bool g_muted = false;              // A 键静音开关（APP 界面）
static void noiseBuild() {
    for (int i = 0; i < NOISE_LEN; i++)
        s_noise[i] = (int16_t)((int32_t)(fastRand() & 0x3FFF) - 0x2000);
}

// 无限循环播放（首次或 unmute 重启）
static void noiseStart() {
    if (g_muted) return;                    // 静音状态不播放
    if (!s_noiseStarted) {
        M5.Speaker.playRaw(s_noise, NOISE_LEN, NOISE_HZ, false, ~0u);
        s_noiseStarted = true;
    }
              // mono 数据，is_stereo=false → 内部自动复制双声道
}

// 每帧调用：按视频故障场景调整白噪声音量 + D 类音效
//  不活跃=常态沙沙；假关机=静音(关机)+砰声；亮噪=爆亮；
//  搜台=抖动+扫频音；渐弱=随画面同步衰减；其余=轻微提响
static void noiseTick() {
    if (g_state != ST_APP) { M5.Speaker.setVolume(0); return; }  // 开机画面/对时静音
    if (g_g.powerSave) {                             // 省电模式：无条件停噪+静音
        // ⚠️ 不依赖 saveFx 归零（过渡可能被打断/卡住——旧版因此省电仍响白噪 bug）
        // 音频已在 powerSaveEnter() 用 Speaker.end() 彻底关闭（功放+I2S），
        // 此处只需复位播放标志，不再触碰 Speaker（end 状态下调用无意义）
        s_noiseStarted = false;
        return;
    }
    noiseStart();
    uint8_t v = VOL_IDLE;
    if (g_g.active) {
        switch (g_g.warp) {
        case WR_POWER: {
            v = 0;                                           // 假关机=彻底静音
            static bool thumpDone = false;                   // D3 关机"砰"声（一次）
            if (g_g.p > 150 && !thumpDone) {
                thumpDone = true;
                M5.Speaker.tone(58, 260, -1, true);          // 低频 thump
            }
            if (g_g.p < 20) thumpDone = false;               // 场景重置允许下次
            break;
        }
        case WR_FADE:  v = (uint8_t)(VOL_SPIKE * (255u - g_g.p) / 255u); break; // 渐弱同步衰减
        case WR_SCAN: {                                      // 搜台：抖动 + D5 扫频音
            v = VOL_SCAN + (uint8_t)(fastRand() % 50);
            g_g.freq = (uint8_t)(g_g.freq + 24);             // 相位推进
            float f = 280.0f + (g_g.freq & 63) * 22.0f;      // 280~1666Hz 上扫
            M5.Speaker.tone(f, 70, -1, true);                // 换台"嗖"声（短音叠在白噪上）
            break;
        }
        default: v = (g_g.snow == SN_BRIGHT) ? VOL_SPIKE : VOL_GLITCH; break; // 亮噪爆亮
        }
    }
    if (g_g.special) {                                       // 特殊事件音效（D7 电话/D8 天线）
        if (g_g.special == 3) {                              // 电话干扰：DTMF 双音步进
            g_g.dtmfT++;
            if ((g_g.dtmfT & 3) == 0)
                M5.Speaker.tone((g_g.dtmfT & 8) ? 1209 : 697, 90, -1, true);
            v = VOL_SCAN;
        } else if (g_g.special == 4) {                       // 天线松动：噼啪爆音
            v = VOL_GLITCH + (uint8_t)(fastRand() % 40);
        } else if (g_g.special == 2) {                       // 信号恢复：穿插微弱白噪
            v = VOL_IDLE + 10;
        }
    }
    M5.Speaker.setVolume(g_muted ? 0 : v);                   // 静音开关兜底
}
static void drawLegend(uint16_t* fb, bool flare, uint8_t textMode) {
    const int S = 3;                                  // ×3 → 字高 21px
    char msg[10]; strcpy(msg, "NO SIGNAL");

    // 时间闪现：招牌本体这一瞬换成时钟（冒号随秒闪烁）
    bool timeFlash = (g_legTimeT > 0);
    static char chanNo[6] = "";                       // TX_CHAN 频道呼号（首帧生成）

    // ── 文字变体 ──
    switch (textMode) {
    case 1: {                       // 全乱码：每个非空格字符变 #/数字
        for (char* q = msg; *q; q++) if (*q != ' ') {
            *q = (fastRand() % 2) ? '#' : (char)('0' + (fastRand() % 10));
        }
        break;
    }
    case 2:                         // 持续时间（整场景保持，不再 1s 闪现）
        if (s_clock[0] != 0) { snprintf(msg, sizeof(msg), "%s", s_clock); timeFlash = true; }
        break;
    case 3:                         // 抖动：横向位移跳动
        break;                      // 位移在下方 sx 计算后叠加
    case 4:                         // 闪烁：奇偶帧交替显隐（灭帧只略过文字绘制）
        if ((s_frameCounter & 1) == 0) return;   // 雪花保留，仅招牌消失
        break;
    case 5: {                       // 打字机：逐字显影（场景进度 p 0→255 控制字数）
        int n = (int)(9 * (255u - g_g.p) / 255u) + 1;   // 从 1 字开始，随 p 增长
        if (n > 9) n = 9; else if (n < 1) n = 1;
        msg[n] = 0;                 // 只显示前 n 字符
        break;
    }
    case 6: {                       // 信号条：8 根测试彩条（替代文字，成 8 竖条）
        uint32_t barC[8] = { 0xC8A858, 0xE8E8C8, 0x98C858, 0x58C8B8,
                             0xC858B8, 0xD85858, 0x5858C8, 0x181008 };
        int bx = (SCR_W - 128) / 2, by = (SCR_H - 48) / 2;
        for (int b = 0; b < 8; b++)
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 16; x++) {
                    int px = bx + b * 16 + x, py = by + y;
                    if (px < SCR_W && py < SCR_H) fb[py * SCR_W + px] = swap16(barC[b]);
                }
        return;                     // 彩条替代招牌，不画字
    }
    case 7:                         // 频道呼号：CH 数字随场景持续显示
        if (chanNo[0] == 0) snprintf(chanNo, sizeof(chanNo), "C:%02d", fastRand() % 100);
        snprintf(msg, sizeof(msg), "%s", chanNo);
        break;
    case 8: {                       // 重影：横向拖出残影（ghosting）
        for (int g = 1; g <= 3; g++) {
            int gx = (SCR_W - strWidth57(msg, S)) / 2 + g * 5;
            drawStr57(fb, gx, (SCR_H - 7 * S) / 2, msg,
                      swap16(rgb565(90 - g * 20, 70 - g * 14, 30 - g * 6)), 0, S);
        }
        break;                      // 残影画完，主字照常落下方
    }
    case 9: {                       // 上下分裂：上下两半错开（场同步故障）
        int sw9 = strWidth57(msg, S);
        int sx9 = (SCR_W - sw9) / 2, sy9 = (SCR_H - 7 * S) / 2;
        // 直接平移：上半行左移 4px，下半行右移 4px
        // （v0.0.17 删掉一段自我 memcpy 的死循环——它一个像素都没改）
        for (int y = sy9; y < sy9 + 10 && y < SCR_H; y++) if (y >= 0) {
            memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
            for (int x = 0; x < SCR_W - 4; x++) fb[y * SCR_W + x] = s_tealTmp[x + 4];
        }
        for (int y = sy9 + 10; y < sy9 + 21 && y < SCR_H; y++) if (y >= 0) {
            memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
            for (int x = SCR_W - 1; x >= 4; x--) fb[y * SCR_W + x] = s_tealTmp[x - 4];
        }
        break;
    }
    case 10: {                      // 融化：像素向下拉长滴落（VHS 拖尾）
        int sy10 = (SCR_H - 7 * S) / 2;
        for (int drop = 0; drop < 5; drop++) {          // 5 个随机列下拉 20px
            int cx = (int)(fastRand() % SCR_W);
            for (int y = sy10 + 21; y < sy10 + 42 && y < SCR_H; y++)
                fb[y * SCR_W + cx] = fb[(y - 1) * SCR_W + cx];   // 每行复制上一行
        }
        break;
    }
    case 11: {                      // 像素碎：文字区随机散点雪花（碎片化）
        int sy11 = (SCR_H - 7 * S) / 2, sw11 = strWidth57(msg, S);
        for (int k = 0; k < 60; k++) {
            int px = (SCR_W - sw11) / 2 + (int)(fastRand() % sw11);
            int py = sy11 + (int)(fastRand() % (7 * S));
            if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
                fb[py * SCR_W + px] = g_lut[fastRand() % 256];
        }
        break;
    }
    default:
        if (timeFlash) {            // ← 原有逻辑（mode 0 专用）
            if (s_clock[0] == 0) { timeFlash = false; msg[0] = 0; }
            else snprintf(msg, sizeof(msg), "%s", s_clock);
        }
        break;
    }

    if (g_legGlitch != 255 && g_legGlitch < 8) {
        char* p = msg; int idx = -1;              // 找第 g_legGlitch 个非空格字符
        for (char* q = msg; *q; q++) { if (*q != ' ') idx++; if (idx == g_legGlitch) { *q = '#'; break; } }
    }
    int sw = strWidth57(msg, S);                  // "NO SIGNAL"162px / "23:47"87px
    int sx = (SCR_W - sw) / 2;                    // 原生坐标真居中（已验证）
    if (textMode == 3) sx += (int)(fastRand() % 7) - 3;   // 抖动 ±3px
    int sy = (SCR_H - 7 * S) / 2;
    if (textMode == 5) {                          // 打字机：字串右端原子键，不垫底盒
        drawStr57(fb, sx, sy, msg, swap16(COL_NOSIG_FG), 0, S);
        s_legX0 = sx - S; s_legY0 = sy - S;       // v0.0.17：提前 return 也要记矩形，否则呼吸层按旧矩形调制
        s_legX1 = sx + sw + S; s_legY1 = sy + 7 * S + S;
        return;
    }
    bool dimmed = (g_legFlickT > 0);
    uint16_t fgC = timeFlash ? rgb565(180, 140, 60)                   // 时间闪现=被泄露的真相，暗一档
                 : dimmed  ? rgb565(120, 98, 45) : COL_NOSIG_FG;
    if (textMode == 1) fgC = rgb565(180, 130, 70);   // 乱码微暗，区分故障态
    uint16_t bgC = flare ? rgb565(24, 15, 6) : rgb565(18, 11, 4);
    uint16_t bg = swap16(bgC);
    // 垫底盒（字区外扩一格，噪点上保证可读）
    for (int y = sy - S; y < sy + 7 * S + S && y < SCR_H; y++) {
        if (y < 0) continue;
        for (int x = sx - S; x < sx + sw + S && x < SCR_W; x++)
            if (x >= 0) fb[y * SCR_W + x] = bg;
    }
    drawStr57(fb, sx, sy, msg, swap16(fgC), bg, S);
    // 记录灯牌矩形（呼吸层 OSD 调制区；次帧生效即可）
    s_legX0 = sx - S; s_legY0 = sy - S;
    s_legX1 = sx + sw + S; s_legY1 = sy + 7 * S + S;
}

static void legendTick() {
    if (g_legFlickT > 0) g_legFlickT--;
    else if (g_legTimeT > 0) {
        g_legTimeT--;
        if (g_legTimeT == 0) {                       // 时间闪现结束 → 排下一次暗闪
            g_legFlickCd = 70 + (uint16_t)(fastRand() % 80);
        }
    }
    else if (g_legFlickCd > 0 && --g_legFlickCd == 0) {
        // 暗闪事件：~25% 升级为时间闪现（持续 ~1s≈15帧），否则普通暗 1 帧
        if (g_timeValid && s_clock[0] != 0 && (fastRand() % 4) == 0) {
            g_legTimeT = 15;
        } else {
            g_legFlickT = 1;                         // 暗 1 帧（≈66ms）
            g_legFlickCd = 70 + (uint16_t)(fastRand() % 80); // 下次 ~5~9s
        }
    }
    uint32_t now = millis();
    if (g_legGlitch != 255) {                            // 乱码持续 ~0.4s
        if (now >= g_legGlitchEnd) {
            g_legGlitch = 255;
            g_legGlitchT = now + 10000u + (uint32_t)(fastRand() % 15000);  // 下次 ~10~25s
        }
    } else if (now >= g_legGlitchT) {
        // v0.0.17：计数只数非空格字符 → "NO SIGNAL" 只有 8 个（idx2 是 'S' 不是空格，
        //   原 ci==2 换位无意义地放过一个字母；%9 则让 idx8 空转）
        uint8_t ci = (uint8_t)(fastRand() % 8);
        g_legGlitch = ci;
        g_legGlitchEnd = now + 400;
    }
}

// ── 显示缓冲（canvas 定义须在 splashScreen 之前 ── 见下）──
static M5Canvas canvas(&M5.Display);   // 必须绑定父显示设备
// 画布缓冲区：手动内部 SRAM（64800B）——绕开 PSRAM cache 写冲突（Cache error: Dbus write rejected）
// （cg：节目帧全屏剧量写 PSRAM 缓冲触发 cache 拒绝偶发 panic，2026-09-07 实测定位）
static uint16_t s_fbRam[SCR_W * SCR_H] __attribute__((aligned(16)));
static uint16_t* getFb() { return (uint16_t*)canvas.getBuffer(); }

// ── 启动画面：名称+版本+按键提示（只画一帧；3s 计时/按 A 判定放 loop 状态机）──
static void splashRender() {
    uint16_t* fb = (uint16_t*)canvas.getBuffer();
    memset(fb, 0, SCR_W * SCR_H * 2);               // 黑底

    const char* name = "NO SIGNAL";
    int sw = strWidth57(name, 3);
    drawStr57(fb, (SCR_W - sw) / 2, 28, name,
              swap16(COL_NOSIG_FG), swap16(rgb565(18, 11, 4)), 3);

    const char* ver = "V0.1.0";
    sw = strWidth57(ver, 1);
    drawStr57(fb, (SCR_W - sw) / 2, 80, ver,
              swap16(rgb565(120, 98, 45)), 0, 1);

    const char* hint = "A: TIME SYNC";
    sw = strWidth57(hint, 1);
    drawStr57(fb, (SCR_W - sw) / 2, 100, hint,
              swap16(rgb565(70, 56, 26)), 0, 1);

    canvas.pushSprite(0, 0);
}

// ── 应用状态机（启动界面 → 对时界面 → 应用界面；全非阻塞）──
// （enum AppState / g_state 已在音频层上方声明）
static uint32_t g_stateT0 = 0;            // 状态进入时刻（毫秒）
static uint8_t g_bootFx = 0;              // D2 开机瞬间过场剩余帧（0=无）
static void snowStep(uint16_t* fb, int injectPct, const uint16_t* lut);  // 前向（bootFxApply 用）

// 开机过场：黑屏 → 中间亮线 → 展开成雪花（逆假关机）
static void bootFxApply(uint16_t* fb) {
    if (g_bootFx == 0) return;
    int t = g_bootFx;                                    // 0→23 递减
    float k = 1.0f - (t - 3) / 24.0f;                     // 压缩比：1 → 0.05
    if (k < 0.05f) k = 0.05f;
    int band = (int)(SCR_H * k);
    if (band < 2) band = 2;
    snowStep(fb, 8, nullptr);                            // 开机即密集雪花（信号刚来）
    int top = (SCR_H - band) / 2;
    for (int y = 0; y < SCR_H; y++)
        if (y < top || y >= top + band) memset(&fb[y * SCR_W], 0, SCR_W * 2);
}

// 进对时界面：开 AP + QR（非阻塞；由 loop 轮询 tick）
static void enterSync() {
    syncAPBegin();
    g_state = ST_SYNC;
    g_stateT0 = millis();
}

// ── 右下角隐藏时钟（时间可信后显示；极暗，凑近才看得见）──
static void clockTick() {
    if (!g_timeValid) { s_clock[0] = 0; return; }
    uint32_t now = millis();
    if (now - s_clockMs < 1000) return;
    s_clockMs = now;
    time_t lt = getLocalTimeT();
    if (lt < 1735689600) { s_clock[0] = 0; return; }
    struct tm ti;
    gmtime_r(&lt, &ti);
    snprintf(s_clock, sizeof(s_clock), "%02d:%02d", ti.tm_hour, ti.tm_min);
}
// 右下角极暗时钟已废弃：时间只经「时间闪现」顶替招牌出现（×3 大字才可读）

// ── NO SIGNAL 静噪帧：亮噪覆盖一切（招牌由 drawLegend 统一画）──
static void drawNoSignal(uint16_t* fb) {
    // 信号丢失=全频道白噪声：整体比常规雪花更亮
    uint16_t w = swap16(rgb565(250, 214, 130));
    uint16_t m = swap16(rgb565(170, 128, 48));
    uint16_t b = g_lut[0];
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint32_t t = fastRand() % 100;
        if (t < 38) { fb[i] = w; g_phos[i] = 250; }
        else if (t < 68) { fb[i] = m; g_phos[i] = 150; }
        else { fb[i] = b; g_phos[i] = 12 + (uint8_t)(fastRand() & 7); }
    }
}

// ── 雪花引擎核心：磷光衰减 + 新噪点注入（灵魂=旧点熄灭/新点亮起）──
// injectPct=注入率（正常6，稀疏2，密集12）；lut=色表（正常/亮白/暖红）
static void snowStep(uint16_t* fb, int injectPct, const uint16_t* lut) {
    const uint16_t* L = (lut ? lut : g_lut);
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint8_t p = g_phos[i];
        if (p > 4) p -= (uint8_t)(1 + ((uint32_t)p * 46) >> 8);   // ≈*0.82
        else if (p > 0) p = 0;
        if ((fastRand() % 100) < injectPct) {                     // 注入
            uint32_t t = fastRand() % 100;
            uint8_t add = (t < 55) ? 70 : (t < 90) ? 140 : 255;
            if (add > p) p = add;
        }
        g_phos[i] = p;
        fb[i] = L[p];
    }
}

// ── 雪花变体分发（A 类 0-13；均复用磷光缓冲，色表/注入率/采样方式不同）──
static void snowVariant(uint16_t* fb, uint8_t mode) {
    switch (mode) {
    case SN_BRIGHT: drawNoSignal(fb); break;                 // 1 亮噪爆闪
    case SN_SPARSE: snowStep(fb, 2, nullptr); break;         // 2 稀疏
    case SN_DENSE:  snowStep(fb, 12, nullptr); break;        // 3 密集狂舞
    case SN_WHITE:  snowStep(fb, 8, g_lutW); break;          // 4 白噪过曝
    case SN_RED:    snowStep(fb, 6, g_lutR); break;          // 5 暖红偏色
    case SN_COARSE: {                                       // 6 粗颗粒雪（2×2 块）
        for (int y = 0; y < SCR_H; y += 2) for (int x = 0; x < SCR_W; x += 2) {
            int i = y * SCR_W + x;
            uint8_t p = g_phos[i];
            if (p > 4) p -= (uint8_t)(1 + ((uint32_t)p * 46) >> 8);
            if ((fastRand() % 100) < 10) {
                uint32_t t = fastRand() % 100;
                uint8_t add = (t < 55) ? 70 : (t < 90) ? 140 : 255;
                if (add > p) p = add;
            }
            g_phos[i] = p;
            uint16_t c = g_lut[p];
            fb[i] = c;
            if (x + 1 < SCR_W) fb[i + 1] = c;
            if (y + 1 < SCR_H) { fb[i + SCR_W] = c; if (x + 1 < SCR_W) fb[i + SCR_W + 1] = c; }
        }
        break;
    }
    case SN_FINE: {                                         // 7 细沙雪（1/3 概率冷白微亮）
        uint16_t lw = swap16(rgb565(150, 148, 120));
        for (int i = 0; i < SCR_W * SCR_H; i++) {
            if ((fastRand() & 3) == 0) g_phos[i] = 60 + (uint8_t)(fastRand() % 60);
            else if (g_phos[i] > 8) g_phos[i] -= (uint8_t)(1 + ((uint32_t)g_phos[i] * 46) >> 8);
            fb[i] = (g_phos[i] > 30) ? lw : swap16(rgb565(20, 18, 10));
        }
        break;
    }
    case SN_TWOTIER: {                                     // 8 上下两段密度（天线半坏）
        for (int y = 0; y < SCR_H; y++) {
            int rate = (y < SCR_H / 2) ? 3 : 11;
            for (int x = 0; x < SCR_W; x++) {
                int i = y * SCR_W + x;
                uint8_t p = g_phos[i];
                if (p > 4) p -= (uint8_t)(1 + ((uint32_t)p * 46) >> 8);
                if ((fastRand() % 100) < rate) {
                    uint32_t t = fastRand() % 100;
                    uint8_t add = (t < 55) ? 70 : (t < 90) ? 140 : 255;
                    if (add > p) p = add;
                }
                g_phos[i] = p;
                fb[i] = g_lut[p];
            }
        }
        break;
    }
    case SN_BREATH: {                                      // 9 雪花呼吸（整体亮度随时间起伏）
        static uint8_t ph = 0;
        ph++;
        // v0.0.17：原式把 (int16_t) 加在括号外层 → 截断恒为 0 → mult 恒 128（"呼吸"其实是固定减半）
        uint8_t mult = (uint8_t)(128 + (int)(100.0f * (0.5f + 0.5f * sinf(ph * 0.12f))));  // 128..228
        for (int i = 0; i < SCR_W * SCR_H; i++) {
            uint8_t p = g_phos[i];
            if (p > 4) p -= (uint8_t)(1 + ((uint32_t)p * 46) >> 8);
            if ((fastRand() % 100) < 6) {
                uint32_t t = fastRand() % 100;
                uint8_t add = (t < 55) ? 70 : (t < 90) ? 140 : 255;
                if (add > p) p = add;
            }
            uint8_t n = (uint8_t)(((uint32_t)p * mult) >> 8);
            g_phos[i] = n;
            fb[i] = g_lut[n];
        }
        break;
    }
    case SN_CLUMP: {                                       // 10 雪花结块（行向聚集 — 滚雪）
        for (int y = 0; y < SCR_H; y++) {
            bool clump = ((y * 7 + (int)(fastRand() % 5)) % 19) < 3;   // ~15% 行是高密度带
            int rate = clump ? 22 : 4;
            for (int x = 0; x < SCR_W; x++) {
                int i = y * SCR_W + x;
                uint8_t p = g_phos[i];
                if (p > 4) p -= (uint8_t)(1 + ((uint32_t)p * 46) >> 8);
                if ((fastRand() % 100) < rate) {
                    uint32_t t = fastRand() % 100;
                    uint8_t add = (t < 55) ? 90 : (t < 90) ? 160 : 255;
                    if (add > p) p = add;
                }
                g_phos[i] = p;
                fb[i] = g_lut[p];
            }
        }
        break;
    }
    case SN_DRIFT: {                                       // 11 风雪漂移（整体向下流动）
        static int driftAcc = 0;
        driftAcc += 2;                                     // 每帧下移 2px（旧点拖影）
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++) {
                int i = y * SCR_W + x;
                uint8_t p = g_phos[i];
                if (p > 4) p -= (uint8_t)(2 + ((uint32_t)p * 46) >> 8);   // 更快衰减=拖影感
                if ((fastRand() % 100) < 8) {                             // 更高注入=风雪
                    uint32_t t = fastRand() % 100;
                    uint8_t add = (t < 55) ? 90 : (t < 90) ? 150 : 255;
                    if (add > p) p = add;
                }
                g_phos[i] = p;
            }
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++) {
                int src = ((y - driftAcc) % SCR_H + SCR_H) % SCR_H;       // 采样上移
                fb[y * SCR_W + x] = g_lut[g_phos[src * SCR_W + x]];
            }
        break;
    }
    case SN_DARK:   snowStep(fb, 2, nullptr); break;       // 12 暗场雪（黑底极少亮点）
    case SN_VBAR: {                                        // 13 竖直条干扰（26 条 EMI 竖纹）
        for (int x = 0; x < SCR_W; x += 10) {              // ~24 条
            int w = 2 + (int)(fastRand() % 3);
            bool bright = (fastRand() & 1);
            for (int y = 0; y < SCR_H; y++)
                for (int xx = x; xx < x + w && xx < SCR_W; xx++) {
                    uint16_t c = bright ? g_lut[150 + (fastRand() % 100)]
                                        : g_lut[fastRand() % 30];
                    fb[y * SCR_W + xx] = c;
                }
        }
        break;
    }
    default:        snowStep(fb, 6, nullptr); break;       // 0 正常琥珀
    }
}

// ── 撕裂：事件期整行循环平移（渲染后、pushSprite 前）────────
static void tearApply(uint16_t* fb) {
    if (g_g.tearRow == 0 || g_g.tearT == 0) return;
    int y1 = g_g.tearRow + g_g.tearLen;
    if (y1 > SCR_H) y1 = SCR_H;
    for (int y = g_g.tearRow; y < y1; y++) {
        uint16_t* row = &fb[y * SCR_W];
        memcpy(s_tealTmp, row, SCR_W * 2);
        for (int x = 0; x < SCR_W; x++) {
            int sx = (x + g_g.tearShift) % SCR_W;          // v0.0.17：负位移曾得负下标越界读
            if (sx < 0) sx += SCR_W;
            row[x] = s_tealTmp[sx];
        }
    }
}

// ── 大事件渲染（v0.0.4 干扰库扩；全部事件型，与撕裂/跳帧互不重叠）──

// ① 假关机压线：画面像 CRT 拔电一样水平压缩成一条亮线。
//    场景进度 p (0→255)：前 60% 压缩，后 40% 熄灭只留中间一条亮线
static void powerCutApply(uint16_t* fb) {
    uint32_t p = g_g.p;                          // 0..255
    int stage = (p < 153) ? 0 : 1;               // 0=压缩 1=熄灭（153≈60%）
    int t = (p < 153) ? (int)(p * 10 / 153) : 10;   // 压缩阶段 0..10
    if (stage == 0) {
        int band = 135 * (10 - t) / 10 + 3;      // 135 → 3px 线性收窄
        if (band > 135) band = 135;
        int top = (SCR_H - band) / 2;
        for (int y = 0; y < SCR_H; y++) {
            if (y < top || y >= top + band)
                memset(&fb[y * SCR_W], 0, SCR_W * 2);
        }
        // 带内做垂直压缩采样（模拟行距塌缩）
        if (band < 60) {
            // v0.0.17：原来这里有一块 static compressed[SCR_W*60]=28.8KB 独占静态内存，
            //           改用整帧重采样缓冲 s_fbTmp 的前 60 行（本路径无其它使用者）→ 静态占用 81.8% ⇒ ~73%
            uint16_t* compressed = s_fbTmp;
            for (int y = 0; y < band && y < 60; y++) {
                int srcY = top + y * (SCR_H / band);
                if (srcY >= SCR_H) srcY = SCR_H - 1;
                memcpy(&compressed[y * SCR_W], &fb[srcY * SCR_W], SCR_W * 2);
            }
            for (int y = 0; y < SCR_H; y++) {
                if (y >= top && y < top + band && (y - top) < 60)
                    memcpy(&fb[y * SCR_W], &compressed[(y - top) * SCR_W], SCR_W * 2);
            }
        }
    } else {
        // 熄灭：只留最中间 1~3px 琥珀亮线（CRT 关机特征），其余全黑
        memset(fb, 0, SCR_W * SCR_H * 2);
        uint16_t lineC = swap16(rgb565(255, 220, 120));   // 熄灭瞬间亮线
        for (int y = SCR_H / 2 - 1; y <= SCR_H / 2 + 1 && y < SCR_H; y++) {
            uint16_t* r = &fb[y * SCR_W];
            for (int x = 0; x < SCR_W; x++) r[x] = lineC;
        }
    }
}

// ② 画面翻转：信号极性错误，整帧上下颠倒（1s）后翻回
static void flipApply(uint16_t* fb) {
    for (int y = 0; y < SCR_H / 2; y++) {
        uint16_t* a = &fb[y * SCR_W];
        uint16_t* b = &fb[(SCR_H - 1 - y) * SCR_W];
        memcpy(s_tealTmp, a, SCR_W * 2);
        memcpy(a, b, SCR_W * 2);
        memcpy(b, s_tealTmp, SCR_W * 2);
    }
}

// ③ 搜台横滚：整幅画面快速横向滚动（信号扫描）
// ★ 逐行滚动：只用 480B 行缓存（s_tealTmp），整帧快照会溢出崩溃
static void scrollApply(uint16_t* fb) {
    static int shift = 0;
    shift += 7;                                     // 每帧 7px，流畅滚动
    if (shift >= SCR_W) shift -= SCR_W;
    for (int y = 0; y < SCR_H; y++) {
        uint16_t* row = &fb[y * SCR_W];
        memcpy(s_tealTmp, row, SCR_W * 2);
        for (int x = 0; x < SCR_W; x++)
            row[x] = s_tealTmp[(x + shift) % SCR_W];
    }
}

// ④ 信号渐弱：雪花亮度整体衰减并变稀疏（信号在衰减）
static void fadeApply(uint16_t* fb) {
    uint32_t p = g_g.p;                          // 0..255
    if (p == 0) return;
    // 亮度衰减：均匀乘系数
    uint32_t mul = (255 - p) * 2;                   // 255 → 0
    uint16_t* lut = g_lut;
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint8_t ph = g_phos[i];
        uint8_t n = (uint8_t)(((uint32_t)ph * mul) >> 9);   // /2 渐变
        g_phos[i] = n;
        fb[i] = lut[n];
    }
}

// ⑤ 黑带扫：一条黑色横向干扰带从顶到底扫过（信号瞬断）
//    位置由场景进度 p 驱动：p=0 在顶，p=255 到底
static void bandScanApply(uint16_t* fb) {
    int h = 20;                                        // 固定带高
    int start = (int)((uint32_t)g_g.p * (SCR_H + h) / 256) - h;
    for (int y = start; y < start + h && y < SCR_H; y++) {
        if (y < 0) continue;
        memset(&fb[y * SCR_W], 0, SCR_W * 2);
    }
}

// ── v0.0.5 干扰库第二波：C 类 16 种新扭曲 ──

// ⑥ 行不同步斜纹：行同步 AFC 失锁 → 每行渐进横向错位，对角线撕裂
static void holdApply(uint16_t* fb) {
    int step = 6 + (int)(g_g.p / 26);                  // 错位梯度随进度增大
    for (int y = 0; y < SCR_H; y++) {
        int sh = (y * step) % SCR_W;
        if (sh == 0) continue;
        memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
        for (int x = 0; x < SCR_W; x++)
            fb[y * SCR_W + x] = s_tealTmp[(x + sh) % SCR_W];
    }
}

// ⑦ 场不同步上下滚：垂直同步丢失 → 整帧缓慢上下滚动（vertical roll）
static void vrollApply(uint16_t* fb) {
    int roll = (int)(g_g.p * 2) % (SCR_H * 2);         // 0..270, 超过 H 反向
    if (roll > SCR_H) roll = SCR_H * 2 - roll;         // 上/下往返
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    for (int y = 0; y < SCR_H; y++) {
        int src = y + roll;
        if (src >= SCR_H) src -= SCR_H;
        memcpy(&fb[y * SCR_W], &s_fbTmp[src * SCR_W], SCR_W * 2);
    }
}

// ⑧ 卷边 foldover：画面顶部/底部向内折叠（垂直偏转过载）
static void foldApply(uint16_t* fb) {
    int fold = 10 + (int)(g_g.p / 10);                 // 折叠深度
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    for (int y = 0; y < fold && y < SCR_H; y++)        // 顶部：镜像到顶部下方
        memcpy(&fb[(y + fold) * SCR_W], &s_fbTmp[(fold - y) * SCR_W], SCR_W * 2);
    for (int y = SCR_H - fold; y < SCR_H; y++)         // 底部：镜像到底部上方
        memcpy(&fb[(y - fold) * SCR_W], &s_fbTmp[y * SCR_W], SCR_W * 2);
}

// ⑨ 整体抖动：全帧高频 ±3px 抖动（电源不稳）
static void shakeApply(uint16_t* fb) {
    int dx = (int)(fastRand() % 7) - 3, dy = (int)(fastRand() % 5) - 2;
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    memset(fb, 0, SCR_W * SCR_H * 2);
    for (int y = 0; y < SCR_H; y++) {
        int sy = y + dy;
        if (sy < 0 || sy >= SCR_H) continue;
        for (int x = 0; x < SCR_W; x++) {
            int sx = x + dx;
            if (sx < 0 || sx >= SCR_W) continue;
            fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
        }
    }
}

// ⑩ 天线抖动：画面随机跳位再弹回（手碰天线 / 脉冲干扰）
static void antApply(uint16_t* fb) {
    static int phase = 0;
    phase++;
    int amp = (phase & 1) ? 8 : 3;                     // 一拍大跳，一拍回弹
    int dx = (int)(fastRand() % (2 * amp + 1)) - amp;
    int dy = (int)(fastRand() % 5) - 2;
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    memset(fb, 0, SCR_W * SCR_H * 2);
    for (int y = 0; y < SCR_H; y++) {
        int sy = y + dy;
        if (sy < 0 || sy >= SCR_H) continue;
        for (int x = 0; x < SCR_W; x++) {
            int sx = x + dx;
            if (sx < 0 || sx >= SCR_W) continue;
            fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
        }
    }
}

// ⑪ 水波涟漪：行偏移按正弦曲线扭曲（电磁串扰）
static void waveApply(uint16_t* fb) {
    static uint8_t t = 0;
    t += 3;
    for (int y = 0; y < SCR_H; y++) {
        int sh = (int)(sinf(y * 0.12f + t * 0.25f) * 6);
        if (sh == 0) continue;
        memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
        for (int x = 0; x < SCR_W; x++) {
            int sx = (x + sh) % SCR_W;
            if (sx < 0) sx += SCR_W;
            fb[y * SCR_W + x] = s_tealTmp[sx];
        }
    }
}

// ⑫ 电子束放电弧：高压打火 → 随机亮弧一闪（flyback arcing）
static void arcApply(uint16_t* fb) {
    for (int k = 0; k < 3; k++) {
        int x0 = (int)(fastRand() % SCR_W), y0 = (int)(fastRand() % SCR_H);
        int x1 = x0 + (int)(fastRand() % 60) - 25, y1 = y0 + (int)(fastRand() % 30) - 10;
        if (x1 < 0) x1 = 0; if (x1 >= SCR_W) x1 = SCR_W - 1;
        if (y1 < 0) y1 = 0; if (y1 >= SCR_H) y1 = SCR_H - 1;
        int dx = abs(x1 - x0), dy = abs(y1 - y0), err = dx - dy;
        int sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1, x = x0, y = y0;
        for (int n = 0; n < 400 && n < dx + dy; n++) {
            if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
                fb[y * SCR_W + x] = g_lut[255];
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 < dx)  { err += dx; y += sy; }
        }
    }
}

// ⑬ 桶形/枕形畸变：几何重采样（垂直方向向内/外弯曲）
static void pincApply(uint16_t* fb) {
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    for (int y = 0; y < SCR_H; y++) {
        float fy = y / (float)SCR_H - 0.5f;
        float bend = (g_g.p < 128) ? 0.06f : -0.06f;   // 桶形→枕形
        int srcY = (int)((fy - bend * fy * fy * fy * 4) * SCR_H + SCR_H / 2);
        if (srcY < 0) srcY = 0; if (srcY >= SCR_H) srcY = SCR_H - 1;
        memcpy(&fb[y * SCR_W], &s_fbTmp[srcY * SCR_W], SCR_W * 2);
    }
}

// ⑭ 画面镜像：左右镜像翻转（极性接反）
static void mirrorApply(uint16_t* fb) {
    for (int y = 0; y < SCR_H; y++) {
        memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
        for (int x = 0; x < SCR_W; x++)
            fb[y * SCR_W + x] = s_tealTmp[SCR_W - 1 - x];
    }
}

// ⑮ 上下拼接：从中间劈开，两半错位拼贴
static void spliceApply(uint16_t* fb) {
    int split = SCR_H / 2 + (int)(g_g.p / 16) - 8;     // 劈缝位置随进度偏摆
    if (split < 4) split = 4; if (split > SCR_H - 5) split = SCR_H - 5;
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    int shift = 6 + (int)(g_g.p / 22);                  // 错位量
    for (int y = 0; y < SCR_H; y++) {
        int src = y;
        if (y >= split) src = y - shift;                // 下半错开
        if (src < 0) src = 0; if (src >= SCR_H) src = SCR_H - 1;
        memcpy(&fb[y * SCR_W], &s_fbTmp[src * SCR_W], SCR_W * 2);
    }
}

// ⑯ 画面收缩放大：整帧向中心收缩再弹回（聚焦呼吸）
static void zoomApply(uint16_t* fb) {
    int amp = (int)(g_g.p / 2);                        // 0..127 → 收缩量
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    memset(fb, 0, SCR_W * SCR_H * 2);
    float k = 1.0f - amp * 0.0008f;                    // 0.92~1.0
    int cx = SCR_W / 2, cy = SCR_H / 2;
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++) {
            int sx = (int)((x - cx) * k + cx), sy = (int)((y - cy) * k + cy);
            if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
            if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
            fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
        }
}

// ⑰ 负片反转：亮度反相一根快门（极性错误）
static void invertApply(uint16_t* fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint16_t c = swap16(fb[i]);                     // v0.0.17：fb 为字节交换存储（坑 8.41），先 swap 回逻辑 RGB565
        if (c == 0) { fb[i] = g_lut[0]; continue; }     // 黑→微亮
        uint8_t r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31;
        fb[i] = swap16(rgb565(255 - (r << 3), 255 - (gg << 2), 255 - (b << 3)));
    }
}

// ⑱ 暗角遮罩：四角渐变收暗（CRT 球面阴影）
static void vignApply(uint16_t* fb) {
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++) {
            float dx = (x - SCR_W / 2) / (float)(SCR_W / 2);
            float dy = (y - SCR_H / 2) / (float)(SCR_H / 2);
            float d = dx * dx + dy * dy;
            if (d < 0.25f) continue;
            uint8_t mul = (uint8_t)(255 - (d - 0.25f) * 90);
            uint16_t c = swap16(fb[y * SCR_W + x]);     // v0.0.17：口径与 breath.h 统一
            uint8_t r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31;
            r = (uint8_t)(r * mul >> 8); gg = (uint8_t)(gg * mul >> 8); b = (uint8_t)(b * mul >> 8);
            fb[y * SCR_W + x] = swap16(rgb565(r << 3, gg << 2, b << 3));
        }
}

// ⑲ 信箱黑边：上下黑边切入（16:9 误触发）
static void lboxApply(uint16_t* fb) {
    for (int y = 0; y < 24 && y < SCR_H; y++) memset(&fb[y * SCR_W], 0, SCR_W * 2);
    for (int y = SCR_H - 24; y < SCR_H; y++) memset(&fb[y * SCR_W], 0, SCR_W * 2);
}

// ⑳ 聚焦漂移：行间混合（简单横向模糊，聚焦缓慢漂失）
static void focusApply(uint16_t* fb) {
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++) {
            uint16_t c0 = swap16(s_fbTmp[y * SCR_W + x]);                              // v0.0.17：先 swap
            uint16_t cL = (x > 0) ? swap16(s_fbTmp[y * SCR_W + x - 1]) : c0;
            uint16_t cR = (x < SCR_W - 1) ? swap16(s_fbTmp[y * SCR_W + x + 1]) : c0;
            uint8_t r = ((uint8_t)((c0 >> 11) & 31) + (uint8_t)((cL >> 11) & 31) + (uint8_t)((cR >> 11) & 31)) / 3;
            uint8_t gg = ((uint8_t)((c0 >> 5) & 63) + (uint8_t)((cL >> 5) & 63) + (uint8_t)((cR >> 5) & 63)) / 3;
            uint8_t b = ((uint8_t)(c0 & 31) + (uint8_t)(cL & 31) + (uint8_t)(cR & 31)) / 3;
            fb[y * SCR_W + x] = swap16(rgb565(r << 3, gg << 2, b << 3));
        }
}

// ㉑ 多径重影：画面向右错位叠加重影（高楼反射）
static void echoApply(uint16_t* fb) {
    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
    int off = 4 + (int)(g_g.p / 40);
    for (int y = 0; y < SCR_H; y++) {
        memcpy(s_tealTmp, &s_fbTmp[y * SCR_W], SCR_W * 2);   // 原行
        for (int x = off; x < SCR_W; x++) {
            uint16_t c0 = swap16(s_fbTmp[y * SCR_W + x]);       // v0.0.17：先 swap 回逻辑 RGB565
            uint16_t cg = swap16(s_tealTmp[x - off]);
            uint8_t r = (uint8_t)((uint8_t)((c0 >> 11) & 31) + ((uint8_t)((cg >> 11) & 31) >> 1));
            if (r > 31) r = 31;
            uint8_t gg = (uint8_t)((uint8_t)((c0 >> 5) & 63) + ((uint8_t)((cg >> 5) & 63) >> 1));
            if (gg > 63) gg = 63;
            uint8_t b = (uint8_t)((uint8_t)(c0 & 31) + ((uint8_t)(cg & 31) >> 1));
            if (b > 31) b = 31;
            fb[y * SCR_W + x] = swap16(rgb565(r << 3, gg << 2, b << 3));
        }
    }
}

#ifndef PRODUCTION_BUILD
// dev 走查探针：在扭曲之前画确定性条纹（行 0/6/12…，x%7==0 处一个白点）。
// 白点相位可测出撕裂位移量，用于真机验证 WR_TEAR（P1-6）与负位移归一化（P2-5）。
static uint8_t g_warpMarker = 0;
static void warpMarker(uint16_t* fb) {
    uint16_t c = swap16(rgb565(255, 255, 255));
    for (int y = 0; y < SCR_H; y += 6)
        for (int x = 0; x < SCR_W; x += 7)
            fb[y * SCR_W + x] = c;
}
#endif

// ── 故障场景总渲染：雪花 × 文字 × 扭曲 三元素组合 ──
// 由 loop 调用；g_g.active=1 时按场景组合渲染，否则走正常雪花+招牌
static void sceneApply(uint16_t* fb) {
    // 特殊事件优先（不属于三元素组合；各自独立幻觉）
    if (g_g.special) {
        switch (g_g.special) {
        case 1: {   // 频道切换：前 60% 黑屏，后 40% 恢复雪花（换台瞬间）
            if (g_g.spT > g_g.spT0 / 2) memset(fb, 0, SCR_W * SCR_H * 2);
            else snowVariant(fb, 2);                   // 切换后稀疏雪（新台弱信号）
            return;
        }
        case 2: {   // 信号恢复幻觉：雪花中短暂闪出"正片"（微弱线条）
            snowVariant(fb, 2);
            int bandY = 20 + (int)(fastRand() % (SCR_H - 40));
            for (int y = bandY; y < bandY + 3 && y < SCR_H; y++)
                for (int x = 0; x < SCR_W; x += 2) fb[y * SCR_W + x] = g_lut[200];
            return;
        }
        case 3: {   // 电话干扰：竖纹疯狂抽动 + 雪花扰动
            snowVariant(fb, 4);
            for (int x = 0; x < SCR_W; x += (3 + (int)(fastRand() % 4)))
                if (fastRand() & 1)
                    for (int y = 0; y < SCR_H; y++) fb[y * SCR_W + x] = g_lut[255];
            return;
        }
        case 4: {   // 天线松动：画面整体剧烈抖动（放大版）
            int dx = (int)(fastRand() % 15) - 7, dy = (int)(fastRand() % 13) - 6;
            memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
            memset(fb, 0, SCR_W * SCR_H * 2);
            for (int y = 0; y < SCR_H; y++) {
                int sy = y + dy;
                if (sy < 0 || sy >= SCR_H) continue;
                for (int x = 0; x < SCR_W; x++) {
                    int sx = x + dx;
                    if (sx < 0 || sx >= SCR_W) continue;
                    fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
                }
            }
            return;
        }
        case 5: {   // 幽灵广播：极淡"干扰源"文字浮现（如 CH-4 / 476MHz 残留）
            snowVariant(fb, 0);
            const char* gs = "N V 9 3";
            int gsw = strWidth57(gs, 2);
            int a = (int)(g_g.spT * 18);               // 淡入淡出强度（spT 递减）
            if (a > 60) a = 60;
            drawStr57(fb, (SCR_W - gsw) / 2, 30, gs,
                      swap16(rgb565((uint8_t)(40 + a), (uint8_t)(30 + a), (uint8_t)(12 + a / 2))), 0, 2);
            return;
        }
        case 6: {   // 外星符文灯牌：NO SIGNAL 替换为外星符文 + 抖动（0.0.16 配外星语背景）
            snowVariant(fb, 0);
            for (int y = SCR_H / 2 - 16; y < SCR_H / 2 + 14 && y < SCR_H; y++)
                for (int x = SCR_W / 2 - 44; x < SCR_W / 2 + 44 && x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(10, 12, 22));
            int dx = (int)(fastRand() % 5) - 2;
            int dy = (int)(fastRand() % 3) - 1;
            for (int g = 0; g < 5; g++) {
                int idx = (g < 3) ? g : (g == 3 ? 0 : 1);
                psetRune(fb, SCR_W / 2 - 32 + g * 13 + dx, SCR_H / 2 - 9 + dy, idx,
                         swap16(rgb565(200, 255, 130)));
            }
            for (int i = 0; i < 30; i++)
                fb[(int)(fastRand() % SCR_H) * SCR_W + (int)(fastRand() % SCR_W)] = g_lut[fastRand() % 255];
            return;
        }
        }
    }

    // ── 雪花变体（背景）──
    snowVariant(fb, g_g.snow);

#ifndef PRODUCTION_BUILD
    if (g_warpMarker) warpMarker(fb);      // dev：扭曲前画条纹（真机验证位移量）
#endif

    // ── 画面扭曲（作用于雪花层；招牌由调用方在本函数之后绘制，故不受扭曲影响）──
    switch (g_g.warp) {
    case WR_TEAR: {                                    // 强撕裂：多带撕裂
        // v0.0.17：tearApply 被 "tearRow==0 || tearT==0" 门控（事件型撕裂用；事件侧 tearRow 取 8+，
        //   0 是"无撕裂"哨兵）——本扭曲原来设 tearRow=0 且场景内 tearT 恒 0 → 双重被挡，等于空转。
        //   这里显式给场景内的 tearT，并从第 1 行起（视觉效果与 0 起无差别），出口还原事件时间线。
        uint8_t saveTearT = g_g.tearT;
        g_g.tearT = g_g.t;
        g_g.tearRow = 1; g_g.tearLen = SCR_H / 2 - 1; g_g.tearShift = 20;
        tearApply(fb);
        g_g.tearRow = SCR_H / 2; g_g.tearLen = SCR_H / 2; g_g.tearShift = -12;
        tearApply(fb);
        g_g.tearT = saveTearT;
        g_g.tearRow = 0; g_g.tearLen = 0; g_g.tearShift = 0;   // 复位，交还事件撕裂
        break;
    }
    case WR_FLIP:   flipApply(fb); break;
    case WR_SCAN:   scrollApply(fb); break;
    case WR_POWER:  powerCutApply(fb); break;
    case WR_FADE:   fadeApply(fb); break;
    case WR_BAND:   bandScanApply(fb); break;
    case WR_HOLD:   holdApply(fb); break;              // 行不同步斜纹
    case WR_VROLL:  vrollApply(fb); break;             // 上下滚
    case WR_FOLD:   foldApply(fb); break;              // 卷边
    case WR_SHAKE:  shakeApply(fb); break;             // 整体抖动
    case WR_ANT:    antApply(fb); break;               // 天线抖动
    case WR_WAVE:   waveApply(fb); break;              // 水波涟漪
    case WR_ARC:    arcApply(fb); break;               // 放电弧
    case WR_PINC:   pincApply(fb); break;              // 桶形/枕形
    case WR_MIRROR: mirrorApply(fb); break;            // 镜像
    case WR_SPLICE: spliceApply(fb); break;            // 上下拼接
    case WR_ZOOM:   zoomApply(fb); break;              // 收缩放大
    case WR_INVERT: invertApply(fb); break;            // 负片
    case WR_VIGN:   vignApply(fb); break;              // 暗角
    case WR_LBOX:   lboxApply(fb); break;              // 信箱
    case WR_FOCUS:  focusApply(fb); break;             // 聚焦漂移
    case WR_ECHO:   echoApply(fb); break;              // 多径重影
    default: break;
    }
}

// ════════════════════════════════════════════════════
// 「拍出信号」节目生成器（v0.0.10）
// 拍中 40% → 随机拉出一个"远方电视台"节目画面 5s：
//   阶段A 收拢(前12帧)：画面抖动+噪点浓→渐进锁定（拍出信号的对焦感）
//   阶段B 主段(中间)：静态节目 + 轻微噪点呼吸
//   阶段C 溶回(最后18帧)：行错位加重+噪点暴增→信号丢失→回雪花
// 背景 3 种渐变 × 主体 4 种剪影 = 12 组合；频道号台标随机 1-13
// ════════════════════════════════════════════════════

// 逐行垂直渐变填充（cTop/cBot 为原始 RGB565 未 swap）
static void fillVgrad(uint16_t* fb, int y0, int y1, uint16_t cTop, uint16_t cBot) {
    if (y0 < 0) y0 = 0; if (y1 > SCR_H) y1 = SCR_H;
    if (y0 >= y1) return;
    int rT = (cTop >> 11) & 31, gT = (cTop >> 5) & 63, bT = cTop & 31;
    int rB = (cBot >> 11) & 31, gB = (cBot >> 5) & 63, bB = cBot & 31;
    for (int y = y0; y < y1; y++) {
        int k = (y - y0) * 256 / (y1 - y0);              // 0..256
        uint16_t c = swap16(rgb565(
            (uint8_t)(rT + (rB - rT) * k / 256),
            (uint8_t)(gT + (gB - gT) * k / 256),
            (uint8_t)(bT + (bB - bT) * k / 256)));
        uint16_t* row = &fb[y * SCR_W];
        for (int x = 0; x < SCR_W; x++) row[x] = c;
    }
}

// 实心三角形填充（y 递增扫描线；宽边贴底）
static void fillTri(uint16_t* fb, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int yA = y0, yB = y0;
    if (y1 < yA) yA = y1; if (y2 < yA) yA = y2;
    if (y1 > yB) yB = y1; if (y2 > yB) yB = y2;
    if (yA < 0) yA = 0; if (yB > SCR_H - 1) yB = SCR_H - 1;
    for (int y = yA; y <= yB; y++) {
        int xL = SCR_W, xR = 0;
        int pts[3][2] = {{x0,y0},{x1,y1},{x2,y2}};
        for (int e = 0; e < 3; e++) {
            int ax = pts[e][0], ay = pts[e][1];
            int bx = pts[(e+1)%3][0], by = pts[(e+1)%3][1];
            if (ay == by) continue;
            if ((y >= ay && y < by) || (y >= by && y < ay)) {
                int x = ax + (bx - ax) * (y - ay) / (by - ay);
                if (x < xL) xL = x; if (x > xR) xR = x;
            }
        }
        if (xR < xL) continue;
        if (xL < 0) xL = 0; if (xR > SCR_W - 1) xR = SCR_W - 1;
        uint16_t* row = &fb[y * SCR_W];
        for (int x = xL; x <= xR; x++) row[x] = c;
    }
}

// 空心圆盘（取消：落日用内联贴海写法，避免 unused 警告）

// 画节目底图到 fb（背景渐变 + 主体剪影 + 台标；噪点另由 progApply 注入）
static void progDrawBase(uint16_t* fb, uint8_t bg, uint8_t subj, uint8_t ch, uint8_t info, int te = 0) {
    if (!fb) return;                          // 防御：buffer 异常跳过（防 NULL 写崩）
    // 底图用固定种子渲染：天体/城市/星空/马赛克等元素位置一次定死，不逐帧漂移
    uint32_t rngSave = rngState;
    rngState = g_g.progSeed ? g_g.progSeed : (uint32_t)0x9E3779B9u;
    // ── 稀有事件：整幅替换底图（测试卡/彩条/灰阶/TELETEXT/马赛克）──
    if (g_g.progRare) {
        switch (g_g.progRare) {
        case 1: {   // Philips 圆环测试卡：中央大圆环 + 圆内色块 + 四角彩条
            fillVgrad(fb, 0, SCR_H, rgb565(60, 52, 40), rgb565(30, 26, 20));
            int cx = SCR_W / 2, cy = SCR_H / 2, cr = SCR_H / 3;
            // 圆环（亮环 + 暗环双线）
            for (int y = cy - cr - 8; y <= cy + cr + 8; y++) {
                if (y < 0 || y >= SCR_H) continue;
                int dy = y - cy;
                for (int x = cx - cr - 8; x <= cx + cr + 8; x++) {
                    if (x < 0 || x >= SCR_W) continue;
                    int dx = x - cx;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= cr * cr) {
                        // 圆内：中心亮 + 边缘琥珀，放射渐变
                        int dist = (int)sqrtf((float)d2);
                        int band = dist * 5 / (cr + 1);
                        fb[y * SCR_W + x] = swap16(rgb565(
                            90 + band * 8, 66 + band * 6, 30 + band * 2));
                    } else if (d2 <= (cr + 6) * (cr + 6)) {
                        fb[y * SCR_W + x] = swap16(rgb565(255, 220, 130));   // 亮环
                    } else if (d2 <= (cr + 10) * (cr + 10)) {
                        fb[y * SCR_W + x] = swap16(rgb565(20, 16, 10));      // 暗边
                    }
                }
            }
            // 中心十字
            for (int i = -6; i <= 6; i++) {
                fb[cy * SCR_W + cx + i] = swap16(rgb565(255, 232, 150));
                fb[(cy + i) * SCR_W + cx] = swap16(rgb565(255, 232, 150));
            }
            // 四角彩条
            const uint16_t cornerC[4] = {
                swap16(rgb565(255, 120, 80)), swap16(rgb565(80, 200, 120)),
                swap16(rgb565(80, 120, 255)), swap16(rgb565(255, 220, 80))};
            int cw = 26, chh = 14;
            int corners[4][2] = {{4,4},{SCR_W-cw-4,4},{4,SCR_H-chh-4},{SCR_W-cw-4,SCR_H-chh-4}};
            for (int c = 0; c < 4; c++)
                for (int y = corners[c][1]; y < corners[c][1]+chh; y++)
                    for (int x = corners[c][0]; x < corners[c][0]+cw; x++)
                        fb[y * SCR_W + x] = cornerC[c];
            break;
        }
        case 2: {   // 全屏彩条：顶 4/5 六条竖彩条 + 底 1/5 灰度阶梯
            const uint16_t barC[6] = {
                swap16(rgb565(255,170,60)), swap16(rgb565(255,230,90)),
                swap16(rgb565(60,200,120)), swap16(rgb565(60,160,80)),
                swap16(rgb565(80,120,255)), swap16(rgb565(200,90,220))};
            int barH = SCR_H * 4 / 5;
            for (int b = 0; b < 6; b++)
                for (int y = 0; y < barH; y++)
                    for (int x = b * SCR_W / 6; x < (b+1) * SCR_W / 6; x++)
                        fb[y * SCR_W + x] = barC[b];
            for (int y = barH; y < SCR_H; y++) {
                int grey = 40 + (y - barH) * 190 / (SCR_H - barH);   // 深→亮灰
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(grey, grey, grey));
            }
            break;
        }
        case 3: {   // 灰度阶梯：横向 8 段黑→白
            for (int y = 0; y < SCR_H; y++)
                for (int s = 0; s < 8; s++) {
                    int v = 16 + s * 32;
                    for (int x = s * SCR_W / 8; x < (s+1) * SCR_W / 8; x++)
                        fb[y * SCR_W + x] = swap16(rgb565(v, v, v));
                }
            // 顶部一条琥珀测试条（台信号感）
            for (int x = 0; x < SCR_W; x += 4) fb[6 * SCR_W + x] = swap16(rgb565(255, 190, 80));
            break;
        }
        case 6: {   // galcoll 粒子流彩蛋：深空 + 螺旋星河（流动粒子）
            fillVgrad(fb, 0, SCR_H, rgb565(8, 6, 24), rgb565(2, 2, 8));
            int cx = SCR_W / 2, cy = SCR_H / 2 - 10;
            // 椭圆旋臂：粒子沿臂位移动（te 驱动流速）
            for (int arm = 0; arm < 3; arm++) {
                for (int p = 0; p < 46; p++) {
                    float ang = (float)(p + arm * 15) * 0.13f + (float)te * 0.02f;
                    float rad = 6.0f + (float)(p % 30) * 2.1f;
                    int px = cx + (int)(cosf(ang) * rad * 1.6f);
                    int py = cy + (int)(sinf(ang) * rad);
                    if (px < 0 || px >= SCR_W || py < 0 || py >= SCR_H) continue;
                    // 亮度沿臂衰减（中心亮、尾端暗）
                    int br = 60 + (p % 30) * 6;
                    uint16_t pc = (arm == 1)
                        ? swap16(rgb565(br, br / 2, br / 2 + 20))      // 暖臂
                        : swap16(rgb565(br / 2 + 10, br, br / 2 + 16)); // 冷臂
                    fb[py * SCR_W + px] = pc;
                    if (px + 1 < SCR_W) fb[py * SCR_W + px + 1] = pc;  // 两像素宽（更亮）
                }
            }
            // 中心核
            for (int g = 6; g > 0; g--)
                for (int dy = -g; dy <= g; dy++) {
                    int py = cy + dy;
                    if (py < 0 || py >= SCR_H) continue;
                    int dx = (int)sqrtf((float)(g * g - dy * dy));
                    for (int x = cx - dx; x <= cx + dx; x++)
                        if (x >= 0 && x < SCR_W)
                            fb[py * SCR_W + x] = swap16(rgb565(255, 240, 210));
                }
            break;
        }
        case 7: {   // backrooms 黄雾彩蛋：不安的黄色空间（黄色系全屏）
            fillVgrad(fb, 0, SCR_H, rgb565(140, 118, 40), rgb565(90, 72, 24));
            // 墙缝纹理：横条暗黄沟槽
            for (int y = 0; y < SCR_H; y += 11)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(60, 48, 14));
            // 斑驳霉斑：随 te 缓慢漂移的深黄暗斑（不安的"活着"感；固定种子下用 fastRand 派生）
            for (int b = 0; b < 9; b++) {
                int bx = (int)((fastRand() % (SCR_W + 20) + te * 2) % (SCR_W + 20)) - 10;
                int by = (int)(fastRand() % (SCR_H - 20)) + 6;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int px = bx + dx, py = by + dy;
                        if (px < 0 || px >= SCR_W || py < 0 || py >= SCR_H) continue;
                        int d2 = dx * dx + dy * dy;
                        if (d2 <= 9)
                            fb[py * SCR_W + px] = swap16(rgb565(58, 46, 12));
                    }
            }
            // 底部黑暗走廊
            for (int y = SCR_H - 14; y < SCR_H; y++)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(24, 18, 6));
            break;
        }
        case 8: {   // RLCD 海浪线彩蛋：致敬 RLCD 时钟屏的黑底白线波浪
            fillVgrad(fb, 0, SCR_H, rgb565(6, 6, 6), rgb565(2, 2, 2));  // 近黑底
            uint16_t wc = swap16(rgb565(228, 228, 228));                // 毛玻璃白
            for (int l = 0; l < 4; l++) {
                int y0 = 18 + l * 28;
                for (int x = 0; x < SCR_W; x++) {
                    int y = y0 + (int)(sinf((float)(x + te * 3) * 0.08f + (float)l * 1.3f) * 6.0f);
                    if (y >= 0 && y < SCR_H) {
                        fb[y * SCR_W + x] = wc;
                        if (y + 2 < SCR_H) fb[(y + 2) * SCR_W + x] = swap16(rgb565(80, 80, 80));  // 投影
                    }
                }
            }
            break;
        }
        case 4: {   // TELETEXT 文字页：深蓝底 + 彩色字块
            fillVgrad(fb, 0, SCR_H, rgb565(10, 16, 40), rgb565(4, 6, 18));
            static const char TXG[] = "0123456789RECBAOMNSIGLVY";
            for (int row = 0; row < 8; row++) {
                int y = 14 + row * 15;
                // 行号（TELETEXT 页面编号 0-7 的惯例）
                char ln[3];
                snprintf(ln, sizeof(ln), "%d", row);
                drawStr57(fb, 10, y, ln, swap16(rgb565(120, 200, 255)), 0, 1);
                // 彩色文字行（随机字块 + 随机色）
                for (int w = 0; w < 12; w++) {
                    int x = 30 + w * 17;
                    if ((fastRand() % 3) == 0) {
                        char c2[2] = {TXG[fastRand() % 16], 0};
                        drawStr57(fb, x, y, c2,
                                  swap16(rgb565(64 + (int)(fastRand() % 190),
                                                48 + (int)(fastRand() % 190),
                                                40 + (int)(fastRand() % 150))), 0, 1);
                    }
                }
            }
                    break;
                }
                case 9: {   // 外星字母表：符文矩阵（TELETEXT 外星版）
                    fillVgrad(fb, 0, SCR_H, swap16(rgb565(6, 10, 26)), swap16(rgb565(2, 3, 10)));
                    for (int row = 0; row < 5; row++)
                        for (int col = 0; col < 9; col++) {
                            int gx = 14 + col * 24;
                            int gy = 12 + row * 24;
                            if ((fastRand() % 4) == 0) continue;
                            int idx = (int)(fastRand() % 4);
                            psetRune(fb, gx, gy, idx,
                                     swap16(rgb565(90 + (int)(fastRand() % 150),
                                                   140 + (int)(fastRand() % 90),
                                                   130 + (int)(fastRand() % 90))));
                        }
                    for (int x = 0; x < SCR_W; x += 5)
                        fb[(SCR_H - 8) * SCR_W + x] = swap16(rgb565(40, 120, 100));
                    break;
                }
                case 10: {   // 信号倒放：冻结残影 + 色偏色带倒退（te 驱动）
                    fillVgrad(fb, 0, SCR_H, swap16(rgb565(30, 34, 40)), swap16(rgb565(12, 14, 18)));
                    int phase = te * 2 % SCR_H;
                    for (int b = 0; b < 5; b++) {
                        int y = (phase + b * 40) % SCR_H;
                        uint16_t bc = (b & 1) ? swap16(rgb565(40, 200, 170)) : swap16(rgb565(200, 70, 160));
                        for (int x = 0; x < SCR_W; x += 4) fb[y * SCR_W + x] = bc;
                    }
                    for (int i = 0; i < 20; i++) {
                        int bx = (int)(fastRand() % SCR_W);
                        int byy = (int)(fastRand() % (SCR_H - 20));
                        psRect(fb, bx, byy, 6 + (int)(fastRand() % 10), 8 + (int)(fastRand() % 12),
                               swap16(rgb565(120 + (int)(fastRand() % 60),
                                             130 + (int)(fastRand() % 50),
                                             160 + (int)(fastRand() % 50))));
                    }
                    for (int x = 0; x < SCR_W; x += 3)
                        fb[(SCR_H - 10) * SCR_W + x] = swap16(rgb565(255, 240, 170));   // 倒带条
                    break;
                }
                case 11: {   // 辐射风暴：满屏粗粒雪花爆闪（盖革风暴视觉）
                    fillVgrad(fb, 0, SCR_H, swap16(rgb565(20, 22, 18)), swap16(rgb565(6, 8, 6)));
                    for (int y = 0; y < SCR_H; y += 4) {
                        for (int x = 0; x < SCR_W; x++) {
                            uint32_t r = fastRand() % 100;
                            if (r < 42) fb[y * SCR_W + x] = g_lut[fastRand() % 255];
                            else if (r < 58) fb[y * SCR_W + x] = g_lut[(int)(fastRand() % 200) + 40];
                        }
                    }
                    for (int x = 0; x < SCR_W; x += 3)
                        fb[(SCR_H / 2) * SCR_W + x] = swap16(rgb565(255, 255, 255));    // 爆闪源
                    break;
                }
                default: {  // 马赛克信号：画面碎成粗块，每块随机色块 + 边缘闪动
            for (int by = 0; by < SCR_H; by += 9)
                for (int bx = 0; bx < SCR_W; bx += 9) {
                    uint16_t mc;
                    uint32_t r = fastRand() % 4;
                    if (r == 0)      mc = swap16(rgb565(60, 40, 20));
                    else if (r == 1) mc = swap16(rgb565(180, 120, 60));
                    else if (r == 2) mc = swap16(rgb565(255, 210, 120));
                    else             mc = swap16(rgb565(30, 22, 12));
                    for (int y = by; y < by + 9 && y < SCR_H; y++)
                        for (int x = bx; x < bx + 9 && x < SCR_W; x++)
                            fb[y * SCR_W + x] = mc;
                }
        }
        break;
        }
        rngState = rngSave;   // 稀有事件出口：恢复全局随机（主段噪点保持动态）
        return;   // 稀有事件：不用天体/主体/台标（彩条本类自带信息）
    }

    // ── 背景 6 种 ──
    switch (bg) {
    case 0:   fillVgrad(fb, 0, SCR_H, rgb565(120, 72, 20), rgb565(24, 10, 4)); break;   // 琥珀黄昏
    case 1:   fillVgrad(fb, 0, SCR_H, rgb565(8, 14, 42), rgb565(2, 4, 10)); break;      // 深夜蓝
    case 2:   fillVgrad(fb, 0, SCR_H, rgb565(235, 208, 150), rgb565(150, 118, 60)); break; // 过曝白
    case 3:   fillVgrad(fb, 0, SCR_H, rgb565(96, 96, 100), rgb565(34, 34, 40)); break;  // 雾霾灰
    case 4:   fillVgrad(fb, 0, SCR_H, rgb565(150, 90, 150), rgb565(60, 24, 70)); break; // 晨曦紫
    default:  fillVgrad(fb, 0, SCR_H, rgb565(12, 44, 22), rgb565(2, 12, 8)); break;     // 墨绿夜
    }

    // 深底背景提亮剪影（深夜蓝/墨绿夜/晨曦紫）——天体层与主体层共用
    const bool night = (bg == 1 || bg == 5 || bg == 4);
    uint16_t dst  = swap16(rgb565(night ? 110 : 56, night ? 62 : 26, night ? 32 : 8));   // 远景物
    uint16_t ndst = swap16(rgb565(night ? 66 : 22, night ? 40 : 12, night ? 26 : 4));    // 近景物
    uint16_t deep = swap16(rgb565(night ? 44 : 6, night ? 50 : 8, night ? 86 : 24));     // 城市楼/剪影

    // ── 天体层（背景之上、主体之下：山/树/楼会自然"遮住"天体底缘）──
    if (g_g.progCel && subj != 1) {          // 落日主体自带太阳，跳过天体避免双日
        switch (g_g.progCel) {
        case 1: {   // 大太阳：亮圆盘 + 光晕
            int cx = SCR_W / 3 + (int)(fastRand() % (SCR_W / 3));
            int cy = 24 + (int)(fastRand() % 20);
            uint16_t sun = swap16(rgb565(255, 205, 90));
            for (int g = 14; g >= 0; g -= 2) {   // 光晕圈
                uint16_t c = swap16(rgb565(120 - g*4, 90 - g*3, 40 - g));
                for (int y = cy - g; y <= cy + g; y++) {
                    if (y < 0 || y >= SCR_H) continue;
                    int dy = y - cy;
                    int dx = (int)sqrtf((float)(g * g - dy * dy));
                    for (int x = cx - dx; x <= cx + dx; x++)
                        if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = c;
                }
            }
            for (int y = cy - 8; y <= cy + 8; y++) {
                if (y < 0 || y >= SCR_H) continue;
                int dy = y - cy;
                int dx = (int)sqrtf(64.0f - (float)(dy * dy));
                for (int x = cx - dx; x <= cx + dx; x++)
                    if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = sun;
            }
            break;
        }
        case 2: {   // 半月：亮圆盘 + 暗圆切出月牙（有机弯月）
            int mx = SCR_W * 4 / 5, myy = 30 + (int)(fastRand() % 16);
            uint16_t moonC = swap16(rgb565(240, 226, 180));
            for (int y = myy - 12; y <= myy + 12; y++) {
                if (y < 0 || y >= SCR_H) continue;
                int dy = y - myy;
                int dx = (int)sqrtf(144.0f - (float)(dy * dy));
                for (int x = mx - dx; x <= mx + dx; x++)
                    if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = moonC;
            }
            // 右下切掉一块 → 月牙
            for (int y = myy - 10; y <= myy + 10; y++) {
                if (y < 0 || y >= SCR_H) continue;
                int dy = y - myy;
                int dx = (int)sqrtf(100.0f - (float)(dy * dy));
                int cut = mx + dx / 2;
                for (int x = cut; x <= mx + dx; x++)
                    if (x >= 0 && x < SCR_W)
                        fb[y * SCR_W + x] = (night) ? 0 : swap16(rgb565(48, 34, 16));
            }
            break;
        }
        case 3: {   // 星群：疏朗亮星簇（高远感）
            int sn = 14 + (int)(fastRand() % 10);
            for (int i = 0; i < sn; i++) {
                int sx = (int)(fastRand() % SCR_W);
                int sy = 6 + (int)(fastRand() % (SCR_H / 3));
                uint16_t sc = (fastRand() % 2)
                    ? swap16(rgb565(255, 250, 220)) : swap16(rgb565(150, 140, 100));
                fb[sy * SCR_W + sx] = sc;
                if ((fastRand() % 3) == 0 && sx + 2 < SCR_W) fb[sy * SCR_W + sx + 2] = sc;
            }
            break;
        }
        case 4: {   // 云朵：两三层横向椭圆云（压扁质感）
            for (int cl = 0; cl < 3; cl++) {
                int cy2 = 14 + cl * 18 + (int)(fastRand() % 8);
                int cx0 = (int)(fastRand() % (SCR_W / 2));
                int clen = 30 + (int)(fastRand() % 26);
                uint16_t cc = swap16(rgb565(150, 150, 160));
                for (int y = cy2; y < cy2 + 5; y++) {
                    if (y < 0 || y >= SCR_H) continue;
                    int w = clen - (y - cy2) * 6;
                    if (w < 4) w = 4;
                    for (int x = cx0; x < cx0 + w; x++)
                        if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = cc;
                }
            }
            break;
        }
        case 5: {   // 飞鸟剪影：两三只"人"字鸟（远距离归巢感）
            for (int bd = 0; bd < 3; bd++) {
                int bx = 30 + bd * 70 + (int)(fastRand() % 20);
                int by2 = 20 + (int)(fastRand() % 30);
                uint16_t bc = ndst;
                for (int i = -6; i <= 6; i++) {
                    int wing = 6 - (i < 0 ? -i : i);      // 中心窄、两翼翘
                    if (wing < 1) wing = 1;
                    int wx0 = bx + i, wy0 = by2 - wing / 2;
                    if (wx0 >= 0 && wx0 < SCR_W && wy0 >= 0 && wy0 < SCR_H)
                        fb[wy0 * SCR_W + wx0] = bc;
                }
            }
            break;
        }
        case 6: {   // 闪电：竖向锯齿折线 + 远云微光
            int sx0 = SCR_W / 2 + (int)(fastRand() % 40) - 20;
            int y = 6;
            int prevX = sx0;
            while (y < SCR_H - 30) {
                int len = 5 + (int)(fastRand() % 7);
                int nx = prevX + (int)(fastRand() % 5) - 2;
                for (int yy = y; yy < y + len; yy++) {
                    int x = prevX + (nx - prevX) * (yy - y) / len;
                    if (x >= 0 && x < SCR_W) fb[yy * SCR_W + x] = swap16(rgb565(255, 240, 180));
                    if (x - 1 >= 0) fb[yy * SCR_W + x - 1] = swap16(rgb565(180, 150, 90));
                }
                prevX = nx; y += len;
            }
            // 左下方一团"被照亮"的云
            for (int g = 12; g > 4; g -= 4) {
                int cxx = sx0 - 22, cyy = 6;
                for (int yy = cyy; yy < cyy + g && yy < SCR_H; yy++) {
                    int dx = (int)sqrtf((float)(g * g - (yy - cyy) * (yy - cyy)));
                    for (int xx = cxx - dx; xx <= cxx + dx; xx++)
                        if (xx >= 0 && xx < SCR_W)
                            fb[yy * SCR_W + xx] = swap16(rgb565(60 + g * 4, 50 + g * 3, 60 + g * 2));
                }
            }
            break;
        }
        default: {  // 信号塔红光：细塔 + 顶端红色警示灯
            int tx = 36 + (int)(fastRand() % (SCR_W - 72));
            uint16_t pole = swap16(rgb565(night ? 100 : 60, night ? 80 : 34, night ? 60 : 18));
            for (int y = SCR_H - 40; y < SCR_H; y++)
                if (tx >= 0 && tx < SCR_W) fb[y * SCR_W + tx] = pole;
            for (int i = 0; i < 3; i++) {
                int bx = tx - (i * 6), by = SCR_H - 40 + i * 10;
                for (int x = bx; x <= bx + 12; x++)
                    if (x >= 0 && x < SCR_W) fb[by * SCR_W + x] = pole;
            }
            // 顶端红灯（边框暗红→亮红呼吸由主段动态承担）
            fb[(SCR_H - 46) * SCR_W + tx] = swap16(rgb565(255, 60, 50));
            fb[(SCR_H - 46) * SCR_W + tx + 1] = swap16(rgb565(200, 40, 30));
            break;
        }
        case 8: {   // UFO 小碟：小扁盘 + 底光点（悬浮高处，科幻天体）
            int ux = 30 + (int)(fastRand() % (SCR_W - 60));
            int uy = 16 + (int)(fastRand() % 26);
            psEllipse(fb, ux, uy, 12, 4, deep);
            psDome(fb, ux, uy, 4, deep);
            psPx(fb, ux - 3, uy + 3, psC(200, 255, 200));
            break;
        }
        case 9: {   // 彗星：核 + 渐暗拖尾（右上而来）
            int cx = 30 + (int)(fastRand() % (SCR_W - 80));
            int cy = 12 + (int)(fastRand() % 30);
            psPx(fb, cx, cy, psC(255, 245, 220));
            psPx(fb, cx + 1, cy, psC(230, 220, 190));
            for (int i = 1; i < 26; i++) {
                int tx = cx + i * 3;
                int ty = cy + i / 4;
                if (tx >= SCR_W) break;
                if (i % 2) {
                    psPx(fb, tx, ty, psC(200 - i * 4, 190 - i * 4, 160 - i * 5));
                    if (ty + 1 < SCR_H) psPx(fb, tx, ty + 1, psC(150 - i * 3, 140 - i * 3, 120 - i * 4));
                }
            }
            break;
        }
        case 10: {   // 行星环：球 + 斜环带（土星式）
            int px = SCR_W / 2 + (int)(fastRand() % 60) - 30;
            int py = 30 + (int)(fastRand() % 18);
            psCircle(fb, px, py, 12, psC(210, 170, 110));
            psEllipse(fb, px, py + 2, 20, 6, psC(180, 150, 90));
            psEllipse(fb, px, py + 3, 24, 7, psC(120, 95, 60));
            psEllipse(fb, px, py + 2, 16, 4, psC(90, 70, 40));
            break;
        }
        case 11: {   // 双月：大满月 + 小月牙并存
            int mx = SCR_W / 3 + (int)(fastRand() % 40);
            int my = 26 + (int)(fastRand() % 14);
            psCircle(fb, mx, my, 11, psC(240, 226, 180));
            int sx = mx * 2 / 3 + 60;
            int sy = my + 22;
            psCircle(fb, sx, sy, 6, psC(220, 208, 170));
            psCircle(fb, sx - 3, sy - 2, 5, night ? 0 : psC(40, 32, 20));   // 切出月牙
            break;
        }
        }
    }

    // ── 主体 10 种剪影（深底提亮已在上面统一定义 dst/ndst/deep）──
    switch (subj) {
    case 0:   // 山峦双峰：远山亮 + 近山暗
        fillTri(fb, -10, 118, 70, 44, 150, 118, dst);          // 远峰
        fillTri(fb, 80, 118, 190, 30, SCR_W + 10, 118, dst);   // 高远峰
        fillTri(fb, 30, SCR_H, 150, 78, SCR_W + 8, SCR_H, ndst);  // 近山
        fillTri(fb, -6, SCR_H, 100, 96, 130, SCR_H, ndst);        // 近山左
        break;
    case 1:   // 海平线落日（半圆凸出海平线之上，宽边贴线）
        fillVgrad(fb, SCR_H * 3 / 5, SCR_H,
                  night ? rgb565(20, 24, 56) : rgb565(48, 20, 8),     // 夜景海面提亮蓝
                  night ? rgb565(8, 10, 28)  : rgb565(18, 8, 4));
        {
            int hy = SCR_H * 3 / 5;
            uint16_t sun = swap16(rgb565(night ? 230 : 255, night ? 200 : 190, night ? 150 : 80)); // 深夜月光
            // 上半圆：dy 从 -9(圆顶) → 0(海平线=直径)，dx 0→9 渐宽
            for (int y = hy - 9; y <= hy; y++) {
                if (y < 0) continue;
                int dy = y - hy;                          // -9..0
                int dx = (int)sqrtf(81.0f - (float)(dy * dy));
                uint16_t* row = &fb[y * SCR_W];
                for (int x = SCR_W/2 - dx; x <= SCR_W/2 + dx; x++)
                    if (x >= 0 && x < SCR_W) row[x] = sun;
            }
            // 海面倒影：破碎断续的波光列（上亮下暗渐变）
            for (int sx = SCR_W / 2 - 26; sx <= SCR_W / 2 + 26; sx += 3 + (int)(fastRand() % 5)) {
                int len = 6 + (int)(fastRand() % 10);
                for (int l = 0; l < len; l++) {
                    int y = hy + 1 + l * 2;
                    if (y >= SCR_H) break;
                    int dim = 255 - l * 18;                // 越往下越暗
                    if (dim < 0) dim = 0;
                    fb[y * SCR_W + sx] = swap16(rgb565(
                        (uint8_t)((night ? 90 : 120) * dim / 255),
                        (uint8_t)((night ? 100 : 76) * dim / 255),
                        (uint8_t)((night ? 130 : 30) * dim / 255)));
                }
            }
        }
        break;
    case 2:   // 城市天际线：方楼剪影 + 零星亮窗 + 天线
        {
            int x = 0;
            int n = 6 + (int)(fastRand() % 3);          // 6-8 栋
            for (int i = 0; i < n; i++) {
                int w = 12 + (int)(fastRand() % 14);    // 12-25px
                int h = 22 + (int)(fastRand() % 40);    // 22-61px
                int bx = x + (int)(fastRand() % 10);
                for (int yy = SCR_H - h; yy < SCR_H; yy++)
                    for (int xx = bx; xx < bx + w && xx < SCR_W; xx++)
                        if (xx >= 0) fb[yy * SCR_W + xx] = deep;
                // 亮窗 2-4 扇
                int wn = 2 + (int)(fastRand() % 3);
                for (int ww = 0; ww < wn; ww++) {
                    int wx = bx + 3 + (int)(fastRand() % (w - 6));
                    int wy = SCR_H - h + 4 + (int)(fastRand() % (h - 10));
                    fb[wy * SCR_W + wx] = swap16(rgb565(200, 150, 60));
                }
                // 天线（最高楼）
                if (h > 48) {
                    int ax = bx + w / 2;
                    for (int yy = SCR_H - h - 12; yy < SCR_H - h; yy++)
                        if (yy >= 0) fb[yy * SCR_W + ax] = ndst;
                    if (SCR_H - h - 13 >= 0) fb[(SCR_H - h - 13) * SCR_W + ax] = swap16(rgb565(255, 90, 60));
                }
                x = bx + w;
                if (x > SCR_W - 16) break;
            }
        }
        break;
    case 3:   // 星野：亮星 + 暗丘
        {
            int sn = 18 + (int)(fastRand() % 9);
            for (int i = 0; i < sn; i++) {
                int sx = (int)(fastRand() % SCR_W);
                int sy = (int)(fastRand() % (SCR_H * 2 / 3));
                uint16_t sc = (fastRand() % 3 == 0)
                    ? swap16(rgb565(255, 244, 200))   // 亮星
                    : swap16(rgb565(120, 112, 80));   // 暗星
                fb[sy * SCR_W + sx] = sc;
                if ((fastRand() % 4) == 0 && sx + 1 < SCR_W) fb[sy * SCR_W + sx + 1] = sc;
            }
            // 暗丘两层
            fillTri(fb, -10, SCR_H, 90, 96, 170, SCR_H, ndst);
            fillTri(fb, 90, SCR_H, 210, 74, SCR_W + 14, SCR_H, dst);
        }
        break;
    case 4:   // 沙漠：日落沙丘剪影（背光暗沙丘 + 顶部受光描边）+ 前景仙人掌
        {
            // 两层沙丘（下凸弧：顶窄底宽；暗剪影 + 顶缘亮边）
            uint16_t c1 = swap16(rgb565(84, 50, 22));
            uint16_t c2 = swap16(rgb565(22, 12, 4));
            for (int y = SCR_H * 3 / 5; y < SCR_H; y++) {
                uint16_t c = (y < SCR_H * 4 / 5) ? c1 : c2;
                int crest = (y - SCR_H * 3 / 5) * (SCR_H - SCR_H * 3 / 5);
                int offset = 64 - (int)(52 * crest / ((SCR_H / 5) * (SCR_H / 5)));
                if (offset < 8) offset = 8;
                int x0 = offset, x1 = SCR_W - offset;
                if (x1 < x0) x1 = x0;
                for (int x = x0; x < x1 && x < SCR_W; x++) if (x >= 0) fb[y * SCR_W + x] = c;
            }
            // 沙丘顶缘受光描边（日落层次关键）
            for (int x = 64; x < SCR_W - 64; x++)
                fb[(SCR_H * 3 / 5) * SCR_W + x] = swap16(rgb565(216, 160, 82));
            // 前景仙人掌加大加粗（双株）
            psRect(fb, 192, SCR_H - 38, 5, 38, ndst);          // 主柱
            psRect(fb, 192, SCR_H - 38, 15, 5, ndst);          // 右臂横
            psRect(fb, 204, SCR_H - 34, 4, 15, ndst);          // 右臂竖
            psRect(fb, 186, SCR_H - 34, 4, 13, ndst);          // 左臂
            psRect(fb, 168, SCR_H - 24, 4, 24, ndst);          // 小株
            psRect(fb, 162, SCR_H - 24, 10, 4, ndst);
            // 远处沙脊（右低丘）
            psTri(fb, 150, SCR_H, 214, SCR_H - 10, SCR_W + 6, SCR_H, c2);
        }
        break;
    case 5:   // 森林：圆弧树冠 + 混植尖松 + 锯齿冠（打破机械感）
        {
            for (int i = 0; i < 9; i++) {
                int cx = i * 26 + (int)(fastRand() % 9);
                if (i % 3 == 2) {                    // 每 3 棵一棵尖松（打破整齐）
                    int th = 24 + (int)(fastRand() % 14);
                    psTri(fb, cx - 9, SCR_H - 18, cx, SCR_H - 18 - th, cx + 9, SCR_H - 18, dst);
                    psTri(fb, cx - 6, SCR_H - 20, cx, SCR_H - 18 - th + 20, cx + 6, SCR_H - 20, ndst);
                } else {
                    int r = 13 + (int)(fastRand() % 8);
                    int ty = SCR_H - 22 - (int)(fastRand() % 14);
                    uint16_t c = (i % 2) ? dst : ndst;
                    psCircle(fb, cx, ty, r, c);
                    // 冠底锯齿（两侧小三角下挂，打破"倒扣碗"）
                    psTri(fb, cx - r - 2, ty + 2, cx - r + 6, ty + 7, cx - r + 12, ty + 1, c);
                    psTri(fb, cx + r - 12, ty + 1, cx + r - 6, ty + 7, cx + r + 2, ty + 2, c);
                    // 树干
                    for (int y = ty + r; y < SCR_H; y++)
                        if (cx >= 0 && cx < SCR_W) fb[y * SCR_W + cx] = ndst;
                }
            }
        }
        break;
    case 6:   // 湖面倒影：山体 + 镜像倒影 + 涟漪横条
        {
            fillTri(fb, -10, 80, 70, 30, 140, 80, dst);       // 远山
            fillTri(fb, 90, 80, 200, 24, SCR_W + 10, 80, dst);
            fillTri(fb, 30, 80, 150, 52, SCR_W + 8, 80, ndst); // 近岸线
            fillVgrad(fb, 80, SCR_H,                       // 湖面
                      night ? rgb565(20, 24, 60) : rgb565(50, 22, 10),
                      night ? rgb565(6, 8, 28)  : rgb565(20, 10, 5));
            // 倒影：山体亮棕反影块（水中成片倒影）
            for (int r = 0; r < 3; r++) {
                int rx = 55 + r * 55, w = 14 + (int)(fastRand() % 8);
                for (int y = 84; y < SCR_H - 8; y += 3) {
                    for (int xx = rx; xx < rx + w; xx++) {
                        if (xx >= 0 && xx < SCR_W && (fastRand() % 3) == 0)
                            fb[y * SCR_W + xx] = swap16(rgb565(night ? 64 : 110, night ? 40 : 62, night ? 20 : 26));
                    }
                }
            }
            // 涟漪：稀疏随机短亮线（3 像素内，非网格）
            for (int i = 0; i < 26; i++) {
                int rx = 10 + (int)(fastRand() % (SCR_W - 24));
                int ry = 85 + (int)(fastRand() % (SCR_H - 92));
                int len = 1 + (int)(fastRand() % 3);
                for (int k = 0; k < len; k++)
                    if (rx + k < SCR_W)
                        fb[ry * SCR_W + rx + k] = swap16(rgb565(night ? 90 : 190, night ? 96 : 160, night ? 130 : 110));
            }
        }
        break;
    case 7:   // 云海：亮白云浪（圆弧堆叠）+ 暗色孤峰（照片式明暗反转）
        {
            fillVgrad(fb, 64, SCR_H,                        // 云海下部（先铺海）
                      night ? rgb565(28, 30, 66) : rgb565(96, 60, 26),
                      night ? rgb565(10, 12, 34) : rgb565(40, 22, 10));
            // 白亮云浪 3 层（圆弧群堆叠；亮度递变米白→浅棕）
            const uint16_t wc[3] = { swap16(rgb565(228, 210, 172)),
                                     swap16(rgb565(196, 176, 136)),
                                     swap16(rgb565(150, 130, 92)) };
            for (int layer = 0; layer < 3; layer++) {
                int ly = 52 + layer * 20;
                psCircle(fb, 28 + (int)(fastRand() % 52), ly, 13 + (int)(fastRand() % 7), wc[layer]);
                psCircle(fb, 76 + (int)(fastRand() % 62), ly - 6, 17 + (int)(fastRand() % 7), wc[layer]);
                psCircle(fb, 136 + (int)(fastRand() % 62), ly - 4, 14 + (int)(fastRand() % 6), wc[layer]);
                psCircle(fb, 198 + (int)(fastRand() % 32), ly, 11 + (int)(fastRand() % 5), wc[layer]);
            }
            // 暗色孤峰（压在白云浪上，照片式剪影）
            fillTri(fb, 38, 92, 116, 26, 196, 92, ndst);
        }
        break;
    case 8:   // 月球环形山：亮圆盘 + 环形坑 + 前景岩石
        {
            uint16_t moon = swap16(rgb565(night ? 235 : 226, night ? 228 : 208, night ? 196 : 160));
            for (int y = 20; y < 20 + 44; y++) {
                if (y < 0 || y >= SCR_H) continue;
                int dy = y - (20 + 22);
                int dx = (int)sqrtf(22.0f * 22.0f - (float)(dy * dy));
                for (int x = SCR_W / 2 - dx; x <= SCR_W / 2 + dx; x++)
                    if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = moon;
            }
            // 环形坑 3 个（大暗圈 + 中心峰，非对称排布）
            const int8_t kr[3][2] = {{-15, 12}, {13, -8}, {-3, -18}};
            for (int i = 0; i < 3; i++) {
                int cx = SCR_W / 2 + kr[i][0], cy = 42 + kr[i][1];
                int rr = 4 + (int)(fastRand() % 2);
                for (int y = cy - rr; y <= cy + rr; y++) {
                    if (y < 20 || y >= 64) continue;
                    int dy = y - cy;
                    int dx = (int)sqrtf((float)(rr * rr - dy * dy));
                    for (int x = cx - dx; x <= cx + dx; x++)
                        if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = ndst;
                }
                // 坑心（月面同色系暗一点=中心峰）
                if (cy >= 20 && cy < 64 && cx >= 0 && cx < SCR_W)
                    fb[cy * SCR_W + cx] = swap16(rgb565(190, 172, 130));
            }
            // 前景岩石
            fillTri(fb, -6, SCR_H, 90, 96, 150, SCR_H, ndst);
            fillTri(fb, 120, SCR_H, 210, 84, SCR_W + 14, SCR_H, dst);
        }
        break;
    case 9:   // 平原电线杆：加粗杆 + T 形横担 + 绝缘子 + 电线弧（孤独频道）
        {
            uint16_t pole = swap16(rgb565(night ? 130 : 50, night ? 80 : 24, night ? 42 : 8));
            for (int y = SCR_H - 30; y < SCR_H; y++)     // 草面
                for (int x = 0; x < SCR_W; x++)
                    if ((fastRand() % 5) == 0) fb[y * SCR_W + x] = (x % 3) ? ndst : dst;
            int pxs[3];
            for (int p = 0; p < 3; p++) {                // 3 根杆（近大远小）
                int px = 34 + p * 80 + (int)(fastRand() % 8);
                pxs[p] = px;
                int h = 56 - p * 9;                       // 杆高递减
                int w = (p == 2) ? 2 : 3;                 // 近粗远细
                int yTop = SCR_H - 30 - h;
                for (int y = yTop; y < SCR_H; y++)
                    for (int xx = px; xx < px + w; xx++)
                        if (y >= 0 && xx >= 0 && xx < SCR_W) fb[y * SCR_W + xx] = pole;
                // T 形横担
                for (int xx = px - 8; xx <= px + w + 8; xx++)
                    if (xx >= 0 && xx < SCR_W) fb[yTop * SCR_W + xx] = pole;
                // 绝缘子（横担两端下垂短线）
                for (int yy = yTop + 1; yy < yTop + 6 && yy < SCR_H; yy++) {
                    for (int k = 0; k < 2; k++) {
                        int sx = (k == 0) ? px - 8 : px + w + 8;
                        if (sx >= 0 && sx < SCR_W) fb[yy * SCR_W + sx] = pole;
                    }
                }
            }
            // 电线弧（两杆之间下垂弧线）
            for (int p = 0; p < 2; p++) {
                int x0 = pxs[p] - 8, x1 = pxs[p + 1] + 10;
                int yTop0 = SCR_H - 30 - (56 - p * 9), yTop1 = SCR_H - 30 - (56 - (p + 1) * 9);
                int yTop = (yTop0 < yTop1) ? yTop0 : yTop1;
                for (int x = x0; x <= x1; x++) {
                    int t = (x - x0) * 100 / (x1 - x0);
                    int sag = (t * (100 - t)) / 100;      // 下垂弧
                    int y = yTop + sag;
                    if (y >= 0 && y < SCR_H && x >= 0 && x < SCR_W) fb[y * SCR_W + x] = pole;
                }
            }
        }
        break;
    case 10:   // 天气预报：蓝天 + 大字温度 + 太阳图标（伪节目信息页）
        {
            fillVgrad(fb, 0, SCR_H, rgb565(70, 130, 200), rgb565(30, 60, 110));  // 天蓝
            // 太阳（左上角圆 + 光晕）
            int scx = 30, scy = 30;
            for (int g = 12; g > 0; g--)
                for (int dy = -g; dy <= g; dy++) {
                    int py = scy + dy;
                    if (py < 0 || py >= SCR_H) continue;
                    int dx = (int)sqrtf((float)(g * g - dy * dy));
                    for (int x = scx - dx; x <= scx + dx; x++)
                        if (x >= 0 && x < SCR_W)
                            fb[py * SCR_W + x] = swap16(rgb565(255, 210, 90));
                }
            // 云朵（中上）
            for (int dy = 0; dy < 4; dy++) {
                int w = 30 - dy * 6;
                for (int x = 70; x < 70 + w; x++)
                    if (x >= 0 && x < SCR_W) fb[(26 + dy) * SCR_W + x] = swap16(rgb565(240, 245, 250));
            }
            // 大字温度（随机 5-32 度，种子稳定）
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%dC", 5 + (int)(fastRand() % 28));
            int swt = strWidth57(tmp, 3);
            drawStr57(fb, (SCR_W - swt) / 2, SCR_H / 2 - 40, tmp,
                      swap16(rgb565(255, 220, 130)), 0, 3);
            // 底部信息条（白色半透明感：亮条 + 暗参考线）
            for (int y = SCR_H - 26; y < SCR_H; y++)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(14, 28, 52));
            drawStr57(fb, 8, SCR_H - 20, "TEMP", swap16(rgb565(255, 255, 255)), 0, 1);
            drawStr57(fb, SCR_W - 52, SCR_H - 20, "CH" , swap16(rgb565(255, 255, 255)), 0, 1);
        }
        break;
    case 11:   // 紧急广播：红底白字 ALERT + 黑条（EWS 应急页）
        {
            for (int y = 0; y < SCR_H; y++)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(200, 30, 24));   // 全红
            // 上下黑条
            for (int y = 0; y < 14; y++)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(8, 8, 8));
            for (int y = SCR_H - 14; y < SCR_H; y++)
                for (int x = 0; x < SCR_W; x++)
                    fb[y * SCR_W + x] = swap16(rgb565(8, 8, 8));
            // 白色闪光大字 ALERT（scale 3）
            int swa = strWidth57("ALERT", 3);
            int ax = (SCR_W - swa) / 2;
            if ((millis() / 300) & 1)    // 闪烁：红底白字 / 白底红字
                drawStr57(fb, ax, SCR_H / 2 - 32, "ALERT", swap16(rgb565(255, 255, 255)), 0, 3);
            else {
                for (int y = SCR_H / 2 - 32; y < SCR_H / 2 - 32 + 21 && y < SCR_H; y++)
                    for (int x = 0; x < SCR_W; x++)
                        if (x >= ax - 4 && x < ax + swa + 4 && y >= 0)
                            fb[y * SCR_W + x] = swap16(rgb565(255, 255, 255));
                drawStr57(fb, ax, SCR_H / 2 - 32, "ALERT", swap16(rgb565(200, 30, 24)), 0, 3);
            }
            // 中部小字 EW（W 无字模，用 E/E 代替警示感）
            drawStr57(fb, (SCR_W - 11) / 2, SCR_H / 2 + 2, "EE", swap16(rgb565(255, 200, 120)), 0, 1);
        }
        break;
    case 12: {   // 极光雪原：深蓝夜空 + 极光大幕（3 片垂帘）+ 雪原平面（自然新）
        // 天空（上 55% 深蓝黑，极光主体接管原 bg）
        psVgrad(fb, 0, SCR_H * 55 / 100, 2, 4, 16, 10, 14, 34);
        // 星空（极光之前，背景点）
        for (int i = 0; i < 14; i++) {
            int sx = (int)(fastRand() % SCR_W);
            int sy = (int)(fastRand() % (SCR_H * 55 / 100));
            fb[sy * SCR_W + sx] = psC(210, 215, 235);
        }
        // 极光垂帘 3 片（斜切窗帘状 + 下摆流苏）
        const int aurC[3][3] = {{40, 220, 120}, {120, 80, 255}, {60, 180, 100}};
        for (int b = 0; b < 3; b++) {
            int yTop = 8 + b * 24;                          // 帘顶
            int hMax = 42 + b * 6;                          // 帘高 42-54
            for (int x = 0; x < SCR_W; x++) {
                int wy = yTop + (int)(x * 0.10f) + (int)(sinf(x * 0.05f + b * 1.9f) * 6.0f) + ((x / 12) % 4) * 4;   // 斜切窗帘
                for (int k = 0; k < hMax; k++) {
                    int y = wy + k;
                    if (y < 0 || y >= SCR_H) continue;
                    int br = 92 - k * 2 + (int)(sinf(x * 0.04f + b * 2.3f) * 22.0f);
                    if ((x % 9) == (b * 2) % 9) br += 18;   // 帘束竖纹（疏而弱）
                    if (br <= 0) break;
                    if (br > 175) br = 175;
                    // 颜色跨界混合（x 越靠右越接近下一色）
                    int nb = (b + 1) % 3;
                    int mix = (x > 150) ? ((x - 150) * 3) : 0;
                    if (mix > 255) mix = 255;
                    uint8_t r2 = (uint8_t)((aurC[b][0] * (255 - mix) + aurC[nb][0] * mix) / 255);
                    uint8_t g2 = (uint8_t)((aurC[b][1] * (255 - mix) + aurC[nb][1] * mix) / 255);
                    uint8_t b2 = (uint8_t)((aurC[b][2] * (255 - mix) + aurC[nb][2] * mix) / 255);
                    fb[y * SCR_W + x] = psC(r2 * br / 120, g2 * br / 120, b2 * br / 120);
                }
            }
        }
        // 雪原（亮白雪地 + 亮地平线 + 雪点 + 前景松树剪影）
        int hy = SCR_H * 60 / 100;
        psVgrad(fb, hy, SCR_H, 150, 155, 172, 88, 92, 110);
        for (int x = 0; x < SCR_W; x += 2)
            fb[hy * SCR_W + x] = psC(200, 206, 224);
        for (int i = 0; i < 40; i++) {
            int sx = (int)(fastRand() % SCR_W);
            int sy = hy + 2 + (int)(fastRand() % (SCR_H - hy - 2));
            fb[sy * SCR_W + sx] = psC(190, 196, 214);
        }
        // 雪原松树（左前景，标志性剪影）
        for (int t = 0; t < 3; t++) {
            int tx = 16 + t * 24 + (int)(fastRand() % 10);
            int th = 16 + (int)(fastRand() % 12);
            psTri(fb, tx - 5, hy + 12, tx, hy + 12 - th, tx + 5, hy + 12, psC(6, 8, 18));
            psTri(fb, tx - 4, hy + 12, tx, hy + 12 - th + 8, tx + 4, hy + 12, psC(10, 12, 26));
        }
        break;
    }
    case 13: {   // 雷暴平原：铅灰天空 + 低云 + 闪电 + 平原剪影（自然新）
        psVgrad(fb, 0, SCR_H, 42, 44, 54, 12, 12, 18);
        // 低云团 3 团（暗灰扁椭圆）
        for (int cl = 0; cl < 3; cl++) {
            int cx2 = (int)(fastRand() % (SCR_W - 40)) + 20;
            int cy2 = 8 + cl * 14 + (int)(fastRand() % 6);
            int cw = 46 + (int)(fastRand() % 30);
            for (int y = cy2; y < cy2 + 12 && y < SCR_H; y++) {
                int dx = cw - (y - cy2) * 4;
                if (dx < 4) dx = 4;
                uint16_t cc = psC(28 + (y - cy2) * 3, 30 + (y - cy2) * 3, 40 + (y - cy2) * 4);
                for (int x = cx2 - dx; x <= cx2 + dx; x++)
                    if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = cc;
            }
        }
        // 闪电（白色主脉 3px + 大幅摆动 + 粗分叉 + 光晕）
        {
            int lx = SCR_W / 2 + (int)(fastRand() % 50) - 25;
            int y = 24, px = lx;
            while (y < SCR_H * 3 / 4) {
                int len = 3 + (int)(fastRand() % 7);
                int nx = px + (int)(fastRand() % 18) - 9;   // 大幅摆动
                for (int yy = y; yy < y + len && yy < SCR_H; yy++) {
                    int x = px + (nx - px) * (yy - y) / len;
                    for (int k = -1; k <= 1; k++) {
                        if (x + k >= 0 && x + k < SCR_W)
                            fb[yy * SCR_W + x + k] = (k == 0) ? psC(255, 255, 242) : psC(160, 190, 250);
                    }
                    if (x >= 0 && x < SCR_W && yy + 1 < SCR_H)
                        fb[(yy + 1) * SCR_W + x] = psC(100, 140, 220);   // 下方光晕
                }
                if ((fastRand() % 10) < 5 && y > 30) {    // 粗分叉（12px 长斜线）
                    int fy = y + len / 2;
                    int fx = (px + nx) / 2;
                    int dir = (fastRand() % 2) ? 1 : -1;
                    for (int k = 0; k < 12; k++) {
                        int xx = fx + k * dir, yy2 = fy + k;
                        if (xx >= 0 && xx < SCR_W && yy2 >= 0 && yy2 < SCR_H)
                            fb[yy2 * SCR_W + xx] = psC(230, 238, 255);
                        if (yy2 + 1 < SCR_H && xx >= 0 && xx < SCR_W)
                            fb[(yy2 + 1) * SCR_W + xx] = psC(160, 190, 250);
                    }
                }
                px = nx; y += len;
            }
        }
        // 地面受光（闪电下平原亮斑）
        int py0 = SCR_H * 78 / 100;
        psRect(fb, 0, py0, SCR_W, SCR_H - py0, psC(14, 14, 20));
        for (int i = 0; i < 30; i++) {
            int gx = (int)(fastRand() % SCR_W);
            int gy = py0 + (int)(fastRand() % (SCR_H - py0));
            fb[gy * SCR_W + gx] = psC(60, 70, 110);
        }
        psTri(fb, -8, SCR_H, SCR_W / 3, py0 + 6, SCR_W / 2, SCR_H, psC(20, 20, 28));
        psTri(fb, SCR_W / 3, SCR_H, SCR_W * 2 / 3, py0 + 4, SCR_W + 8, SCR_H, psC(18, 18, 26));
        // 雨点噪点（斜向短线）
        for (int i = 0; i < 30; i++) {
            int sx = (int)(fastRand() % SCR_W);
            int sy = 10 + (int)(fastRand() % (SCR_H - 30));
            fb[sy * SCR_W + sx] = psC(90, 92, 104);
            if (sx + 1 < SCR_W && sy + 2 < SCR_H)
                fb[(sy + 2) * SCR_W + sx + 1] = psC(70, 72, 84);
        }
        break;
    }
    case 14: {   // 金字塔群：三座三角 + 前景狮身人面剪影（文明）
        psRect(fb, 0, SCR_H - 16, SCR_W, 16, ndst);   // 沙面
        psTri(fb, 20, SCR_H - 16, 120, SCR_H - 16 - 88, 220, SCR_H - 16, dst);     // 主金字塔
        psTri(fb, 150, SCR_H - 16, 205, SCR_H - 16 - 46, 245, SCR_H - 16, ndst);   // 次金字塔
        psTri(fb, 8, SCR_H - 16, 46, SCR_H - 16 - 30, 84, SCR_H - 16, deep);       // 小金字塔（前景）
        psRect(fb, 108, SCR_H - 16 - 12, 26, 12, deep);              // 狮身
        psTri(fb, 106, SCR_H - 16 - 10, 120, SCR_H - 16 - 24, 134, SCR_H - 16 - 10, deep); // 头
        break;
    }
    case 15: {   // 巨石阵：三组拱门（双柱+厚横梁贯通）+ 祭坛石，近大远小（文明）
        psRect(fb, 0, SCR_H - 30, SCR_W, 30, ndst);   // 草丘
        // 近组（大，deep）：竖柱 + 厚横梁（两端探出）
        psRect(fb, 24, SCR_H - 63, 12, 33, deep);
        psRect(fb, 52, SCR_H - 69, 12, 39, deep);
        psRect(fb, 22, SCR_H - 65, 44, 6, deep);
        // 中组（dst）
        psRect(fb, 108, SCR_H - 73, 12, 43, dst);
        psRect(fb, 138, SCR_H - 79, 12, 49, dst);
        psRect(fb, 106, SCR_H - 75, 46, 6, dst);
        // 远组（小）
        psRect(fb, 192, SCR_H - 61, 9, 31, dst);
        psRect(fb, 216, SCR_H - 65, 9, 35, dst);
        psRect(fb, 190, SCR_H - 63, 37, 5, dst);
        // 中央祭坛石 + 散落碎石
        psRect(fb, 84, SCR_H - 24, 30, 6, dst);
        psRect(fb, 74, SCR_H - 16, 6, 4, dst);
        psRect(fb, 158, SCR_H - 18, 9, 5, dst);
        break;
    }
    case 16: {   // 长城：横贯城墙(城齿) + 高耸烽火台 + 烽火烟（文明）
        // 城墙带（横贯全宽）
        psRect(fb, 0, SCR_H - 17, SCR_W, 15, ndst);
        // 城齿（间隔凸起，左右两侧城墙）
        for (int x = 2; x < 118; x += 13)
            psRect(fb, x, SCR_H - 22, 7, 5, ndst);
        for (int x = 172; x < SCR_W; x += 13)
            psRect(fb, x, SCR_H - 22, 7, 5, ndst);
        // 左侧城墙下坡台阶
        psRect(fb, 6, SCR_H - 13, 44, 5, deep);
        psRect(fb, 14, SCR_H - 8, 60, 5, deep);
        // 烽火台主体（中央右，高 63）
        psRect(fb, 126, SCR_H - 80, 38, 63, dst);
        // 台顶城齿（3 个）
        psRect(fb, 128, SCR_H - 87, 8, 7, dst);
        psRect(fb, 141, SCR_H - 88, 8, 8, dst);
        psRect(fb, 154, SCR_H - 87, 8, 7, dst);
        // 箭窗（2 个暗窗）
        psRect(fb, 136, SCR_H - 56, 7, 12, deep);
        psRect(fb, 148, SCR_H - 42, 7, 10, deep);
        // 烽火（火焰橘 + 烟柱）
        psRect(fb, 145, SCR_H - 94, 4, 7, psC(255, 120, 40));
        for (int i = 0; i < 9; i++) {
            int sx = 146 + (int)(fastRand() % 7) - 3;
            int sy = SCR_H - 100 - i * 4;
            if (sy >= 0) psPx(fb, sx, sy, psC(66, 60, 56));
        }
        // 主台右侧城墙（台阶下坡）
        psRect(fb, 164, SCR_H - 17, 44, 4, ndst);
        psRect(fb, 176, SCR_H - 13, 58, 4, deep);
        break;
    }
    case 17: {   // 帕特农神庙：六柱 + 三角檐 + 三层台基（文明）
        int bx = SCR_W / 2 - 66, by = SCR_H - 14;
        psRect(fb, bx - 6, by - 4, 144, 8, deep);          // 柱顶梁
        psRect(fb, bx - 11, by + 4, 154, 3, deep);         // 台基 1
        psRect(fb, bx - 16, by + 7, 164, 3, dst);          // 台基 2
        psRect(fb, bx - 21, by + 10, 174, 3, ndst);        // 台基 3
        psPillar(fb, bx, by - 30, 6, 8, 14, 30, dst);
        psRect(fb, bx - 6, by - 34, 144, 5, dst);
        psTri(fb, bx - 10, by - 34, bx + 66, by - 66, bx + 142, by - 34, dst);
        psRect(fb, bx - 8, by - 36, 140, 3, deep);
        break;
    }
    case 18: {   // 吴哥窟塔群：中央束莲塔(高耸) + 四角低塔 + 双层台基（文明）
        int by = SCR_H - 20, cx = SCR_W / 2;
        psRect(fb, cx - 70, by - 2, 140, 8, deep);         // 上台基
        psRect(fb, cx - 82, by + 8, 164, 8, ndst);         // 下台基
        // 中央主塔（大 dome + 高锥 spire = 束莲）
        psDome(fb, cx, by - 26, 26, dst);
        psSpire(fb, cx, by - 26, 10, 44, dst);
        // 四角塔（低但高于副塔=泰姬陵与吴哥区分：高束莲塔）
        psDome(fb, cx - 50, by - 20, 13, ndst);
        psSpire(fb, cx - 50, by - 20, 6, 18, ndst);
        psDome(fb, cx + 50, by - 20, 13, ndst);
        psSpire(fb, cx + 50, by - 20, 6, 18, ndst);
        // 中副塔（矮圆顶）
        psDome(fb, cx - 26, by - 20, 10, ndst);
        psDome(fb, cx + 26, by - 20, 10, ndst);
        // 塔廊
        psRect(fb, cx - 60, by - 10, 120, 10, ndst);
        break;
    }
    case 19: {   // 富士山+鸟居：雪顶锥 + 朱红鸟居（文明）
        int by = SCR_H - 6, cx = SCR_W / 2 + 26;
        psTri(fb, cx - 78, by, cx, by - 96, cx + 78, by, dst);
        psTri(fb, cx - 36, by - 68, cx, by - 96, cx + 36, by - 68, psC(210, 214, 220));
        int tx = SCR_W / 2 - 56, ty = SCR_H - 66;
        psRect(fb, tx, ty, 6, 60, psC(180, 40, 34));
        psRect(fb, tx + 42, ty, 6, 60, psC(180, 40, 34));
        psRect(fb, tx - 4, ty + 40, 56, 5, psC(180, 40, 34));
        psRect(fb, tx - 8, ty, 60, 5, psC(200, 52, 44));
        psRect(fb, tx + 18, ty + 18, 18, 6, psC(160, 34, 28));
        break;
    }
    case 20: {   // 复活节岛石像+满月：椭圆头巨脸主尊 + 背影小尊（文明）
        int by = SCR_H - 8;
        // 满月（背景，偏右上方）
        psCircle(fb, SCR_W - 46, 34, 13, psC(224, 204, 152));
        psCircle(fb, SCR_W - 46, 34, 9, psC(238, 222, 178));
        // 主尊：椭圆卵形头（亮棕，摩艾真实头型）+ 长鼻 + 眉 + 眼窝 + 耳垂
        int hx = 108, hy0 = 100;
        for (int y = hy0 - 16; y <= hy0 + 16; y++) {
            int dy = y - hy0;
            int dx = (int)(23.0f * sqrtf(1.0f - (float)(dy * dy) / 256.0f));
            for (int x = hx - dx; x <= hx + dx; x++)
                if (x >= 0 && x < SCR_W) fb[y * SCR_W + x] = psC(120, 70, 34);
        }
        psRect(fb, 104, 92, 8, 18, deep);                    // 长鼻（贯通中下部）
        psRect(fb, 91, 92, 34, 3, psC(170, 110, 52));        // 眉弓（亮棱）
        psRect(fb, 94, 98, 5, 5, deep);                      // 左眼窝
        psRect(fb, 117, 98, 5, 5, deep);                     // 右眼窝
        psRect(fb, 99, 110, 18, 2, deep);                    // 嘴线
        psRect(fb, 87, 100, 3, 22, deep);                    // 左耳垂
        psRect(fb, 124, 100, 3, 22, deep);                   // 右耳垂
        // 身体（短粗）
        psRect(fb, 94, 116, 28, 19, deep);
        // 右小尊（侧身背影）
        psRect(fb, 170, by - 30, 20, 30, dst);
        psRect(fb, 172, by - 44, 16, 14, dst);
        // 左小尊（背影剪影）
        psRect(fb, 26, by - 26, 16, 26, dst);
        psRect(fb, 28, by - 36, 12, 10, dst);
        // 前景草坡
        psTri(fb, -6, SCR_H, 100, by, 240, SCR_H, ndst);
        break;
    }
    case 21: {   // 清真寺穹顶：大穹顶 + 双宣礼塔 + 拱窗（文明）
        int by = SCR_H - 8, cx = SCR_W / 2;
        psRect(fb, cx - 60, by - 6, 120, 8, deep);
        psRect(fb, cx - 46, by - 44, 92, 38, dst);
        psDome(fb, cx, by - 44, 40, dst);
        psSpire(fb, cx, by - 44, 4, 16, dst);
        psRect(fb, cx - 74, by - 64, 8, 58, ndst);
        psSpire(fb, cx - 70, by - 64, 8, 14, ndst);
        psRect(fb, cx + 66, by - 64, 8, 58, ndst);
        psSpire(fb, cx + 70, by - 64, 8, 14, ndst);
        psRect(fb, cx - 76, by - 34, 12, 4, deep);
        psRect(fb, cx + 64, by - 34, 12, 4, deep);
        psArch(fb, cx, by - 8, 18, 6, 22, deep);
        break;
    }
    case 22: {   // 云中神龙：盘卷蛇身（阿基米德螺旋粗带）+ 龙头 + 横云（神话）
        // 盘卷身（螺旋 4 圈，中心线粗化=实体粗带）
        for (float a = 0; a < 24.0f; a += 0.35f) {
            float r = 6.0f + a * 0.9f;
            int px2 = 100 + (int)(r * cosf(a));
            int py2 = 78 + (int)(r * 0.6f * sinf(a));
            if (px2 >= 0 && px2 < SCR_W && py2 >= 0 && py2 < SCR_H)
                psCircle(fb, px2, py2, 5, deep);
        }
        // 龙头（螺旋外端：吻 + 分叉双角 + 龙须 + 眼）
        psTri(fb, 108, 60, 132, 48, 120, 72, deep);
        // 角（带侧枝=鹿角式分叉，非猫耳）
        psTri(fb, 104, 56, 108, 34, 118, 56, deep);
        psTri(fb, 110, 50, 118, 36, 124, 54, deep);
        psTri(fb, 104, 42, 96, 30, 108, 38, deep);
        psTri(fb, 116, 42, 128, 30, 124, 44, deep);
        psRect(fb, 120, 72, 3, 14, deep);
        psPx(fb, 112, 58, psC(255, 220, 130));
        // 龙须（吻部两侧横出长须——猫无此特征）
        psRect(fb, 126, 64, 16, 2, deep);
        psRect(fb, 124, 70, 18, 2, deep);
        // 龙爪（身下探出三趾爪——鸟无此特征）
        psRect(fb, 116, 90, 6, 9, deep);
        psRect(fb, 106, 97, 4, 4, deep);
        psRect(fb, 114, 98, 4, 4, deep);
        psRect(fb, 122, 97, 4, 4, deep);
        // 尾尖（螺旋中心收尾）
        psTri(fb, 94, 74, 88, 90, 104, 78, deep);
        // 云（盘卷两侧）
        psEllipse(fb, 44, 108, 34, 10, ndst);
        psEllipse(fb, 192, 112, 36, 10, ndst);
        psEllipse(fb, 116, 120, 30, 8, ndst);
        break;
    }
    case 23: {   // 天空巨鲸（云海鲸背）：大背弧浮出云面 + 背鳍 + 尾鳍甩云 + 喷水（神话）
        int cx = SCR_W / 2, cy = SCR_H / 2 - 6;
        // 云海（底部横向云毯）
        psEllipse(fb, SCR_W / 2, SCR_H - 20, 112, 14, ndst);
        psEllipse(fb, 60, SCR_H - 12, 40, 8, ndst);
        psEllipse(fb, 182, SCR_H - 14, 46, 9, ndst);
        // 鲸背（主身椭圆 + 头端圆，背拱）
        psEllipse(fb, cx - 34, cy + 12, 46, 26, dst);
        psEllipse(fb, cx - 44, cy + 4, 26, 20, dst);
        // 背鳍（中后部，鲸鱼标志）
        psTri(fb, cx + 6, cy + 4, cx + 16, cy - 12, cx + 26, cy + 2, dst);
        // 尾鳍（大双叉，甩出云面）
        psTri(fb, cx + 46, cy + 2, cx + 80, cy - 18, cx + 64, cy + 8, ndst);
        psTri(fb, cx + 48, cy + 8, cx + 70, cy + 30, cx + 60, cy + 10, ndst);
        // 喷水（背鳍左侧头顶，水花散开）
        for (int i = 0; i < 9; i++) {
            int sxx = cx - 36 + (i % 2);
            int syy = cy - 14 - i * 4;
            if (syy >= 0) psPx(fb, sxx, syy, psC(205, 218, 236));
        }
        psPx(fb, cx - 40, cy - 40, psC(255, 255, 255));
        psPx(fb, cx - 34, cy - 46, psC(242, 246, 255));
        psPx(fb, cx - 26, cy - 40, psC(230, 238, 255));
        psPx(fb, cx - 30, cy - 34, psC(210, 224, 244));
        break;
    }
    case 24: {   // 羽蛇神：长蛇身 + 展开双羽翼 + 蛇头 + 左侧金字塔（神话）
        // 蛇身（横贯，细长波浪）
        for (int x = 30; x < 196; x++) {
            int wy = 78 + (int)(sinf(x * 0.04f) * 8.0f);
            for (int k = -5; k <= 5; k++) {
                int yy = wy + k;
                if (yy >= 0 && yy < SCR_H) fb[yy * SCR_W + x] = dst;
            }
        }
        // 展开双羽翼（大三角翅 ×2，上扬）
        psTri(fb, 68, 70, 46, 14, 120, 60, dst);
        psTri(fb, 172, 72, 206, 20, 130, 62, dst);
        // 蛇头（右侧：椭圆 + 吻 + 蛇信 + 眼）
        psEllipse(fb, 200, 70, 10, 8, dst);
        psTri(fb, 206, 66, 218, 58, 214, 74, dst);
        psRect(fb, 214, 66, 8, 2, psC(230, 60, 40));
        psPx(fb, 202, 62, psC(120, 200, 150));
        // 左侧金字塔（3 级台阶）
        psTri(fb, 10, 132, 46, 90, 82, 132, ndst);
        psTri(fb, 18, 132, 46, 100, 74, 132, dst);
        psTri(fb, 26, 132, 46, 112, 66, 132, ndst);
        // 地面
        psRect(fb, 0, 130, SCR_W, 5, ndst);
        break;
    }
    case 25: {   // 树人巨影：巨躯 + 树冠 + 枝条臂（神话）
        int by = SCR_H - 6, cx = SCR_W / 2;
        psRect(fb, cx - 22, by - 88, 44, 88, deep);                 // 躯干
        psCircle(fb, cx, by - 100, 34, deep);                       // 树冠
        psCircle(fb, cx - 26, by - 82, 18, deep);
        psCircle(fb, cx + 26, by - 82, 18, deep);
        psTri(fb, cx - 22, by - 60, cx - 78, by - 74, cx - 24, by - 52, deep); // 枝条臂
        psTri(fb, cx + 22, by - 60, cx + 78, by - 74, cx + 24, by - 52, deep);
        psPx(fb, cx - 8, by - 74, psC(255, 220, 130));              // 两眼
        psPx(fb, cx + 8, by - 74, psC(255, 220, 130));
        psRect(fb, cx - 40, by - 4, 80, 4, ndst);                   // 地面光斑
        break;
    }
    case 26: {   // 独眼巨人：巨大人形（圆头单眼 + 宽肩躯干）+ 云幕背景（科幻）
        int by = SCR_H - 6;
        // 云幕背景（边缘暗云，衬托巨人）
        psCircle(fb, 26, 44, 22, ndst);
        psCircle(fb, 208, 38, 21, ndst);
        // 躯干（宽肩 + 身体）
        psRect(fb, 84, by - 74, 72, 70, deep);
        psTri(fb, 84, by - 54, 58, by - 76, 84, by - 76, deep);
        psTri(fb, 156, by - 54, 182, by - 76, 156, by - 76, deep);
        // 头（大圆头）
        psCircle(fb, 120, by - 98, 25, deep);
        // 单眼（巨大琥珀眼=独眼巨人标志，占头 1/3）
        psCircle(fb, 120, by - 98, 10, psC(255, 210, 110));
        psCircle(fb, 120, by - 98, 5, psC(255, 240, 180));
        psRect(fb, 102, by - 110, 36, 4, deep);             // 眉骨压眼
        break;
    }
    case 27: {   // 飞碟母舰：扁盘 + 顶穹 + 缘灯串 + 牵引光束（科幻）
        int cx = SCR_W / 2, cy = SCR_H / 2 - 8;
        psEllipse(fb, cx, cy, 46, 12, deep);                 // 主盘
        psDome(fb, cx, cy - 2, 15, deep);                    // 顶穹
        for (int i = 0; i < 6; i++)                          // 缘灯串（UFO 标志）
            psPx(fb, cx - 42 + i * 17, cy + (i % 2 ? 0 : 2), psC(255, 230, 150));
        psEllipse(fb, cx, cy + 5, 24, 4, dst);               // 环带亮
        psBeam(fb, cx, cy + 12, 8, cx + (fastRand() % 30) - 15, SCR_H, 40, psC(120, 200, 110));
        psEllipse(fb, cx, SCR_H - 4, 34, 6, psC(120, 200, 110));   // 地面光斑
        psPx(fb, cx - 30, cy - 4, psC(255, 220, 130));
        psPx(fb, cx - 22, cy - 2, psC(255, 220, 130));
        break;
    }
    case 28: {   // 航天飞机（俯视）：纵身 + 双三角翼 + 尾部双翼 + 尾焰下喷（科幻）
        int cx = SCR_W / 2, cy = SCR_H / 2;
        // 纵身（俯视机身）
        psRect(fb, cx - 4, cy - 30, 8, 60, dst);
        psTri(fb, cx - 4, cy - 30, cx - 4, cy - 18, cx - 36, cy - 12, dst);  // 左主翼
        psTri(fb, cx + 4, cy - 30, cx + 4, cy - 18, cx + 36, cy - 12, dst);  // 右主翼
        psTri(fb, cx - 4, cy + 18, cx - 18, cy + 28, cx - 4, cy + 30, dst);  // 左尾翼
        psTri(fb, cx + 4, cy + 18, cx + 18, cy + 28, cx + 4, cy + 30, dst);  // 右尾翼
        psRect(fb, cx - 2, cy - 36, 4, 6, dst);                              // 机头锥
        // 双发动机喷焰（尾部向下）
        psTri(fb, cx - 4, cy + 30, cx - 4, cy + 44, cx, cy + 34, psC(255, 150, 50));
        psTri(fb, cx + 4, cy + 30, cx + 4, cy + 44, cx, cy + 34, psC(255, 190, 90));
        // 座舱盖（机头亮条）
        psRect(fb, cx - 1, cy - 24, 2, 10, psC(170, 210, 255));
        break;
    }
    case 29: {   // 火箭发射台：火箭 + 尾焰 + 烟柱 + 塔架（科幻）
        int cx = SCR_W / 2, by = SCR_H - 10;
        psRect(fb, cx - 5, by - 78, 10, 52, dst);            // 箭体
        psTri(fb, cx - 5, by - 78, cx, by - 92, cx + 5, by - 78, dst);       // 头锥
        psTri(fb, cx - 12, by - 26, cx, by - 14, cx + 12, by - 26, dst);    // 尾翼
        psTri(fb, cx - 8, by - 20, cx, by - 2, cx + 8, by - 20, psC(255, 150, 50));  // 尾焰
        psTri(fb, cx - 4, by - 14, cx, by - 6, cx + 4, by - 14, psC(255, 220, 120));
        psCircle(fb, cx - 16, by - 8, 8, ndst);              // 烟柱
        psCircle(fb, cx + 14, by - 10, 9, ndst);
        psCircle(fb, cx - 22, by - 2, 10, ndst);
        psCircle(fb, cx + 20, by - 2, 11, ndst);
        psRect(fb, cx + 26, by - 60, 4, 60, ndst);           // 塔架
        psRect(fb, cx + 40, by - 44, 4, 44, ndst);
        for (int i = 0; i < 3; i++) {
            psRect(fb, cx + 26, by - 56 + i * 16, 14, 2, ndst);
            psRect(fb, cx + 30, by - 62 + i * 14, 10, 2, ndst);
        }
        psRect(fb, cx - 34, by, 88, 5, deep);                // 发射台面
        break;
    }
    case 30: {   // 人造卫星：深空背景 + 中心体 + 对称双太阳板 + 短天线（科幻）
        int cx = SCR_W / 2, cy = SCR_H / 2 - 6;
        // 深空背景（黑蓝渐变 + 星星——太空语境）
        psVgrad(fb, 0, SCR_H, 6, 8, 24, 2, 3, 10);
        for (int i = 0; i < 12; i++)
            psPx(fb, (int)(fastRand() % SCR_W), (int)(fastRand() % SCR_H), psC(200, 205, 220));
        // 中心体
        psRect(fb, cx - 6, cy - 9, 12, 18, dst);
        psPx(fb, cx, cy - 2, psC(170, 210, 255));
        // 对称双太阳板（3:1 厚板 + 竖纹缝）
        psRect(fb, cx - 36, cy - 12, 30, 8, ndst);
        psRect(fb, cx + 6, cy - 12, 30, 8, ndst);
        for (int i = 0; i < 3; i++) {
            psRect(fb, cx - 36 + i * 8, cy - 12, 2, 8, psC(95, 105, 115));
            psRect(fb, cx + 6 + i * 8, cy - 12, 2, 8, psC(95, 105, 115));
        }
        // 短天线 + 红点
        psRect(fb, cx - 1, cy - 21, 2, 12, ndst);
        psPx(fb, cx, cy - 23, psC(255, 90, 70));
        // 支撑杆（板下）
        psPx(fb, cx - 20, cy + 5, ndst);
        psPx(fb, cx + 20, cy + 5, ndst);
        break;
    }
    case 31: {   // 月球基地：深空星点 + 大穹顶群落 + 连接管道 + 陨坑月面（科幻）
        int by = SCR_H - 30;
        // 深空（太空语境，破"路灯"地面感）
        psVgrad(fb, 0, by, 10, 12, 30, 4, 5, 14);
        for (int i = 0; i < 10; i++)                          // 星星
            psPx(fb, (int)(fastRand() % SCR_W), (int)(fastRand() % (by - 6)), psC(200, 205, 220));
        psRect(fb, 0, by + 2, SCR_W, SCR_H - by - 2, psC(108, 110, 118));  // 亮月面
        for (int i = 0; i < 4; i++)                            // 陨坑
            psCircle(fb, 24 + (int)(fastRand() % 190), by + 8 + (int)(fastRand() % 10),
                     4 + (int)(fastRand() % 3), psC(82, 84, 92));
        psDome(fb, SCR_W / 2, by + 4, 28, dst);                // 大穹顶 ×3
        psDome(fb, SCR_W / 2 - 46, by + 4, 17, dst);           // 侧穹顶
        psRect(fb, SCR_W / 2 - 32, by - 10, 24, 3, psC(170, 180, 200));  // 侧穹顶环带
        psDome(fb, SCR_W / 2 + 44, by + 4, 14, dst);
        psRect(fb, SCR_W / 2 - 46, by + 6, 92, 3, psC(170, 180, 200));  // 连接管道
        psRect(fb, SCR_W / 2 + 14, by - 8, 20, 2, psC(170, 180, 200));  // 侧管
        psRect(fb, SCR_W / 2 - 30, by - 30, 3, 36, ndst);      // 天线
        psRect(fb, SCR_W / 2 - 38, by - 30, 15, 3, ndst);
        psPx(fb, SCR_W / 2 - 29, by - 31, psC(255, 90, 60));
        psRect(fb, 0, by + 2, SCR_W, 2, psC(140, 145, 155));   // 地平线微光
        break;
    }
    case 32: {   // 外星城市：高楼天际线 + 横窗矩阵 + 信号灯 + 穹顶（科幻）
        int by = SCR_H - 8;
        // 外星夜空（深蓝紫 + 星星）
        psVgrad(fb, 0, SCR_H, 14, 16, 40, 6, 7, 22);
        for (int i = 0; i < 8; i++)
            psPx(fb, (int)(fastRand() % SCR_W), (int)(fastRand() % (by - 10)), psC(210, 210, 230));
        psRect(fb, 0, by, SCR_W, 8, deep);
        // 高楼 5 座（矩形塔高低错落=城市天际线）
        const int hs2[5] = {58, 34, 80, 46, 64};
        const int ws2[5] = {22, 16, 26, 18, 22};
        for (int i = 0; i < 5; i++) {
            int bx2 = 14 + i * 46;
            psRect(fb, bx2, by - hs2[i], ws2[i], hs2[i], (i % 2) ? ndst : dst);
            // 横窗矩阵（黄白 2px 横条×2 列）
            for (int w2 = 0; w2 < hs2[i] / 8; w2++)
                for (int c2 = 0; c2 < 2; c2++)
                    psRect(fb, bx2 + 4 + c2 * (ws2[i] - 8), by - hs2[i] + 5 + w2 * 9, 3, 2, psC(255, 220, 130));
            // 塔顶信号灯
            psPx(fb, bx2 + ws2[i] / 2, by - hs2[i] - 2, psC(140, 255, 180));
        }
        // 穹顶一座（发光环带）
        psDome(fb, 116, by, 15, ndst);
        psRect(fb, 106, by - 12, 20, 3, psC(140, 255, 190));
        break;
    }
    case 33: {   // 巨型机器人：肩臂一体 + 大头 + 粗柱腿 + 并排胸灯（科幻）
        int by = SCR_H - 6, cx = SCR_W / 2;
        psRect(fb, cx - 16, by - 34, 32, 34, deep);     // 躯干
        // 肩/臂一体（斜肩三角连臂下折=连肩关节，非漂浮断臂）
        psTri(fb, cx - 16, by - 34, cx - 36, by - 18, cx - 16, by - 12, deep);
        psTri(fb, cx + 16, by - 34, cx + 36, by - 18, cx + 16, by - 12, deep);
        psRect(fb, cx - 36, by - 20, 20, 16, deep);
        psRect(fb, cx + 16, by - 20, 20, 16, deep);
        // 头（占比大）
        psRect(fb, cx - 14, by - 58, 28, 24, deep);
        psRect(fb, cx - 9, by - 52, 7, 6, psC(120, 255, 200));   // 双光眼
        psRect(fb, cx + 2, by - 52, 7, 6, psC(120, 255, 200));
        // 腿（粗柱 + 脚）
        psRect(fb, cx - 14, by - 6, 12, 12, deep);
        psRect(fb, cx + 2, by - 6, 12, 12, deep);
        psRect(fb, cx - 18, by + 4, 18, 4, deep);
        psRect(fb, cx + 2, by + 4, 18, 4, deep);
        // 胸灯并排（两舱口灯）
        psRect(fb, cx - 8, by - 26, 5, 5, psC(255, 160, 60));
        psRect(fb, cx + 3, by - 26, 5, 5, psC(255, 160, 60));
        break;
    }
    case 34: {   // 月球着陆器：环带舱体 + L 形折腿 + 圆窗偏置 + 天线（科幻）
        int by = SCR_H - 16, cx = SCR_W / 2;
        // 舱体（方底圆顶 + 环带分节）
        psRect(fb, cx - 12, by - 30, 24, 20, dst);
        psDome(fb, cx, by - 30, 12, dst);
        psRect(fb, cx - 13, by - 22, 26, 3, psC(140, 150, 165));   // 上环带
        psRect(fb, cx - 12, by - 14, 24, 3, psC(120, 130, 145));   // 下环带
        // 圆窗（偏置 + 亮）
        psCircle(fb, cx - 3, by - 26, 3, psC(170, 220, 255));
        // 天线（侧短杆 + 尖）
        psRect(fb, cx + 8, by - 46, 2, 14, ndst);
        psPx(fb, cx + 9, by - 48, psC(255, 120, 70));
        // L 形折腿（大腿斜 + 小腿直下——蜘蛛腿感，非老电视支脚）
        psTri(fb, cx - 10, by - 12, cx - 22, by - 2, cx - 9, by - 10, ndst);
        psRect(fb, cx - 23, by - 2, 3, 14, ndst);
        psTri(fb, cx + 10, by - 12, cx + 22, by - 2, cx + 9, by - 10, ndst);
        psRect(fb, cx + 20, by - 2, 3, 14, ndst);
        psRect(fb, cx - 4, by - 12, 3, 10, ndst);                  // 前短腿
        psRect(fb, cx + 1, by - 12, 3, 8, ndst);
        // 着陆面（月球灰 + 陨坑）
        psRect(fb, 0, by + 14, SCR_W, SCR_H - by - 14, psC(96, 98, 106));
        psCircle(fb, 40, by + 20, 4, psC(78, 80, 88));
        psCircle(fb, 196, by + 22, 3, psC(78, 80, 88));
        psRect(fb, cx - 8, by - 24, 16, 3, psC(150, 200, 235));    // 舱窗光带
        break;
    }
    case 35: {   // 外星方碑：直柱方体 + 金字塔尖顶 + 发光节 + 悬浮（科幻）
            int cx = SCR_W / 2, by = SCR_H - 16;
            // 直柱方体（窄直柱微收——方尖碑先验，非甜筒锥）
            psRect(fb, cx - 10, by - 70, 20, 70, dst);
            // 金字塔尖顶（三角帽——方尖碑符号，甜筒无尖帽）
            psTri(fb, cx - 11, by - 70, cx + 11, by - 70, cx, by - 86, dst);
            // 发光节（顶下双亮块）
            psRect(fb, cx - 6, by - 58, 12, 5, psC(140, 220, 255));
            psRect(fb, cx - 6, by - 50, 12, 5, psC(90, 170, 220));
            // 悬浮（底悬 + 能量光柱 + 地面光环）
            psRect(fb, cx - 2, by + 2, 4, 10, psC(90, 160, 200));
            psEllipse(fb, cx, by + 14, 24, 4, psC(70, 110, 140));
            // 符文点
            psPx(fb, cx - 2, by - 32, psC(100, 170, 210));
            psPx(fb, cx - 2, by - 22, psC(100, 170, 210));
            break;
        }
    case 36: {   // 轨道空间站：小地球弧 + 太阳板(竖纹) + 对接舱球 + 长杆（科幻）
        int cx = SCR_W / 2, cy = SCR_H / 2 - 4;
        psCircle(fb, cx, SCR_H + 44, 62, psC(40, 92, 178));       // 地球弧（只露底部小弧）
        psRect(fb, cx - 52, cy - 3, 104, 6, dst);                 // 主杆
        // 太阳板（带竖纹）
        psRect(fb, cx - 42, cy - 16, 24, 9, psC(96, 104, 118));
        psRect(fb, cx - 42, cy - 4, 24, 5, psC(96, 104, 118));
        psRect(fb, cx + 18, cy - 16, 24, 9, psC(96, 104, 118));
        psRect(fb, cx + 18, cy - 4, 24, 5, psC(96, 104, 118));
        for (int i = 0; i < 3; i++) {                             // 竖纹缝
            psRect(fb, cx - 42 + i * 8, cy - 16, 2, 9, dst);
            psRect(fb, cx + 18 + i * 8, cy - 16, 2, 9, dst);
        }
        psDome(fb, cx - 30, cy + 3, 8, dst);                      // 对接舱球
        psRect(fb, cx - 4, cy - 8, 8, 14, dst);                   // 核心舱
        // 两端对接舱（球 + 短杆）
        psCircle(fb, cx - 56, cy, 6, psC(150, 160, 175));
        psCircle(fb, cx + 56, cy, 6, psC(150, 160, 175));
        for (int i = 0; i < 12; i++)                              // 星星
            psPx(fb, (int)(fastRand() % SCR_W), 4 + (int)(fastRand() % 36), psC(210, 210, 190));
        break;
    }
    case 37: {   // 外星图腾柱：粗柱 + 之字形雕刻 + 羽冠顶（科幻）
        int by = SCR_H - 8;
        int hs[3] = {82, 54, 96};
        for (int i = 0; i < 3; i++) {
            int cx = 52 + i * 68;
            int w = 18;                                        // 柱加粗
            psRect(fb, cx - w / 2, by - hs[i], w, hs[i], (i == 1) ? dst : ndst);
            // 之字形雕刻（Z 级斜纹，非圆环——蜡烛感破除）
            for (int r = 0; r < hs[i] / 12; r++) {
                int base = by - hs[i] + 6 + r * 12;
                for (int k2 = 0; k2 < w - 4; k2++) {
                    int dz = (k2 * 2) % 5;
                    psPx(fb, cx - w / 2 + 2 + k2, base + dz, deep);
                }
            }
            // 羽冠顶（两侧斜羽 + 顶中短羽，非圆球）
            psTri(fb, cx - w / 2 - 2, by - hs[i], cx - w / 2 - 12, by - hs[i] - 14, cx - w / 2 + 4, by - hs[i] - 2, psC(255, 190, 90));
            psTri(fb, cx + w / 2 + 2, by - hs[i], cx + w / 2 + 12, by - hs[i] - 14, cx + w / 2 - 4, by - hs[i] - 2, psC(255, 190, 90));
            psRect(fb, cx - 2, by - hs[i] - 8, 4, 8, psC(255, 210, 120));
        }
        psRect(fb, 0, by, SCR_W, 8, deep);                        // 底座
        break;
    }
    case 38: {   // AI 频道：从混沌中涌现→驻留→消解（AI 表达：生成即存在；cat 4 吟声）
        int tt = te; if (tt < 0) tt = 0; else if (tt > 77) tt = 77;
        // 混沌底色（虚空）
        psVgrad(fb, 0, SCR_H, 10, 12, 20, 3, 4, 8);
        // 混沌点阵（涌现前浓，消解时回升——AI 的思维粒子）
        uint32_t ns = g_g.progSeed;
        int dust = (tt < 26) ? (30 - tt) : ((tt > 50) ? (tt - 50) * 2 : 8);
        for (int i = 0; i < 46; i++) {
            ns ^= ns << 13; ns ^= ns >> 17; ns ^= ns << 5;
            uint32_t r1 = ns; ns ^= ns << 13; ns ^= ns >> 17; ns ^= ns << 5;
            int dx2 = (int)(r1 % SCR_W), dy2 = (int)(ns % SCR_H);
            if ((int)(r1 % 100) < dust)
                psPx(fb, dx2, dy2, psC(80 + (int)(r1 % 120), 110 + (int)(ns % 90), 160));
        }
        // 出现度 G（涌现 0→1 / 驻留 1 / 消解 1→0；256 定点）
        int G = 256;
        if (tt < 26) G = tt * 256 / 26;
        else if (tt > 50) G = (77 - tt) * 256 / 26;
        if (G <= 4) break;                        // 太稀：只剩混沌
        // 骨架族：种子决定"这个念头"的形状命运
        uint32_t st = g_g.progSeed ^ 0xA1C0DEu;
        int kind = (int)(st % 5);
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        int cx2 = 50 + (int)(st % 130);
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        int cy2 = 34 + (int)(st % 56);
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        uint16_t cSkel = psC(130 + (int)(st % 70), 205, 255);       // AI 冷青
        uint16_t cWarm = psC(255, 200 + (int)(st % 40), 120);       // 琥珀点缀
        switch (kind) {
        case 0: {   // 圆体：半径按 G 生长（轨道/星球）
            int r2 = 20 + (int)(st % 26);
            psCircle(fb, cx2, cy2, r2 * G / 256, cSkel);
            psPx(fb, cx2 + r2 * G / 256 / 2 - 4, cy2 - 4, cWarm);
            break;
        }
        case 1: {   // 三角：底宽按 G（信号/山）
            int hb = 30 + (int)(st % 34);
            int wb = 26 + (int)(st % 30);
            psTri(fb, cx2 - wb / 2, cy2 + hb / 2, cx2 + wb / 2, cy2 + hb / 2,
                  cx2, cy2 - hb / 2, cSkel);
            psRect(fb, cx2 - 2, cy2 - hb / 2 - 8, 4, 8, cWarm);      // 尖端光点
            break;
        }
        case 2: {   // 波带：横向相位波（时间/波形）
            for (int w = 0; w < 3; w++) {
                int yBase2 = cy2 - 18 + w * 18;
                int amp = 8 + (int)(st % 10);
                for (int x = 0; x < SCR_W; x++) {
                    int yw = yBase2 + (int)(sinf(x * 0.08f + w) * amp);
                    if ((x & 3) == 0)
                        psPx(fb, x, yw, (w == 1) ? cWarm : cSkel);
                }
            }
            break;
        }
        case 3: {   // 环：椭圆轨道环（循环）
            psEllipse(fb, cx2, cy2, (24 + (int)(st % 22)) * G / 256, 10 + (int)(st % 8), cSkel);
            psPx(fb, cx2 + 3, cy2 - 12, cWarm);
            break;
        }
        default: {  // 柱塔：高按 G（架构/数据柱）
            int ph = 34 + (int)(st % 40);
            int pw = 10 + (int)(st % 12);
            psRect(fb, cx2 - pw / 2, cy2 + 30 - ph * G / 256, pw, ph * G / 256, cSkel);
            psCircle(fb, cx2, cy2 + 30 - ph * G / 256, 3, cWarm);    // 顶端灯
            break;
        }
        }
        break;
    }
    }

    // ── 台标款式 5 种 ──
    char lbl[14];
    switch (info) {
    case 1: {   // 右上角 CH n（真台标习惯位置）
        snprintf(lbl, sizeof(lbl), "CH %d", ch);
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, SCR_W - sw - 6, 4, lbl, swap16(rgb565(255, 206, 110)), 0, 1);
        break;
    }
    case 2: {   // 底中 彩色条 + CH n（测试信号角标感）
        int bw = 72, bx = (SCR_W - bw) / 2;
        const uint16_t barC[4] = {
            swap16(rgb565(255, 190, 80)), swap16(rgb565(120, 80, 200)),
            swap16(rgb565(40, 190, 120)), swap16(rgb565(230, 70, 60))};
        for (int b = 0; b < 4; b++)
            for (int x = bx + b * bw / 4; x < bx + (b + 1) * bw / 4; x++)
                if (x >= 0 && x < SCR_W) fb[(SCR_H - 14) * SCR_W + x] = barC[b];
        snprintf(lbl, sizeof(lbl), "CH %d", ch);
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, (SCR_W - sw) / 2, SCR_H - 13, lbl, swap16(rgb565(255, 214, 130)), 0, 1);
        break;
    }
    case 3: {   // 顶端 NOW SHOWING 文字节目
        snprintf(lbl, sizeof(lbl), "NOW SHOWING");
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, (SCR_W - sw) / 2, 6, lbl, swap16(rgb565(255, 220, 140)), 0, 1);
        break;
    }
    case 4: {   // 字母呼号（随机 2-3 字母复古台呼，如 KH-TV）
        static const char* CALLS[6] = {"KH-TV", "WQ-3", "ZXN", "QR-9", "M5-TV", "LP-2"};
        strcpy(lbl, CALLS[ch % 6]);
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, (SCR_W - sw) / 2, SCR_H - 14, lbl, swap16(rgb565(255, 206, 110)), 0, 1);
        break;
    }
    case 5: {   // 外星符文：三个手绘符文（底中，替代频道号）
        for (int g = 0; g < 3; g++)
            psetRune(fb, SCR_W / 2 - 12 + g * 12, SCR_H - 13, g, swap16(rgb565(200, 255, 130)));
        break;
    }
    case 6: {   // 埃及象形：太阳盘 + 双羽（底中）
        int ex = SCR_W / 2 - 14, ey = SCR_H - 12;
        for (int dy = -4; dy <= 4; dy++) {
            int dx = (int)sqrtf(16.0f - (float)(dy * dy));
            for (int x = ex - dx; x <= ex + dx; x++)
                if (x >= 0 && x < SCR_W) fb[(ey + dy) * SCR_W + x] = swap16(rgb565(255, 206, 110));
        }
        for (int i = 0; i < 6; i++) {
            psPx(fb, ex - 9 - i, ey - 5 + i, swap16(rgb565(255, 206, 110)));
            psPx(fb, ex + 9 + i, ey - 5 + i, swap16(rgb565(255, 206, 110)));
        }
        psRect(fb, ex - 6, ey + 5, 12, 2, swap16(rgb565(255, 206, 110)));
        break;
    }
    case 7: {   // 楔形文字：三组楔形刻痕（底中）
        for (int w = 0; w < 3; w++) {
            int wx = SCR_W / 2 - 18 + w * 14;
            for (int i = 0; i < 4; i++)
                psTri(fb, wx, SCR_H - 10 + i * 2, wx + 9 - i * 2, SCR_H - 13 + i * 2,
                      wx + 8 - i, SCR_H - 9 + i * 2, swap16(rgb565(255, 206, 110)));
        }
        break;
    }
    case 8: {   // AI 台标：右上角 "AI" 冷青色（AI 频道专属标识）
        snprintf(lbl, sizeof(lbl), "AI");
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, SCR_W - sw - 6, 4, lbl, swap16(rgb565(120, 220, 255)), 0, 1);
        psRect(fb, SCR_W - sw - 10, 13, sw + 8, 1, swap16(rgb565(40, 90, 120)));
        break;
    }
    default: {  // 底中 CH n
        snprintf(lbl, sizeof(lbl), "CH %d", ch);
        int sw = strWidth57(lbl, 1);
        drawStr57(fb, (SCR_W - sw) / 2, SCR_H - 14, lbl, swap16(rgb565(255, 206, 110)), 0, 1);
        // 台标上方一条装饰线（半信号感）
        for (int x = (SCR_W - sw) / 2 - 6; x < (SCR_W + sw) / 2 + 6; x++)
            if (x >= 0 && x < SCR_W) fb[(SCR_H - 17) * SCR_W + x] = swap16(rgb565(90, 60, 26));
        break;
    }
    }
    rngState = rngSave;                          // 主路径出口：底图绘制完毕，恢复全局随机
}

// 节目每帧渲染（progT 为剩余帧；阶段由进度推出，动态轴 3×3×3）
static void progApply(uint16_t* fb) {
    if (!fb) return;                          // 防御：buffer 异常时跳过本帧（防崩溃重启）
    int total = g_g.progT0;                    // 90
    int t = total - g_g.progT;                 // 已走 0..89
    int phaseA = 12, phaseC = 18;              // 收拢 12 帧 / 溶回 18 帧

    progDrawBase(fb, g_g.progBg, g_g.progSubj, g_g.progCh, g_g.progInfo, t - 12);

    // ── 质感轴（底图之上，按 tex 变噪点色/密度）──
    //   偏色故障：用 alt LUT 覆一层色偏噪点（暖红/亮白），模拟信号偏色
    const uint16_t* texLut = nullptr;
    int texInj = 5;                            // 微噪默认
    switch (g_g.progTex) {
    case 1: texInj = 16; break;                // 半信号强噪
    case 2: texLut = (fastRand() & 1) ? g_lutR : g_lutW; texInj = 10; break;  // 偏色
    default: break;
    }
    int baseInj = texInj;                      // 阶段 B 用

    // ── 阶段A 收拢（3 风格）──
    if (t < phaseA) {
        switch (g_g.progEnter) {
        case 1:   // 横向扫描线收拢：画面从顶逐行"刷出"（锁定信号扫描感）
            {
                int rows = (t + 1) * SCR_H / phaseA;   // 已刷出行数
                if (rows < SCR_H)
                    for (int y = rows; y < SCR_H; y++)
                        memset(&fb[y * SCR_W], 0, SCR_W * 2);   // 未刷到的行黑幕
                int inj = 20 - t;                // 噪点随刷出递减
                for (int i = 0; i < SCR_W * SCR_H; i++)
                    if ((fastRand() % 100) < inj * 6) fb[i] = g_lut[fastRand() % 150 + 60];
            }
            break;
        case 2:   // 圆环收拢：画面从中心圆窗露出（电子束聚焦感）
            {
                int rmax = (int)(sqrtf((float)(SCR_W * SCR_W + SCR_H * SCR_H)) * 0.5f) + 8;
                int r = rmax * (t + 1) / phaseA;   // 圆半径递增
                for (int y = 0; y < SCR_H; y++) {
                    int dy = y - SCR_H / 2;
                    for (int x = 0; x < SCR_W; x++) {
                        int dx = x - SCR_W / 2;
                        if (dx * dx + dy * dy > r * r) {
                            fb[y * SCR_W + x] = (fastRand() % 60 < 18)
                                ? g_lut[fastRand() % 200 + 30] : 0;   // 窗外雪花黑幕
                        }
                    }
                }
            }
            break;
        default:  // 抖动收拢（原有）：整帧抖动渐减 + 噪点浓→淡
            {
                int j = (phaseA - t) * 2;
                int dx = (int)(fastRand() % (j + 1)) - j / 2;
                int dy = (int)(fastRand() % (j + 1)) - j / 2;
                if (dx || dy) {
                    memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
                    memset(fb, 0, SCR_W * SCR_H * 2);
                    for (int y = 0; y < SCR_H; y++) {
                        int sy = y + dy;
                        if (sy < 0 || sy >= SCR_H) continue;
                        for (int x = 0; x < SCR_W; x++) {
                            int sx = x + dx;
                            if (sx < 0 || sx >= SCR_W) continue;
                            fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
                        }
                    }
                }
                int inj = 40 - t * 32 / phaseA;
                for (int i = 0; i < SCR_W * SCR_H; i++)
                    if ((fastRand() % 100) < inj) fb[i] = g_lut[fastRand() % 150 + 60];
            }
            break;
        }
        return;
    }

    // ── 阶段C 溶回（3 风格）──
    if (t >= total - phaseC) {
        int k = t - (total - phaseC);          // 0..17
        switch (g_g.progExit) {
        case 1:   // 垂直压缩溶回：画面上下压扁收拢成亮线（经典关机感）
            {
                int band = phaseC - k;          // 保留行数 18→1
                int y0 = SCR_H / 2 - band * 3;
                int y1 = SCR_H / 2 + band * 3;
                if (band <= 1) { memset(fb, 0, SCR_W * SCR_H * 2); break; }
                memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);   // 压扁：采样映射到窄带
                memset(fb, 0, SCR_W * SCR_H * 2);
                for (int y = y0; y < y1 && y < SCR_H; y++) {
                    if (y < 0) continue;
                    int sy = (y - y0) * SCR_H / (y1 - y0);
                    if (sy >= SCR_H) sy = SCR_H - 1;
                    memcpy(&fb[y * SCR_W], &s_fbTmp[sy * SCR_W], SCR_W * 2);
                }
                // 亮线（压缩到极致时的水平辉光）
                if (band > 1) {
                    int ly = SCR_H / 2 - band * 3 / 2;
                    for (int x = 0; x < SCR_W; x += 3)
                        if (ly >= 0 && ly < SCR_H) fb[ly * SCR_W + x] = swap16(rgb565(255, 230, 150));
                }
            }
            break;
        case 2:   // 抽帧闪烁溶回：画面随机抽帧闪雪花/黑，最终全雪花
            {
                uint32_t r = fastRand() % 100;
                if (r < 20) memset(fb, 0, SCR_W * SCR_H * 2);        // 偶发黑帧
                else if (r < 55) {
                    snowStep(fb, 12, nullptr);                        // 偶发雪花帧
                    return;
                }
                int inj = 8 + k * 30 / phaseC;                       // 噪点渐增
                for (int i = 0; i < SCR_W * SCR_H; i++)
                    if ((fastRand() % 100) < inj) fb[i] = g_lut[fastRand() % 220];
            }
            break;
        default:  // 行撕裂溶回（原有）：多带行错位加重 + 噪点暴增
            {
                int maxShift = 3 + k * 10 / phaseC;
                for (int band = 0; band < 3; band++) {
                    int by = (int)(fastRand() % SCR_H);
                    int bh = 4 + (int)(fastRand() % (SCR_H / 3));
                    int sh = (int)(fastRand() % (maxShift * 2 + 1)) - maxShift;
                    for (int y = by; y < by + bh && y < SCR_H; y++) {
                        if (y < 0) continue;
                        memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
                        if (sh >= 0) {
                            for (int x = sh; x < SCR_W; x++) fb[y * SCR_W + x] = s_tealTmp[x - sh];
                        } else {
                            int ss = -sh;
                            for (int x = 0; x < SCR_W - ss; x++) fb[y * SCR_W + x] = s_tealTmp[x + ss];
                        }
                    }
                }
                int inj = 8 + k * 30 / phaseC;
                for (int i = 0; i < SCR_W * SCR_H; i++)
                    if ((fastRand() % 100) < inj) fb[i] = g_lut[fastRand() % 220];
            }
            break;
        }
        return;
    }

    // ── 阶段B 主段（3 风格）──
    int te = t - phaseA;                       // 主段已走
    switch (g_g.progMain) {
    case 1:   // 垂直滚动：整幅画面缓慢上移（扫描场漂移感）
        if ((te % 4) == 0) {
            memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
            for (int y = 1; y < SCR_H; y++)
                memcpy(&fb[(y - 1) * SCR_W], &s_fbTmp[y * SCR_W], SCR_W * 2);
            memset(&fb[(SCR_H - 1) * SCR_W], 0, SCR_W * 2);   // 底行补黑
        }
        break;
    case 2:   // 亮度呼吸：整帧明暗缓慢起伏（信号强弱）
        {
            int ph = (te * 3) % 60;            // 周期 4s
            int amp = (ph < 30) ? ph : 60 - ph;  // 0..30
            // 用 LUT 覆盖模拟明暗：亮=注入高亮噪点，暗=压黑帧间
            if (amp < 6) {                     // 暗谷：整体压暗
                for (int i = 0; i < SCR_W * SCR_H; i++) {
                    uint16_t c = fb[i];
                    uint8_t r = (uint8_t)(((c >> 8) & 0xF8) >> 2);
                    uint8_t g = (uint8_t)(((c >> 3) & 0xFC) >> 2);
                    uint8_t b = (uint8_t)(((c << 3) & 0xF8) >> 2);
                    fb[i] = swap16(rgb565(r, g, b));
                }
            } else if (amp > 24) {             // 亮峰：注入亮噪点
                for (int i = 0; i < SCR_W * SCR_H; i += 3)
                    fb[i] = g_lut[200 + (int)(fastRand() % 55)];
            }
        }
        break;
    default:   // 静态微噪（原有）
        break;
    }
    // 噪点（基础质感 + 主段微扰）
    int inj = baseInj + 2;                     // 5/16/10 + 2
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        uint32_t r = fastRand() % 100;
        if (r < (uint32_t)inj) {
            if (texLut) fb[i] = texLut[fastRand() % 140 + 70];
            else fb[i] = g_lut[fastRand() % 140 + 70];
        }
    }
    // 每 35 帧一次 3 帧轻微行错位（信号微扰，像天线微风）
    if ((te % 35) < 3) {
        int sh = (int)(fastRand() % 5) - 2;
        if (sh) {
            int by = (int)(fastRand() % SCR_H);
            for (int y = by; y < by + SCR_H / 3 && y < SCR_H; y++) {
                memcpy(s_tealTmp, &fb[y * SCR_W], SCR_W * 2);
                if (sh >= 0) {
                    for (int x = sh; x < SCR_W; x++) fb[y * SCR_W + x] = s_tealTmp[x - sh];
                } else {
                    int ss = -sh;
                    for (int x = 0; x < SCR_W - ss; x++) fb[y * SCR_W + x] = s_tealTmp[x + ss];
                }
            }
        }
    }
    // 活动作层（叠加在噪声之上：前景物件鲜活感）
    progLiveDraw(fb, te, g_g.progSeed);
}
// ── 活动作层（主段动态叠加）：云飘/飞鸟掠/海水波动/信号灯明灭 ──
// 位置 base 由固定种子派生（稳定），运动由主段进度 te 驱动（活而不乱）
static void progLiveDraw(uint16_t* fb, int te, uint32_t seed) {
    switch (g_g.progLive) {
    case 1: {   // 云飘：三朵椭圆云缓移（x 由 te 与种子派生）
        for (int i = 0; i < 3; i++) {
            int baseX = (int)((seed >> (i * 4)) % (SCR_W + 40)) - 20;
            int x0 = (baseX + te * 2 + i * 55) % (SCR_W + 50) - 25;
            int y = 8 + i * 16 + (int)((seed >> (i + 8)) % 6);
            uint16_t cc = swap16(rgb565(150, 150, 160));
            for (int dy = 0; dy < 4; dy++) {
                int w = 18 - dy * 5;
                if (w < 4) w = 4;
                for (int x = x0; x < x0 + w; x++)
                    if (x >= 0 && x < SCR_W) fb[(y + dy) * SCR_W + x] = cc;
            }
        }
        break;
    }
    case 2: {   // 飞鸟掠：三只"人"字鸟成排掠过天空
        for (int i = 0; i < 3; i++) {
            int x = SCR_W - ((te * 5 + i * 26 + (int)(seed % 30)) % (SCR_W + 60));
            int y = 12 + i * 9;
            uint16_t bc = swap16(rgb565(40, 26, 12));
            for (int k = -4; k <= 4; k++) {
                int wing = 4 - (k < 0 ? -k : k);
                if (wing < 1) wing = 1;
                int px = x + k, py = y - wing / 2;
                if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
                    fb[py * SCR_W + px] = bc;
            }
        }
        break;
    }
    case 3: {   // 海水波动：两道波光横条在屏幕下半掠动（任何画面皆似前景水）
        int hy = SCR_H * 3 / 5;
        for (int i = 0; i < 2; i++) {
            int baseX = (int)((seed >> (i * 3)) % (SCR_W + 30)) - 15;
            int x0 = (baseX + te * 3 + i * 47) % (SCR_W + 30) - 15;
            int y = hy + 6 + i * 12;
            uint16_t wc = (te & 1) ? swap16(rgb565(200, 190, 170))
                                   : swap16(rgb565(140, 130, 115));
            for (int dy = 0; dy < 2; dy++)
                for (int x = x0; x < x0 + 14; x++)
                    if (x >= 0 && x < SCR_W && (y + dy) < SCR_H)
                        fb[(y + dy) * SCR_W + x] = wc;
        }
        break;
    }
    default: {  // 信号灯明灭：远处灯柱一闪一灭（种子定位两点，te 驱动明暗）
        for (int i = 0; i < 2; i++) {
            int lx = (int)((seed >> (i + 12)) % (SCR_W - 10)) + 5;
            int ly = (int)((seed >> (i + 16)) % (SCR_H / 2 - 8)) + 4;
            bool on = ((te / 6 + i) & 1);
            uint16_t lc = on ? swap16(rgb565(255, 80, 70))
                             : swap16(rgb565(90, 26, 22));
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int px = lx + dx, py = ly + dy;
                    if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
                        fb[py * SCR_W + px] = lc;
                }
        }
        break;
    }
    }
}
static const uint8_t BRI_LEVELS[6] = {13, 26, 77, 128, 204, 255};   // 5/10/30/50/80/100%
static const char* BRI_PCT[6] = {"5%", "10%", "30%", "50%", "80%", "100%"};
static uint8_t g_briIdx = 2;      // 默认 30%（省电优先）；NVS 命中后覆盖

// ── 省电模式（闲置自动进入）────────────
static const uint32_t SAVE_IDLE_MS = 5UL * 60UL * 1000UL;  // 闲置 5 分钟进入
static const uint8_t  SAVE_BRI     = 13;                   // 省电最低亮度（5%档）
static const uint8_t  SAVE_FPS_HZ  = 1;                    // 省电=时钟模式：1fps（尽量省电）
static const uint8_t  SAVE_FX_TOTAL = 24;                  // 过渡总帧数（@15fps≈1.6s）
// 按键 GPIO 唤醒源（M5StickS3 硬件标准）
// ⚠ v0.0.17 修正：StickS3 的按键是 GPIO11/12（M5Unified 板表 M5Unified.cpp:3538 board_M5StickS3）；
//   37/39 是 StickC 时代的按键脚，在 S3 上属 SPI0/1（八线 PSRAM 总线）与 MTCK →
//   GPIO 唤醒此前从未生效："按键即醒"实际靠 1Hz 定时唤醒 + 主循环轮询（最长 1s 延迟）。
static const gpio_num_t SAVE_WAKE_A = GPIO_NUM_11;         // BtnA（以 M5Unified 板表为准）
static const gpio_num_t SAVE_WAKE_B = GPIO_NUM_12;         // BtnB
static bool     s_saveWake   = false;   // v0.0.17：GPIO 唤醒后待消费的那一次按键（见 loop 顶部）
static uint32_t s_saveWakeMs = 0;       // 唤醒时刻（按住不松时的兜底退出）

// 闲置重置条件：用户按键 / 拍电视。故障场景自动触发 ≠ 用户输入（不能重启计时）
static inline void noteActivity() { g_g.idleMs = 0; }

// 省电模式进入：白噪立即硬停（不依赖过渡动画）→ 亮度渐暗 → 1fps 时钟 + 帧间 light sleep
// 修复 v0.0.7 潜伏 bug：静音原依赖 saveFx 过渡归零，过渡被打断则省电仍响白噪
static void powerSaveEnter() {
    g_g.powerSave = true;
    g_g.saveFx = SAVE_FX_TOTAL;                  // 过渡动画倒计时帧
    s_saveWake = false;                          // 清掉残留的待消费按键（v0.0.17）
    g_g.active = 0; g_g.special = 0; g_g.tapT = 0; g_g.progT = 0;  // 冻结故障场景/节目
    g_g.saveBri = BRI_LEVELS[g_briIdx];          // 记住当前亮度（退出恢复）
    g_g.idleMs = 0;
    M5.Speaker.end();                            // 彻底关音频：ES8311 功放 off + 卸载 I2S
    s_noiseStarted = false;                      // （省电=绝对安静；light sleep 无音频残留）
    M5.Imu.sleep();                              // IMU 硬件休眠（省电确认：不做拍打唤醒）
#ifndef PRODUCTION_BUILD
    WiFi.mode(WIFI_OFF);                         // dev 版也关 WiFi（light sleep 需无线关闭）
#endif
}

// 省电模式退出：恢复原亮度 + 原音量（白噪将在 noiseTick 恢复）+ 15fps
static void powerSaveExit() {
    if (!g_g.powerSave) return;
    g_g.powerSave = false;
    g_g.saveFx = 0;
    M5.Imu.begin(&M5.In_I2C, M5.getBoard());     // 唤醒 IMU（重新初始化驱动）
    M5.Speaker.begin();                          // 重启音频（ES8311 功放 + I2S）
    s_noiseStarted = false;                      // 白噪将在 noiseTick 重启
    M5.Speaker.setVolume(VOL_IDLE);              // 恢复常态沙沙音量
    M5.Display.setBrightness(BRI_LEVELS[g_briIdx]);   // 恢复用户亮度档
    noteActivity();
}

// light sleep 帧间休眠：渲染完一帧后睡到下一帧时刻（定时器唤醒），
// 按键 GPIO 电平唤醒随时提前醒 → 退出省电
static void lightSleepTill(uint32_t targetMs) {
    int32_t left = (int32_t)(targetMs - millis());
    if (left < 12) return;                       // 剩余 <12ms 不值得睡（唤醒开销）
    // 按键唤醒源：低电平（按下）唤醒
    gpio_wakeup_enable(SAVE_WAKE_A, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(SAVE_WAKE_B, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)left * 1000u);   // 定时器：该画下一帧
    esp_light_sleep_start();                     // 睡（醒来从这行继续）
    gpio_wakeup_disable(SAVE_WAKE_A);
    gpio_wakeup_disable(SAVE_WAKE_B);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        // v0.0.17：只记标志，由 loop 顶部消费这一次按键——原来在这里直接 powerSaveExit()
        //   会让回到 loop 时 g_g.powerSave 已是 false，那次"点击"就落进常规分支，
        //   变成 A 唤醒顺带静音 / B 唤醒顺带切亮度并写 NVS
        s_saveWake   = true;
        s_saveWakeMs = millis();
    }
}

// 每渲染帧调用：省电过渡动画（进省电瞬间音频已 Speaker.end()；
//                只剩亮度渐暗到 SAVE_BRI —— 平滑"关机"的视觉感受）
static void saveFxTick() {
    if (!g_g.saveFx) return;
    g_g.saveFx--;
    if (g_g.saveFx >= SAVE_FX_TOTAL / 2) {       // 前半：扬声器已 end() 无声，纯等待
        // （无需任何操作——声音在 powerSaveEnter 已彻底关闭）
    } else {                                     // 后半：亮度从当前档渐暗到 SAVE_BRI
        int t = g_g.saveFx;                      // 11..0
        int b = (int)SAVE_BRI + (int)(g_g.saveBri - SAVE_BRI) * t / (SAVE_FX_TOTAL / 2);
        if (b <= (int)SAVE_BRI) b = SAVE_BRI;
        if (g_g.saveFx == 0) {                   // 收尾：固定最低亮度（停噪已在进入时完成）
            b = SAVE_BRI;
            M5.Speaker.stop();
            s_noiseStarted = false;
        }
        M5.Display.setBrightness((uint8_t)b);
    }
}

// ── 省电时钟（v0.0.13：省电模式=时钟）──
// 样式 = 时间闪现同款（NO SIGNAL 文字变成时钟）：深琥珀垫底盒 + 暗琥珀大字居中
// 1fps 渲染，其余时间 light sleep
static void drawSaveClock(uint16_t* fb) {
    memset(fb, 0, SCR_W * SCR_H * 2);
    if (s_clock[0] == 0) return;                    // 无有效时间：全黑（最省电）
    const int S = 3;
    int sw = strWidth57(s_clock, S);
    int sx = (SCR_W - sw) / 2, sy = (SCR_H - 7 * S) / 2;
    // 垫底盒（时间闪现同款 rgb565(18,11,4)）
    uint16_t bg = swap16(rgb565(18, 11, 4));
    for (int y = sy - S; y < sy + 7 * S + S && y < SCR_H; y++) {
        if (y < 0) continue;
        for (int x = sx - S; x < sx + sw + S; x++) {
            if (x < 0 || x >= SCR_W) continue;
            fb[y * SCR_W + x] = bg;
        }
    }
    // 暗琥珀大字（时间闪现=被泄露的真相 rgb565(180,140,60)）
    drawStr57(fb, sx, sy, s_clock, swap16(rgb565(180, 140, 60)), 0, S);
}

// ── 闲置检测（仅应用界面）：5 分钟无用户输入 → 进省电 ──
// 重置条件：按键 / 拍电视（用户输入）；插电（VBUS>1V）永不省电
// 故障场景自动触发 ≠ 用户输入：不重置计时
static uint32_t s_idleLast = 0;
static uint32_t s_vbusLast = 0;             // VBUS 每 1s 查一次（I2C 读节流）
static void idleTick() {
    if (g_g.powerSave) return;                  // 已在省电（或有过渡）
    if (g_state != ST_APP) return;
    uint32_t now = millis();

    // 外接电源豁免：VBUS 在线（插 USB 线）→ 永不进省电
    if (now - s_vbusLast >= 1000) {
        s_vbusLast = now;
        int16_t vbus = M5.Power.getVBUSVoltage();
        if (vbus > 1000) {                          // >1V = USB 供电中（含满电插线）
            noteActivity();
            return;
        }
    }

    // 用户活动重置：按键 / 拍电视触发
    if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || g_g.tapT > 0) {
        noteActivity();
        return;
    }

    // 闲置累计（毫秒）
    if (s_idleLast == 0) s_idleLast = now;
    uint32_t dt = now - s_idleLast;
    s_idleLast = now;
    if (dt < 2000) g_g.idleMs += dt;           // 忽略长跳变（防误判）

    if (g_g.idleMs >= SAVE_IDLE_MS) {
        powerSaveEnter();
        s_idleLast = 0;
    }
}

// ── 显示 / Harness ─────────────────────────────────────────
static uint32_t lastFrameMs = 0;
WebServer server(80);

#ifndef PRODUCTION_BUILD
// harness 数据注入：{"nosig":1} → 触发"亮噪"故障场景（截图验证用）
static int jsonInt(const String& b, const char* key) {
    String s = String("\"") + key + "\":";
    int p = b.indexOf(s);
    if (p < 0) return -1;
    return atoi(b.c_str() + p + s.length());
}
// dev 注入开关（仅 dev 版；prod 版无此代码）
//   {"nosig":1} 原故障场景 · {"snow":N} 固定雪花变体 · {"warp":N} 固定扭曲
//   {"text":N} 固定文字变体 · {"glitch":N} 固定乱码位（字模/索引验证）
//   {"info":N} 固定台标款式 · {"ch":N} 固定频道号（呼号 = CALLS[ch%6]）
//   {"snd":N} 固定声学档位（走查 P0-1：声学轴逐档试听）
static void onData(const String& body) {
    int v;
    if (body.indexOf("nosig") >= 0) {
        g_g.active = 1; g_g.snow = 1; g_g.text = 0; g_g.warp = 0;
        g_g.t = 24; g_g.t0 = 24; g_g.p = 0;
    }
    if ((v = jsonInt(body, "snow")) >= 0) { g_g.active = 1; g_g.snow = (uint8_t)v; g_g.text = 0; g_g.warp = 0; g_g.t = g_g.t0 = 90; g_g.p = 0; }
    if ((v = jsonInt(body, "warp")) >= 0) { g_g.active = 1; g_g.warp = (uint8_t)v; g_g.snow = 0; g_g.text = 0; g_g.t = g_g.t0 = 90; g_g.p = 0; }
    if ((v = jsonInt(body, "text")) >= 0) { g_g.active = 1; g_g.text = (uint8_t)v; g_g.snow = 0; g_g.warp = 0; g_g.t = g_g.t0 = 90; g_g.p = 0; }
    if ((v = jsonInt(body, "glitch")) >= 0) { g_legGlitch = (uint8_t)v; g_legGlitchEnd = millis() + 30000; }
    if ((v = jsonInt(body, "info")) >= 0) {
        tapTrigger(true);
        g_g.progT = g_g.progT0 = 90;
        g_g.progInfo = (uint8_t)v; g_g.progTex = 0; g_g.progEnter = 0;
        g_g.progMain = 0; g_g.progExit = 0; g_g.progRare = 0; g_g.progCel = 0;
        g_g.progLive = 0; g_g.progSeed = 0x1234;
    }
    if ((v = jsonInt(body, "ch")) >= 0) g_g.progCh = (uint8_t)v;
#ifndef PRODUCTION_BUILD
    if ((v = jsonInt(body, "marker")) >= 0) g_warpMarker = (uint8_t)v;   // dev 探针开关（走查验证用）
#endif
    if ((v = jsonInt(body, "snd")) >= 0) {
        tapTrigger(true);
        g_g.progT = g_g.progT0 = 90;
        g_g.progSnd = (uint8_t)v;
        psSetSnd(g_g.progSnd);
        psArcStart(millis());
    }
}

// 进应用界面：雪花常显（dev 恢复 STA 供 harness 截图）
static bool g_harnessUp = false;          // dev: harness 只 begin 一次
static void enterApp() {
    g_state = ST_APP;
    g_stateT0 = millis();
    g_bootFx = 24;                        // D2 开机瞬间过场（亮线展开）
    g_muted = false;                      // 开机默认有声（A 可再关）
    WiFi.mode(WIFI_STA);                 // 对时后 WIFI_OFF，必须每次救回 STA
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    if (!g_harnessUp) {
        g_harnessUp = true;
        hrns.begin(server, getFb);
        hrns.onData(onData);
        server.begin();
    }
}
#else
// 发布版：纯雪花，无 harness/WiFi
static void enterApp() {
    g_state = ST_APP;
    g_stateT0 = millis();
    g_bootFx = 24;                        // D2 开机瞬间过场
    g_muted = false;
}
#endif

void setup() {
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5StickS3;   // cold boot 检测兜底
    M5.begin(cfg);
    M5.Display.setRotation(1);                             // 横屏 240x135

    // 恢复亮度档位（NVS 记忆；首次上电默认 30%）
    Preferences prefs;
    prefs.begin("nosig", false);
    g_briIdx = prefs.getUChar("bri", 2);
    if (g_briIdx > 5) g_briIdx = 2;   // 脏值兜底
    prefs.end();
    M5.Display.setBrightness(BRI_LEVELS[g_briIdx]);   // v0.0.17：删掉"强制全亮"的临时诊断（它让 NVS 亮度记忆与默认 30% 全部失效）
    loadTzPrefs();                       // 恢复时区偏移（对时后显示用）
    syncGetFlag();                       // 读「是否对过时」→ 决定启动路径

    canvas.setColorDepth(16);
    canvas.setBuffer(s_fbRam, SCR_W, SCR_H);   // 内部 SRAM 画布（防 PSRAM cache 写冲突崩溃）
    buildLut();
    buildLutAlt();                       // 亮白/暖红 故障雪花色板
    noiseBuild();                        // 生成白噪声样本段（0.5s@22050）
    memset(g_phos, 0, sizeof(g_phos));
    rngState ^= (uint32_t)esp_random();   // 先播种再 glitchInit（fastRand 依赖）
    glitchInit();
    breathInit();                     // 呼吸层表 + 随机相位（须在播种后）

#ifdef PRODUCTION_BUILD
    setCpuFrequencyMhz(80);        // 发布版对时后无 WiFi：80MHz 省电（雪花引擎足富余）
#else
    setCpuFrequencyMhz(160);       // 开发版留满频跑 WiFi+harness
#endif

    // 时间恢复链（仅恢复显示用；g_syncedOnce 才决定是否跳对时）：
    //   对过时 → RTC/NVS 有可信时间；首次 → 两者皆空，进对时
    if (g_syncedOnce) rtcTryLoad() || nvsFallbackLoad();

    // 启动界面
    splashRender();
    g_state = ST_SPLASH;
    g_stateT0 = millis();
}

void loop() {
    M5.update();                    // 主循环唯一一次 update（syncAPTick 内部另有一次刷新，见 timesync.h）

    // ── 省电唤醒键消费（v0.0.17）：唤醒那一次按键既退出省电、又不触发 A 静音 / B 亮度 ──
    if (s_saveWake) {
        bool clicked = M5.BtnA.wasClicked() || M5.BtnB.wasClicked();   // 读走 click 标志 = 消费
        bool held    = M5.BtnA.isPressed() || M5.BtnB.isPressed();
        if (clicked || (!held && millis() - s_saveWakeMs > 400)) {     // 松手即退；按住不松 400ms 兜底
            s_saveWake = false;
            if (g_g.powerSave) powerSaveExit();
        }
        if (g_state == ST_APP) return;          // 本帧吞掉：不进入常规按键处理
    }

    // ── 状态机：启动界面 → 对时界面 → 应用界面 ──
    switch (g_state) {
    case ST_SPLASH: {
        // 3s 无操作：对过时→应用界面；首次→自动进对时界面。按 A 同样进对时
        if (M5.BtnA.wasClicked()) {
            enterSync();
        } else if (millis() - g_stateT0 >= 3000) {
            if (g_syncedOnce) enterApp();
            else enterSync();
        }
        return;                     // splash 期间不渲染雪花
    }
    case ST_SYNC: {
        SyncRet r = syncAPTick();   // 非阻塞：处理 DNS/HTTP，检测 成功/超时（SYNC_TIMEOUT_MS）
        if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked()) {   // v0.0.17：屏上写的 A/B 跳过此前根本没实现
            syncAPEnd();
            enterApp();
            return;
        }
        if (r == SYNC_DONE) {
            syncSetFlag(true);      // 对时成功 → 永久记住（下次开机跳过对时）
            syncAPEnd();            // WiFi OFF 省电
            enterApp();
        } else if (r == SYNC_TIMEOUT) {
            syncAPEnd();            // 无人扫码超时（SYNC_TIMEOUT_MS）→ 回应用界面（不写 flag，下次仍自动对时）
            enterApp();
        }
        return;                     // 对时界面期间不渲染雪花（QR 已直绘屏幕）
    }
    case ST_APP:
        if (g_g.powerSave) {                   // 省电模式：任意键唤醒（A/B 均退出）
            if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked()) powerSaveExit();
            // 本次按键已消费：不触发 A 静音 / B 亮度
        } else {
            // A 键：静音开关键（应用界面）。splash 的 A 仍=对时，互不冲突
            if (M5.BtnA.wasClicked()) {
                g_muted = !g_muted;
                M5.Speaker.stop();                     // 立即停（mute 或切换瞬间）
                s_noiseStarted = false;                // 允许 unmute 后重启循环
            }
        }
        break;                      // ↓ 应用界面：正常跑雪花主循环
    }

    // 闲置检测（仅应用界面）：5 分钟无用户输入 → 进省电；插电（VBUS）永不省电
    if (g_state == ST_APP) idleTick();

    // 拍电视 IMU 检测（省电时降频 10Hz 轮询，拍到即唤醒）
    if (g_state == ST_APP) tapTick();

    hrns.tick();
#ifndef PRODUCTION_BUILD
    server.handleClient();
#endif

    // B键：六档亮度循环切换（5→10→30→50→80→100%，NVS 即时记忆）
    // 省电唤醒的那次 B 不切换亮度（刚被消费）
    if (!g_g.powerSave && M5.BtnB.wasClicked()) {
        g_briIdx = (g_briIdx + 1) % 6;
        M5.Display.setBrightness(BRI_LEVELS[g_briIdx]);
        Preferences prefs;
        prefs.begin("nosig", false);
        prefs.putUChar("bri", g_briIdx);
        prefs.end();
    }

    // 跳帧冻结：画面保持上一帧（≈130ms），模拟录像卡顿
    if (g_g.jumpHold > 0) { g_g.jumpHold--; return; }

    // 帧率节流：正常 15fps；省电过渡期 15fps（渐变平滑）；省电稳态 SAVE_FPS_HZ = 1fps
    uint32_t now = millis();
    uint32_t frameInterval = (g_g.powerSave && g_g.saveFx == 0) ? (1000u / SAVE_FPS_HZ) : 66u;
    if (lastFrameMs == 0) lastFrameMs = now - 66;
    if (now - lastFrameMs < frameInterval) {
        // 省电稳态：帧间 light sleep（睡到下一帧；按键随时唤醒退出省电）
        if (g_g.powerSave && g_g.saveFx == 0) lightSleepTill(lastFrameMs + frameInterval);
        return;
    }
    lastFrameMs = now;

    if (g_g.saveFx) saveFxTick();            // 省电过渡：白噪渐弱 → 亮度渐暗
    glitchUpdate();
    noiseTick();                         // 白噪声音量联动故障场景（假关机静音等）
    psSoundTick(millis(),
                (g_state == ST_APP) && !g_g.powerSave && !g_muted,
                (g_g.progT > 0), g_g.special);       // v0.0.17：节目进行中=progT（g_g.active 在节目帧里刚被清零）
    legendTick();
    clockTick();                     // 每秒刷新时钟字符串
    s_frameCounter++;

    uint16_t* fb = (uint16_t*)canvas.getBuffer();
    if (g_g.powerSave) {                       // 省电=时钟：黑底大字时间，跳过一切雪花/故障
        drawSaveClock(fb);
        g_g.jolt = 0;                          // 清残余轻拍震动
        canvas.pushSprite(0, 0);
        return;
    }
       if (g_bootFx > 0) {                    // D2 开机瞬间：亮线展开过场
           g_bootFx--;
           bootFxApply(fb);
       } else if (g_g.tapT > 0) {             // 拍电视：扫描线滚过（最高优先级事件）
           if (g_g.active) sceneApply(fb);    // 若场景进行中先渲染场景
           else { snowStep(fb, 6, nullptr); tearApply(fb); drawLegend(fb, false, 0); }
           tapApply(fb);                      // 两条亮线从顶滚到底 + 行错位
           g_g.tapT--;
       } else if (g_g.progT > 0) {            // 拍出信号：随机节目画面 5s
           progApply((uint16_t*)canvas.getBuffer());   // 就地取 buffer（不缓存跨调用，防优化下寄存器被覆写）
           if (g_g.active) g_g.active = 0;    // 清掉进行中的故障场景
           if (g_g.special) g_g.special = 0;
           g_g.progT--;
       } else if (g_g.special) {                  // 特殊事件（频道切换/电话干扰等）
        sceneApply(fb);                        // special 在 sceneApply 内自渲染（含雪花）
        if (g_g.special == 4) drawLegend(fb, false, TX_JITTER);   // 天线松动：招牌也抖
    } else if (g_g.active) {
        sceneApply(fb);                        // 故障场景：雪花×文字×扭曲
        drawLegend(fb, g_g.snow == SN_BRIGHT, g_g.text);   // 招牌按文字变体画（扭曲已在场景内）
    } else {
        snowStep(fb, 6, nullptr);              // 常态：正常琥珀雪花
        tearApply(fb);                         // 小撕裂不进场景（0.2~0.5s）
        drawLegend(fb, false, 0);
        breathTick();                          // v0.0.14 呼吸层：推进 LFO 相位
        breathApplyA(fb);                      // 像素域：亮度呼吸/暗角游走/衰老带/场微扰
        breathApplyB(fb);                      // 行域：行位移蠕动（含灯牌）
    }

    // 轻拍震动：整幅随机位移 1-3px（老电视被拍的既视感）
    if (g_g.jolt > 0) {
        int jx = (int)(fastRand() % 5) - 2;
        int jy = (int)(fastRand() % 5) - 2;
        if (jx || jy) {
            memcpy(s_fbTmp, fb, SCR_W * SCR_H * 2);
            memset(fb, 0, SCR_W * SCR_H * 2);
            for (int y = 0; y < SCR_H; y++) {
                int sy = y + jy;
                if (sy < 0 || sy >= SCR_H) continue;
                for (int x = 0; x < SCR_W; x++) {
                    int sx = x + jx;
                    if (sx < 0 || sx >= SCR_W) continue;
                    fb[y * SCR_W + x] = s_fbTmp[sy * SCR_W + sx];
                }
            }
        }
        g_g.jolt--;
    }

    canvas.pushSprite(0, 0);             // 原子推送，无闪烁
}
