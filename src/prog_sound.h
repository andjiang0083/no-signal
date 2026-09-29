// ============================================================
//  prog_sound.h — 声学轴 (no-signal v0.0.16)
//  节目专属声音：盖革计数器为核心的事件音系统。
//  架构：白噪 playRaw 无限循环已占用输出流 → 事件音全部走
//        M5.Speaker.tone() 短音叠加（现状已证明可行：拍打嗡声/
//        换台嗖声/DTMF 均叠加工作）。
//  调度：帧级 psSoundTick(now) 检查事件时间线；泊松间隔
//        （稀 3~8s / 密 0.4~1.2s，每次触发 ±30~40% 抖动防机械感）。
//  档位：按主体 cat 抽取（70% 偏置 / 30% 全随机）：
//        自然=盖革稀/机械嗡 · 文明=盖革稀/SOS · 神话=外星语/SOS/密 · 科幻=密/外星语
//  header-only，自包含（独立 RNG psRand，不依赖 main.cpp 符号）。
// ============================================================
#pragma once
#include <stdint.h>
#include <M5Unified.h>

enum { PS_SILENT = 0, PS_GEIGER_SPARSE, PS_GEIGER_DENSE, PS_ALIEN, PS_MORSE_SOS, PS_HUM_CRACKLE };

// ── 独立 RNG（xorshift32，不串扰主随机流）──
static uint32_t s_psSeed = 0x9E3779B9u;
static inline uint32_t psRand() {
    s_psSeed ^= s_psSeed << 13; s_psSeed ^= s_psSeed >> 17; s_psSeed ^= s_psSeed << 5;
    return s_psSeed;
}

// ── 事件时间线状态 ──
static uint32_t s_psNext = 0;         // 下次事件 millis
static uint32_t s_psT2   = 0xFFFFFFFF;// 二次事件（外星双音第二音/噼啪）
static uint8_t  s_psSnd  = PS_SILENT; // 当前声学档
static uint8_t  s_psMorse = 0;        // SOS 符号索引（0..17 两轮）
static bool     s_psArc = false;      // 拍打锁定音弧进行中
static uint32_t s_psArcEnd = 0, s_psArcNext = 0;

// 短音叠加：master 音量瞬时升-降形成包络（tone 异步播放，音量曲线=短促脉冲）
static void psClick(uint16_t f, uint16_t ms, uint8_t vol) {
    uint8_t old = (uint8_t)M5.Speaker.getVolume();
    M5.Speaker.setVolume(vol);
    M5.Speaker.tone(f, ms, -1, true);
    M5.Speaker.setVolume(old);
}

// 拍打锁定音弧：1.1~2s 密集盖革梭子（200ms 级间隔）
static inline void psArcStart(uint32_t now) {
    s_psArc = true;
    s_psArcEnd  = now + 1100u + (uint32_t)(psRand() % 900);
    s_psArcNext = now + 40u;                       // 立即首响（40ms 内）
}

// 泊松间隔判定：命中返回 true 并安排下次间隔（minMs~maxMs 均匀 + 抖动 0.6~1.4x）
static bool psPoisson(uint32_t minMs, uint32_t maxMs, uint32_t now) {
    if (now < s_psNext) return false;
    uint32_t span = maxMs - minMs;
    uint32_t dt = minMs + (uint32_t)(psRand() % (span + 1));
    dt = dt * (60u + (uint32_t)(psRand() % 81)) / 100u;   // 0.6~1.4x
    if (dt < 60u) dt = 60u;
    s_psNext = now + dt;
    return true;
}

// 按 cat 抽声学档（70% 偏置表 / 30% 全随机 1-5）
static uint8_t psPick(uint8_t cat) {
    uint32_t r = psRand() % 100;
    if (r < 30) return 1 + (uint8_t)(psRand() % 5);        // 30% 无视类别全随机
    switch (cat) {
        case 0:  return (r < 30) ? PS_SILENT : ((r < 80) ? PS_GEIGER_SPARSE : PS_HUM_CRACKLE);  // 自然：静谧/盖革稀/机械嗡
        case 1:  return (r < 30) ? PS_SILENT : ((r < 80) ? PS_GEIGER_SPARSE : PS_MORSE_SOS);    // 文明：静谧/盖革稀/SOS
        case 2:  return (r < 20) ? PS_SILENT : ((r < 50) ? PS_ALIEN : ((r < 80) ? PS_MORSE_SOS : PS_GEIGER_DENSE)); // 神话
        case 4:  return (r < 15) ? PS_SILENT : ((r < 70) ? PS_ALIEN : ((r < 90) ? PS_GEIGER_SPARSE : PS_HUM_CRACKLE)); // AI 频道
        default: return (r < 15) ? PS_SILENT : ((r < 85) ? PS_GEIGER_DENSE : PS_ALIEN);          // 科幻：盖革密/外星语
    }
}

// ★ 节目开始时把抽到的档位推进本模块（main 侧唯一接线点）
//   v0.0.17 走查 P0-1：此前 main 只把档位存进 g_g.progSnd 就没人读，
//   模块内 s_psSnd 永远停在 PS_SILENT → 盖革稀/密/SOS/机械嗡四档全是死代码。
static inline void psSetSnd(uint8_t snd) {
    s_psSnd   = snd;
    s_psNext  = millis() + 300u;      // 首响延迟 300ms：让拍打锁定的调谐音先响
    s_psMorse = 0;
    s_psT2    = 0xFFFFFFFF;
}

// 外星数字语：双音错乱步进（间隔 50~300ms 随机，第二音紧随 20~60ms）
static void alienStep(uint32_t now) {
    if (now < s_psNext) {
        if (now >= s_psT2 && s_psT2 != 0xFFFFFFFF) {       // 第二音（错乱对）
            psClick(500 + (uint16_t)(psRand() % 1000), 60 + (uint16_t)(psRand() % 90), 105);
            s_psT2 = 0xFFFFFFFF;
        }
        return;
    }
    s_psNext = now + 50u + (uint32_t)(psRand() % 251);
    s_psT2   = now + 20u + (uint32_t)(psRand() % 40);
    psClick(400 + (uint16_t)(psRand() % 1100), 90 + (uint16_t)(psRand() % 80), 105);
}

// SOS 摩尔斯：点=800Hz·100ms / 划=1500Hz·300ms / 符号间隙 300ms / 字母间隙 +300ms；两轮(18 符号)后静默
static const uint8_t s_morseSeq[9] = {0, 0, 0, 1, 1, 1, 0, 0, 0};
static void morseStep(uint32_t now) {
    if (now < s_psNext) return;
    if (s_psMorse >= 18) { s_psSnd = PS_SILENT; s_psMorse = 0; return; }
    int idx = (int)(s_psMorse % 9);
    if (s_morseSeq[idx]) { psClick(1500, 300, 100); s_psNext = now + 600u; }   // 划+300 间隙
    else                 { psClick(800, 100, 100);  s_psNext = now + 400u; }   // 点+300 间隙
    if (idx == 2 || idx == 5) s_psNext += 300u;                                // 字母间隙
    if (idx == 8) s_psNext += 500u;                                            // 词尾更长的空隙
    s_psMorse++;
}

// 机械嗡：55Hz 低频 0.9s 间歇（4~11s）+ 半概率跟一串低频噼啪
static void humStep(uint32_t now) {
    if (now < s_psNext) {
        if (now >= s_psT2 && s_psT2 != 0xFFFFFFFF) {
            psClick(300 + (uint16_t)(psRand() % 500), 30 + (uint16_t)(psRand() % 50), 90);
            s_psT2 = (psRand() % 3) ? now + 80u + (uint32_t)(psRand() % 200) : 0xFFFFFFFF;
        }
        return;
    }
    psClick(55, 900, 80);
    s_psNext = now + 4000u + (uint32_t)(psRand() % 7000);
    if (psRand() % 2) s_psT2 = now + 60u + (uint32_t)(psRand() % 300);  // 50% 跟噼啪串
    else              s_psT2 = 0xFFFFFFFF;
}

// 帧级主入口（main 每渲染帧调用一次）：
//   appOk  = ST_APP 且非省电且非静音（由 main 侧计算）
//   active = 节目进行中（g_g.progT > 0）——不是 g_g.active（那是故障场景）
//   special = 特殊事件类型（6=外星灯牌 → 低密度外星语背景）
static inline void psSoundTick(uint32_t now, bool appOk, bool active, uint8_t special) {
    if (!appOk) { s_psSnd = PS_SILENT; s_psArc = false; return; }
    // ① 拍打锁定音弧（最高优先，播完进节目声学轴）
    if (s_psArc) {
        if (now >= s_psArcEnd) { s_psArc = false; }
        else if (now >= s_psArcNext) {
            psClick(900 + (uint16_t)(psRand() % 800), 14 + (uint16_t)(psRand() % 10), 130);
            s_psArcNext = now + 140u + (uint32_t)(psRand() % 130);   // 140~270ms → 音弧内 4~10 响
        }
        return;
    }
    // ② special6 外星灯牌：低密度外星语背景（静置事件）
    if (special == 6 && !active) {
        if (s_psSnd != PS_ALIEN) { s_psSnd = PS_ALIEN; s_psNext = now + 500u; }
        if (psPoisson(1500, 4000, now))
            psClick(500 + (uint16_t)(psRand() % 900), 120 + (uint16_t)(psRand() % 100), 95);
        return;
    }
    // ③ 无节目：静默（盖革停 = "信号再次丢失"）
    if (!active) {
        if (s_psSnd != PS_SILENT) { s_psSnd = PS_SILENT; s_psNext = 0; s_psMorse = 0; s_psT2 = 0xFFFFFFFF; }
        return;
    }
    // ④ 节目声学轴
    switch (s_psSnd) {
    case PS_GEIGER_SPARSE:                     // 稀：3~8s 一声（1800Hz 级短"嗒"）
        if (psPoisson(3000, 8000, now))
            psClick(1400 + (uint16_t)(psRand() % 1000), 12 + (uint16_t)(psRand() % 10), 105);
        break;
    case PS_GEIGER_DENSE:                      // 密：0.4~1.2s 一声（更低更短，辐射强信号感）
        if (psPoisson(400, 1200, now))
            psClick(700 + (uint16_t)(psRand() % 1100), 9 + (uint16_t)(psRand() % 9), 118);
        break;
    case PS_ALIEN:        alienStep(now); break;
    case PS_MORSE_SOS:    morseStep(now); break;
    case PS_HUM_CRACKLE:  humStep(now);  break;
    default: break;
    }
}