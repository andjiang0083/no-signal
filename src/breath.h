// ============================================================
//  breath.h — 呼吸层 (no-signal v0.0.14)
//  层级：事件层(GlitchState/拍打/节目) 之下，内容层(雪花+灯牌) 之上。
//  定位：生命感。事件是"一次性痉挛"，呼吸层是"持续生存状态"——
//  呼吸深度 = 细粒度随机游走（25~70s 换目标），纯净/常态/病态随机穿插，
//  无大块稳态（"健康状态机"已否决：3.5min 无呼吸窗违反连续性初衷）。
//  形态：5 个互质周期慢 LFO + 游走门控 wa + 病态事件（排他）。
//  时序：snowStep → tearApply → drawLegend → breathApplyA → breathApplyB → push
//  暂停：事件/拍打/节目/特殊/时钟模式 分支不调用 → 相位自然冻结，事件后原位继续。
//  病态事件与故障事件天然排他：呼吸只在常态分支跑，两者永不叠加。
//  设计档案见 SPEC.md §2.8；取舍记录：灯牌区亮度调制用 16-bit 近似混合（不豁免），
//  行位移全局含灯牌。省电=时钟模式不跑呼吸（深度睡眠叙事 + 省电优先）。
// ============================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// ── 呼吸轴周期（毫秒）：9s / 17s / 41s / 83s / 197s ──
// 全部互质 → 组合永不回环（延续项目"不重复"DNA）
#define BREATH_N_AXIS 5
static const uint32_t BREATH_PERIOD[BREATH_N_AXIS] = { 9000u, 17000u, 41000u, 83000u, 197000u };

// ── 轴索引 ──
enum { BR_AX_BRIGHT = 0, BR_AX_VIGN = 1, BR_AX_SHIFT = 2, BR_AX_BAND = 3, BR_AX_FIELD = 4 };

// ── 幅度基准（255 定点；用户拍板"夸张些没问题"）──
#define BREATH_A0 48        // 亮度呼吸满幅：暗沉深度 19% (207..255)；纯净保底 25% 幅度
#define BREATH_A1 153       // 暗角满幅：边缘衰减至 40%
#define BREATH_A2 4         // 行位移满幅 ±4px
#define BREATH_A3 56        // 衰老带满幅：带内暗至 78%（中心最深）
#define BREATH_A4 13        // 场相位微扰满幅 ±5%
#define BREATH_VIG_R2 18906 // 屏幕对角线半径² (中心到角 ≈137.5px)
#define BR_WA_SICK 383      // 病态深度：满幅 ×1.5（暗角边缘 ~10%、位移 6px）

// ── 生命节律参数（用户拍板 2026-09-06：随机穿插，不要大块稳态）──
#define BR_PICK_MIN 25000u  // 游走目标重抽间隔 25~70s（细粒度，呼吸感连续）
#define BR_PICK_RNG 45001u
#define BR_ILL_NEXT_MIN 60000u   // 病态事件判定间隔 60~180s，25% 触发
#define BR_ILL_NEXT_RNG 120001u
#define BR_ILL_DUR_MIN 25000u    // 病态爆发时长 25~45s（短促事件，与其他故障事件排他）
#define BR_ILL_DUR_RNG 20001u
#define BR_ILL_TRIGGER 25        // 判定触发率 25%

// ── main.cpp 共享符号 ──
extern uint8_t  g_phos[];       // 磷光亮度层
extern uint16_t g_lut[];        // 琥珀 LUT（已 swap）
extern uint16_t s_fbTmp[];      // 整帧重采样缓冲（64KB）
extern int16_t  s_legX0, s_legY0, s_legX1, s_legY1;   // 灯牌矩形（drawLegend 记录；-1=未记录）

namespace {

const int BW = 240, BH = 135;
const int BN = BW * BH;

// ── 预计算表 ──
static int8_t  s_sinT[256];   // 正弦表 (-127..127)
static uint8_t s_vigC[256];   // 暗角归一曲线: dn(0..255) → 0..255（运行时乘幅度）
static bool    s_tblReady = false;
// 呼吸层独立 RNG（xorshift32，与主引擎解耦；仅表初始化/翻身用）
static uint32_t s_brRng = 0x51F15E4Du;

static inline uint32_t brRand() {
    s_brRng ^= s_brRng << 13;
    s_brRng ^= s_brRng >> 17;
    s_brRng ^= s_brRng << 5;
    return s_brRng;
}

static void breathTblInit() {
    for (int i = 0; i < 256; i++) {
        double a = i * (6.283185307179586 / 256.0);
        s_sinT[i] = (int8_t)(127.0 * sin(a));
        double n = i / 255.0;
        s_vigC[i] = (uint8_t)(255.0 * pow(n, 0.6));
    }
    s_tblReady = true;
}

// ── 非对称波：陡升(0..90) 缓落(90..255) → 呼快吸慢的呼吸曲线（正弦是机械，不对称才是生命）
static inline uint8_t breathWave(uint8_t pp) {
    if (pp < 90) return (uint8_t)((uint32_t)pp * 255u / 90u);
    return (uint8_t)(255u - (uint32_t)(pp - 90) * 255u / 165u);
}

struct Breath {
    uint32_t t0[BREATH_N_AXIS];  // 各轴相位起点 ms
    uint8_t  p[BREATH_N_AXIS];   // 当前相位 0..255
    // 翻身（deep breath）：单轴 30~60s 升幅（40~150s 随缘一次，仅非健康期）
    uint32_t deepNext, deepT0, deepMs;
    uint8_t  deepAxis;
    bool     deepOn;
};
static Breath s_b;

// ── 生命节律：呼吸深度 wa 随机游走 + 病态事件 ──
// 游走：每 25~70s 抽一次新目标（0..255 均匀）→ 纯净(<45)/常态/高呼吸 随机穿插，
//       无大块稳态，呼吸感连续；纯净窗 = 目标自然触底（每次 <1min，不赖着不走）
// 病态：独立事件（60~180s 判定一次、25% 触发）→ 深度强制 383，25~45s 短爆发回落
struct BrHealth {
    int16_t  waCur, waTgt;   // 渐变当前值/目标 0..383
    uint32_t nextPick;       // 下次游走重抽时刻
    bool     ill;            // 病态事件进行中
    uint32_t illUntil;       // 病态结束时刻
    uint32_t illNext;        // 下次病态判定时刻
};
static BrHealth s_h;

// 渐变缓动：每帧 ±6 → 全档 ~7s 过渡（穿插平滑，呼吸连续感不突变）
static void rhythmTick() {
    uint32_t ms = millis();
    // 病态事件（与故障事件天然排他：呼吸只在常态分支跑）
    if (!s_h.ill) {
        if (ms >= s_h.illNext) {
            s_h.illNext = ms + BR_ILL_NEXT_MIN + (brRand() % BR_ILL_NEXT_RNG);
            if ((brRand() % 100) < BR_ILL_TRIGGER) {
                s_h.ill = true;
                s_h.waTgt = BR_WA_SICK;                          // 爆发 ×1.5
                s_h.illUntil = ms + BR_ILL_DUR_MIN + (brRand() % BR_ILL_DUR_RNG);
            }
        }
    } else if (ms >= s_h.illUntil) {
        s_h.ill = false;
        s_h.waTgt = (int16_t)(brRand() % 256);                  // 回落：随机深度继续游走
    }
    // 随机游走：细粒度重抽目标
    if (!s_h.ill && ms >= s_h.nextPick) {
        s_h.nextPick = ms + BR_PICK_MIN + (brRand() % BR_PICK_RNG);
        s_h.waTgt = (int16_t)(brRand() % 256);
    }
    int d = (int)s_h.waTgt - (int)s_h.waCur;
    if (d > 0) s_h.waCur += (d > 6) ? 6 : d;
    else if (d < 0) s_h.waCur += (d < -6) ? -6 : d;
}

// 翻身包络：0..255 单峰（sin bump）
static inline uint8_t breathDeepEnv(uint32_t ms) {
    if (!s_b.deepOn) return 0;
    uint32_t pr = (uint32_t)(ms - s_b.deepMs);
    if (pr >= s_b.deepT0) { s_b.deepOn = false; return 0; }
    uint32_t ph = (pr * 256u) / s_b.deepT0;      // 0..255
    return (uint8_t)(s_sinT[(64 + (int)(ph >> 1)) & 255] + 127);  // 0..254 bump
}

// ── 初始化：预计算表 + 随机相位 + 生命节律（须在 rngState 播种后调用）──
static void breathInit() {
    if (!s_tblReady) breathTblInit();
    uint32_t ms = millis();
    for (int i = 0; i < BREATH_N_AXIS; i++)
        s_b.t0[i] = ms - (uint32_t)(brRand() % BREATH_PERIOD[i]);
    s_b.deepNext = ms + 40000u + (brRand() % 110001u);   // 首次 40~150s
    s_b.deepOn = false;
    // 生命节律：开机从中高呼吸起步（先展示效果），随即进入随机穿插
    s_h.waCur = s_h.waTgt = 220;
    s_h.nextPick = ms + 30000u;
    s_h.ill = false;
    s_h.illNext = ms + BR_ILL_NEXT_MIN + (brRand() % BR_ILL_NEXT_RNG);
}

// ── 每渲染帧推进（毫秒解耦，帧率无关）──
static void breathTick() {
    uint32_t ms = millis();
    for (int i = 0; i < BREATH_N_AXIS; i++) {
        uint32_t d = (ms - s_b.t0[i]) % BREATH_PERIOD[i];
        s_b.p[i] = (uint8_t)((d * 256u) / BREATH_PERIOD[i]);
    }
    rhythmTick();
    // 翻身：呼吸深度足够时（游走/病态中高位）才随缘一次，纯净窗不惊悸
    if (!s_b.deepOn && s_h.waCur > 100 && ms >= s_b.deepNext) {
        s_b.deepOn = true;
        s_b.deepAxis = (uint8_t)(brRand() % BREATH_N_AXIS);
        s_b.deepT0 = 30000u + (brRand() % 30001u);
        s_b.deepMs = ms;
        s_b.deepNext = ms + 40000u + (brRand() % 110001u);
    }
}

// ── 像素域调制（A 阶段）：亮度呼吸 + 暗角游走 + 衰老带 + 场相位微扰 ──
// 灯牌区不豁免：OSD 也参与所有调制（16-bit 近似混合到暗琥珀方向）
// 各轴幅度线性随健康门控 wad（0=纯净 · 255=满幅 · 383=病态×1.5）：
//   亮度呼吸保底 12/255（健康期也活着），其余轴随 wad 全开全灭
static void breathApplyA(uint16_t* fb) {
    uint32_t ms = millis();
    int wad = s_h.waCur;
    if (s_b.deepOn) {
        int env = breathDeepEnv(ms);
        wad += ((190 * env) >> 8);                 // 翻身加深（最多 +190）
        if (wad > BR_WA_SICK) wad = BR_WA_SICK;
    }
    int w0 = breathWave(s_b.p[BR_AX_BRIGHT]);
    int bAmp = 12 + ((36 * (wad > 255 ? 255 : wad)) >> 8);   // 亮度呼吸 12..48（健康保底）
    int kk = 255 - ((bAmp * w0) >> 8);                        // 亮度系数 207..255
    int a1 = (BREATH_A1 * wad) >> 8;              // 暗角幅度（病态 229 → 边缘 ~10%）
    int a3 = (BREATH_A3 * wad) >> 8;              // 衰老带幅度
    int a4 = (BREATH_A4 * wad) >> 8;              // 场微扰幅度
    // 暗角中心游走：椭圆巡游（41s 一圈）
    int cx = 120 + ((s_sinT[(s_b.p[BR_AX_VIGN] + 64) & 255] * 30) >> 7);
    int cy = 67  + ((s_sinT[s_b.p[BR_AX_VIGN]] * 18) >> 7);
    // 衰老带中心（83s 全程漂移一次）
    int by = (int)(((uint32_t)s_b.p[BR_AX_BAND] * (BH + 14)) >> 8) - 7;
    // 场微扰相位
    int offC = (int)(s_b.p[BR_AX_FIELD] * 2 & 255);
    int offR = (int)(s_b.p[BR_AX_FIELD] * 3 & 255);

    for (int y = 0; y < BH; y++) {
        uint16_t* row = &fb[y * BW];
        int dy = y - cy, ryy = dy * dy;
        bool inBand = (y >= by - 7 && y <= by + 7);
        int bandZ = inBand ? (y >= by ? (y - by) : (by - y)) : 0;   // 0..7 带内距中心
        bool inLegY = (y >= s_legY0 && y <= s_legY1);
        for (int x = 0; x < BW; x++) {
            int i = y * BW + x;
            int k = kk;
            if (inLegY && x >= s_legX0 && x <= s_legX1) {
                // 灯牌区（OSD）：16-bit 近似混合（不豁免暗角/带/微扰）
                int dx = x - cx, d2 = dx * dx + ryy;
                int dn = (d2 >= BREATH_VIG_R2) ? 255 : (int)(((uint32_t)d2 * 255u) / BREATH_VIG_R2);
                k = (k * (255 - ((a1 * s_vigC[dn]) >> 8))) >> 8;
                if (inBand) {
                    int wz = 255 - bandZ * 28;
                    k = (k * (255 - ((a3 * wz) >> 8))) >> 8;
                }
                int cf = s_sinT[(x * 3 + offC) & 255], rf = s_sinT[(y * 5 + offR) & 255];
                k = (k * (255 + ((a4 * (cf * rf)) >> 14))) >> 8;
                if (k > 255) k = 255; else if (k < 0) k = 0;   // clamp 双向防截断翻转
                uint16_t c = (uint16_t)((fb[i] << 8) | (fb[i] >> 8));   // swap → 逻辑 RGB565
                int r = (c >> 11) & 31, g2 = (c >> 5) & 63, b = c & 31;
                r = (r * k) >> 8; g2 = (g2 * k) >> 8; b = (b * k) >> 8;
                fb[i] = (uint16_t)(((r & 31) << 11) | ((g2 & 63) << 5) | (b & 31));
                fb[i] = (uint16_t)((fb[i] << 8) | (fb[i] >> 8));       // swap 回
            } else {
                // 雪花区：调制 g_phos 亮度域 + LUT 重映射（有机一体，无叠加膜）
                int dx = x - cx, d2 = dx * dx + ryy;
                int dn = (d2 >= BREATH_VIG_R2) ? 255 : (int)(((uint32_t)d2 * 255u) / BREATH_VIG_R2);
                k = (k * (255 - ((a1 * s_vigC[dn]) >> 8))) >> 8;
                if (inBand) {
                    int wz = 255 - bandZ * 28;
                    k = (k * (255 - ((a3 * wz) >> 8))) >> 8;
                }
                int cf = s_sinT[(x * 3 + offC) & 255], rf = s_sinT[(y * 5 + offR) & 255];
                k = (k * (255 + ((a4 * (cf * rf)) >> 14))) >> 8;
                if (k > 255) k = 255; else if (k < 0) k = 0;   // clamp（同上）
                uint8_t ph = g_phos[i];
                uint8_t nph = (uint8_t)((ph * k) >> 8);
                if (nph != ph) { g_phos[i] = nph; row[x] = g_lut[nph]; }
            }
        }
    }
}

// ── 行域重采样（B 阶段）：行位移蠕动（磁场轻拉，灯牌也蠕动——全局）──
static void breathApplyB(uint16_t* fb) {
    int amp = (BREATH_A2 * (int)s_h.waCur + 127) >> 8;   // 0..6px（纯净≈0，病态 6px）
    if (s_b.deepOn && s_b.deepAxis == BR_AX_SHIFT) {
        int env = breathDeepEnv(millis());
        amp = (amp * (255 + ((153 * env) >> 8))) >> 8;             // 翻身 ×1.0~1.6
    }
    if (amp == 0) return;
    int off = (int)((uint32_t)(s_b.p[BR_AX_SHIFT] * 2) & 255);     // 相位慢转（83s）
    memcpy(s_fbTmp, fb, BN * 2);
    for (int y = 0; y < BH; y++) {
        int sh = (amp * s_sinT[(y * 6 + off) & 255]) >> 7;         // -amp..amp
        if (sh == 0) continue;
        uint16_t* src = &s_fbTmp[y * BW];
        uint16_t* dst = &fb[y * BW];
        if (sh > 0) {
            for (int x = BW - 1; x >= sh; x--) dst[x] = src[x - sh];
            for (int x = 0; x < sh; x++) dst[x] = src[BW - sh + x];  // 左缘 wrap
        } else {
            int ns = -sh;
            for (int x = 0; x < BW - ns; x++) dst[x] = src[x + ns];
            for (int x = BW - ns; x < BW; x++) dst[x] = src[x - (BW - ns)];
        }
    }
}

} // namespace